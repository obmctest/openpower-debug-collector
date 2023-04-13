# SPDX-License-Identifier: Apache-2.0
cxx = meson.get_compiler('cpp')

sdeventplus_dep = dependency('sdeventplus')

systemd_dep = dependency('systemd')

phosphorlogging = dependency(
    'phosphor-logging',
    fallback: [
        'phosphor-logging',
        'phosphor_logging_dep'
    ]
)

dump_deps = [
    systemd_dep,
    sdbusplus_dep,
    phosphor_dbus_interfaces_dep,
    sdeventplus_dep,
    fmt_dep,
    phosphorlogging,
]

collect_deps = [
   fmt_dep,
   phosphorlogging,
   cxx.find_library('pdbg'),
   cxx.find_library('libdt-api'),
   cxx.find_library('phal'),
]

# source files
dump_src = files(
    'dump_manager_main.cpp',
    'dump_utils.cpp',
    'dump_manager.cpp',
)

collect_src = files(
    'argument.cpp',
    'dump_utils.cpp',
    'dump_collect.cpp',
    'dump_collect_main.cpp',
    'create_pel.cpp',
)


executable('openpower-dump-manager',
    dump_src,
    dependencies: dump_deps,
    implicit_include_directories: true,
    install: true
)

executable('dump-collect',
    collect_src,
    dependencies: collect_deps,
    implicit_include_directories: true,
    install: true
)

# subdir('dist')
