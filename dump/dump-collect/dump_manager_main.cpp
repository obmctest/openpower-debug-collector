#include "dump_manager.hpp"

#include <sdbusplus/bus.hpp>

//The D-Bus root of openpower-dump-collector
constexpr auto OP_DUMP_OBJPATH = "/org/openpower/dump";

// The bus name of openpower-dump-collector
constexpr auto OP_DUMP_BUSNAME = "org.open_power.Dump.Manager";

int main()
{
    auto bus = sdbusplus::bus::new_default();

    // Add sdbusplus ObjectManager for the 'root' path of the DUMP manager.
    sdbusplus::server::manager::manager objManager(bus, OP_DUMP_OBJPATH);
    bus.request_name(OP_DUMP_BUSNAME);

    openpower::dump::Manager mgr(bus, OP_DUMP_OBJPATH);

    while (true)
    {
        bus.process_discard();
        bus.wait();
    }
    exit(EXIT_SUCCESS);
}
