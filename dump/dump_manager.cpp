extern "C"
{
#include <libpdbg_sbe.h>
}

#include "config.h"

#include "dump_collect.hpp"
#include "dump_manager.hpp"
#include "dump_utils.hpp"
#include "sbe_consts.hpp"

#include <fmt/core.h>
#include <libphal.H>
#include <sys/wait.h>
#include <unistd.h>

#include <phosphor-logging/elog-errors.hpp>
#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Common/File/error.hpp>
#include <xyz/openbmc_project/Common/error.hpp>
#include <xyz/openbmc_project/Dump/Create/error.hpp>

#include <chrono>
#include <fstream>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

namespace openpower
{
namespace dump
{
using namespace phosphor::logging;
using InternalFailure =
    sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;

constexpr auto DUMP_CREATE_IFACE = "xyz.openbmc_project.Dump.Create";
constexpr auto ERROR_DUMP_DISABLED =
    "xyz.openbmc_project.Dump.Create.Error.Disabled";
constexpr auto ERROR_DUMP_QUOTA_EXCEEDED =
    "xyz.openbmc_project.Dump.Create.Error.QuotaExceeded";
constexpr auto ERROR_DUMP_NOT_ALLOWED =
    "xyz.openbmc_project.Common.Error.NotAllowed";
constexpr auto OP_SBE_FILES_PATH = "plat_dump";
constexpr auto DUMP_NOTIFY_IFACE = "xyz.openbmc_project.Dump.NewDump";
constexpr auto DUMP_PROGRESS_IFACE = "xyz.openbmc_project.Common.Progress";
constexpr auto STATUS_PROP = "Status";
constexpr auto MAX_ERROR_LOG_ID = 0xFFFFFFFF;
constexpr auto INVALID_FAILING_UNIT = 0xFF;

// Maximum 32 processors are possible in a system.
constexpr auto MAX_FAILING_UNIT = 0x20;

/* @struct DumpTypeInfo
 * @brief to store basic info about different dump types
 */
struct DumpTypeInfo
{
    std::string dumpPath;           // D-Bus path of the dump
    std::string dumpCollectionPath; // Path were dumps are stored
};

/* Map of dump type to the basic info of the dumps */
std::map<uint8_t, DumpTypeInfo> dumpInfo = {
    {SBE::SBE_DUMP_TYPE_HOSTBOOT,
     {HB_DUMP_DBUS_OBJPATH, HB_DUMP_COLLECTION_PATH}},
    {SBE::SBE_DUMP_TYPE_HARDWARE,
     {HW_DUMP_DBUS_OBJPATH, HW_DUMP_COLLECTION_PATH}},
    {SBE::SBE_DUMP_TYPE_SBE,
     {SBE_DUMP_DBUS_OBJPATH, SBE_DUMP_COLLECTION_PATH}}};

/* @struct DumpData
 * @brief To store the data for notifying the status of dump
 */
struct DumpData
{
    uint32_t id;
    uint8_t type;
    std::string pathStr;
    DumpData(uint32_t id, uint8_t type, std::string pathStr) :
        id(id), type(type), pathStr(pathStr)
    {}
};

static int callback(sd_event_source*, const siginfo_t* si, void* data)
{
    using namespace phosphor::logging;
    using InternalFailure =
        sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;
    DumpData* dumpData = reinterpret_cast<DumpData*>(data);
    log<level::INFO>(
        fmt::format("Updating status of path({})", dumpData->pathStr).c_str());
    auto bus = sdbusplus::bus::new_system();
    try
    {
        if (si->si_status == 0)
        {
            log<level::INFO>("Dump collected, initiating packaging");
            auto dumpManager = util::getService(
                bus, DUMP_NOTIFY_IFACE, dumpInfo[dumpData->type].dumpPath);
            auto method = bus.new_method_call(
                dumpManager.c_str(), dumpInfo[dumpData->type].dumpPath.c_str(),
                DUMP_NOTIFY_IFACE, "Notify");
            method.append(static_cast<uint32_t>(dumpData->id),
                          static_cast<uint64_t>(0));
            bus.call_noreply(method);
        }
        else
        {
            log<level::ERR>("Dump collection failed, updating status");
            util::setProperty(
                DUMP_PROGRESS_IFACE, STATUS_PROP, dumpData->pathStr, bus,
                std::variant<std::string>("xyz.openbmc_project.Common.Progress."
                                          "OperationStatus.Failed"));
        }
    }
    catch (const InternalFailure& e)
    {
        commit<InternalFailure>();
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>(
            fmt::format(
                "Unable to update the dump status, errorMsg({}) path({})",
                e.what(), dumpData->pathStr)
                .c_str());
        commit<InternalFailure>();
    }
    delete dumpData;
    return 0;
}

sdbusplus::message::object_path
    Manager::createDump(util::DumpCreateParams params)
{
    using namespace phosphor::logging;
    using InvalidArgument =
        sdbusplus::xyz::openbmc_project::Common::Error::InvalidArgument;
    using CreateParameters =
        sdbusplus::com::ibm::Dump::server::Create::CreateParameters;
    using Argument = xyz::openbmc_project::Common::InvalidArgument;

    auto iter = params.find(
        sdbusplus::com::ibm::Dump::server::Create::
            convertCreateParametersToString(CreateParameters::DumpType));
    if (iter == params.end())
    {
        log<level::ERR>("Required argument, dump type is not passed");
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("DUMP_TYPE"),
                              Argument::ARGUMENT_VALUE("MISSING"));
    }
    std::string dumpType = std::get<std::string>(iter->second);

    iter = params.find(
        sdbusplus::com::ibm::Dump::server::Create::
            convertCreateParametersToString(CreateParameters::ErrorLogId));
    if (iter == params.end())
    {
        log<level::ERR>("Required argument, error log id is not passed");
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("ERROR_LOG_ID"),
                              Argument::ARGUMENT_VALUE("MISSING"));
    }

    // get error log id
    uint64_t errorId = 0;
    try
    {
        errorId = std::get<uint64_t>(iter->second);
    }
    catch (const std::bad_variant_access& e)
    {
        // Exception will be raised if the input is not uint64
        auto err = errno;
        log<level::ERR>(fmt::format("An ivalid error log id is passed, setting "
                                    "as 0, errorMsg({}), errno({}), error({})",
                                    e.what(), err, strerror(err))
                            .c_str());
        report<InvalidArgument>(Argument::ARGUMENT_NAME("ERROR_LOG_ID"),
                                Argument::ARGUMENT_VALUE("INVALID INPUT"));
    }

    if (errorId > MAX_ERROR_LOG_ID)
    {
        // An error will be logged if the error log id is larger than maximum
        // value and set the error log id as 0.
        log<level::ERR>(fmt::format("Error log id is greater than maximum({}) "
                                    "length, setting as 0, errorid({})",
                                    MAX_ERROR_LOG_ID, errorId)
                            .c_str());
        report<InvalidArgument>(
            Argument::ARGUMENT_NAME("ERROR_LOG_ID"),
            Argument::ARGUMENT_VALUE(std::to_string(errorId).c_str()));
    }

    // Make it 8 char length string.
    std::stringstream ss;
    ss << std::setw(8) << std::setfill('0') << std::hex << errorId;
    std::string elogId = ss.str();

    uint8_t type = 0;
    uint64_t failingUnit = INVALID_FAILING_UNIT;

    if (dumpType == "com.ibm.Dump.Create.DumpType.Hostboot")
    {
        type = SBE::SBE_DUMP_TYPE_HOSTBOOT;
    }
    else if (dumpType == "com.ibm.Dump.Create.DumpType.Hardware")
    {
        type = SBE::SBE_DUMP_TYPE_HARDWARE;
    }
    else if (dumpType == "com.ibm.Dump.Create.DumpType.SBE")
    {
        type = SBE::SBE_DUMP_TYPE_SBE;
    }
    else
    {
        log<level::ERR>(fmt::format("Invalid dump type passed dumpType({})",
                                    dumpType.c_str())
                            .c_str());
        elog<InvalidArgument>(Argument::ARGUMENT_NAME("DUMP_TYPE"),
                              Argument::ARGUMENT_VALUE(dumpType.c_str()));
    }

    sdbusplus::message::object_path newDumpPath;
    if ((type == SBE::SBE_DUMP_TYPE_HARDWARE) ||
        (type == SBE::SBE_DUMP_TYPE_SBE))
    {
        iter = params.find(sdbusplus::com::ibm::Dump::server::Create::
                               convertCreateParametersToString(
                                   CreateParameters::FailingUnitId));
        if (iter == params.end())
        {
            log<level::ERR>("Required argument, failing unit id is not passed");
            elog<InvalidArgument>(Argument::ARGUMENT_NAME("FAILING_UNIT_ID"),
                                  Argument::ARGUMENT_VALUE("MISSING"));
        }

        try
        {
            failingUnit = std::get<uint64_t>(iter->second);
        }
        catch (const std::bad_variant_access& e)
        {
            // Exception will be raised if the input is not uint64
            auto err = errno;
            log<level::ERR>(
                fmt::format("An invalid failing unit id is passed "
                            "errorMsg({}), errno({}), errorString({})",
                            e.what(), err, strerror(err))
                    .c_str());
            elog<InvalidArgument>(Argument::ARGUMENT_NAME("FAILING_UNIT_ID"),
                                  Argument::ARGUMENT_VALUE("INVALID INPUT"));
        }

        if (failingUnit > MAX_FAILING_UNIT)
        {
            log<level::ERR>(fmt::format("Invalid failing uint id: greater than "
                                        "maximum number({}): input({})",
                                        failingUnit, MAX_FAILING_UNIT)
                                .c_str());
            elog<InvalidArgument>(
                Argument::ARGUMENT_NAME("FAILING_UNIT_ID"),
                Argument::ARGUMENT_VALUE(std::to_string(failingUnit).c_str()));
        }
    }

    try
    {
        // Pass empty create parameters since no additional parameters
        // are needed.
        util::DumpCreateParams createDumpParams;
        auto dumpManager =
            util::getService(bus, DUMP_CREATE_IFACE, dumpInfo[type].dumpPath);

        auto method = bus.new_method_call(dumpManager.c_str(),
                                          dumpInfo[type].dumpPath.c_str(),
                                          DUMP_CREATE_IFACE, "CreateDump");
        method.append(createDumpParams);
        auto response = bus.call(method);
        response.read(newDumpPath);
    }
    catch (const sdbusplus::exception::SdBusError& e)
    {
        log<level::ERR>(
            fmt::format("D-Bus call exception, errorMsg({})", e.what())
                .c_str());
        if (e.name() == ERROR_DUMP_DISABLED)
        {
            elog<sdbusplus::xyz::openbmc_project::Dump::Create::Error::
                     Disabled>();
        }
        if (e.name() == ERROR_DUMP_QUOTA_EXCEEDED)
        {
            using DumpQuotaExceeded = sdbusplus::xyz::openbmc_project::Dump::
                Create::Error::QuotaExceeded;
            using Reason =
                xyz::openbmc_project::Dump::Create::QuotaExceeded::REASON;
            elog<DumpQuotaExceeded>(Reason(e.description()));
        }
        if (e.name() == ERROR_DUMP_NOT_ALLOWED)
        {
            using DumpNotAllowed =
                sdbusplus::xyz::openbmc_project::Common::Error::NotAllowed;
            using Reason = xyz::openbmc_project::Common::NotAllowed::REASON;
            elog<DumpNotAllowed>(Reason(e.description()));
        }
        else
        {
            // re-throw exception
            throw;
        }
    }

    // DUMP Path format /xyz/openbmc_project/dump/<dump_type>/entry/<id>
    std::string pathStr = newDumpPath;
    auto pos = pathStr.rfind("/");
    if (pos == std::string::npos)
    {
        log<level::ERR>(
            fmt::format("Invalid dump path, path({})", pathStr).c_str());
        elog<InternalFailure>();
    }

    auto idString = pathStr.substr(pos + 1);
    auto id = std::stoi(idString);

    // Initiating a BMC dump
    log<level::INFO>(
        fmt::format("Initiating a BMC dump for host dump({})", pathStr)
            .c_str());
    openpower::dump::util::requestBMCDump();

    pid_t pid = fork();
    if (pid == 0)
    {
        std::filesystem::path dumpPath(dumpInfo[type].dumpCollectionPath);
        dumpPath /= std::to_string(id);
        dumpPath /= OP_SBE_FILES_PATH;

        util::prepareCollection(dumpPath, elogId);
        execl("/usr/bin/dump-collect", "dump-collect", "--type",
              std::to_string(type).c_str(), "--id", std::to_string(id).c_str(),
              "--path", dumpPath.c_str(), "--failingunit",
              std::to_string(failingUnit).c_str(), (char*)0);
        log<level::ERR>(
            fmt::format("Failed to start collection error({})", errno).c_str());
        std::exit(EXIT_FAILURE);
    }
    else if (pid < 0)
    {
        // Fork failed
        log<level::ERR>("Failure in fork call");
        elog<InternalFailure>();
    }
    else
    {
        using InternalFailure =
            sdbusplus::xyz::openbmc_project::Common::Error::InternalFailure;
        log<level::ERR>(
            fmt::format(
                "Adding handler for id({}), type({}), path({}), pid({})", id,
                type, pathStr, pid)
                .c_str());
        DumpData* data = new DumpData(id, type, pathStr);
        auto rc =
            sd_event_add_child(eventLoop.get(), nullptr, pid,
                               WEXITED | WSTOPPED, callback, (void*)(data));
        if (0 > rc)
        {
            // Failed to add to event loop
            log<level::ERR>(
                fmt::format(
                    "Error occurred during the sd_event_add_child call, rc({})",
                    rc)
                    .c_str());
            elog<InternalFailure>();
        }
    }
    // return object path
    return newDumpPath;
}
} // namespace dump
} // namespace openpower
