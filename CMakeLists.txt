set(kfsw_services_sources
    src/boot.c
    src/log.c
)

if(CONFIG_KFSW_PARAM)
    list(APPEND kfsw_services_sources src/parameter.c)
    if(CONFIG_KFSW_PARAM_PERSISTENCE)
        list(APPEND kfsw_services_sources src/parameter_persistence.c)
    endif()
endif()

if(CONFIG_KFSW_PARAM_CSP)
    set(KFSW_LIBPARAM_ROOT
        "${CMAKE_CURRENT_LIST_DIR}/third_party/libparam"
    )

    if(NOT EXISTS "${KFSW_LIBPARAM_ROOT}/LICENSE")
        message(FATAL_ERROR
            "libparam is missing; run 'west update libparam' from the workspace root"
        )
    endif()

    set(KFSW_LIBPARAM_GENERATED_INCLUDE
        "${CMAKE_CURRENT_BINARY_DIR}/generated/libparam"
    )
    file(MAKE_DIRECTORY "${KFSW_LIBPARAM_GENERATED_INCLUDE}")
    file(MAKE_DIRECTORY "${KFSW_LIBPARAM_GENERATED_INCLUDE}/mpack")
    configure_file(
        zephyr/libparam.h.in
        "${KFSW_LIBPARAM_GENERATED_INCLUDE}/libparam.h"
        @ONLY
    )
    configure_file(
        zephyr/mpack-config.h
        "${KFSW_LIBPARAM_GENERATED_INCLUDE}/mpack/mpack-config.h"
        COPYONLY
    )

    set(kfsw_libparam_sources
        src/libparam_compat.c
        "${KFSW_LIBPARAM_ROOT}/src/mpack/mpack.c"
        "${KFSW_LIBPARAM_ROOT}/src/param/list/param_list.c"
        "${KFSW_LIBPARAM_ROOT}/src/param/param.c"
        "${KFSW_LIBPARAM_ROOT}/src/param/param_queue.c"
        "${KFSW_LIBPARAM_ROOT}/src/param/param_serializer.c"
        "${KFSW_LIBPARAM_ROOT}/src/param/param_server.c"
        "${KFSW_LIBPARAM_ROOT}/src/param/param_string.c"
        "${KFSW_LIBPARAM_ROOT}/src/param/param_wildcard.c"
        "${KFSW_LIBPARAM_ROOT}/src/vmem/vmem.c"
    )
    add_library(kfsw_libparam STATIC ${kfsw_libparam_sources})
    set_source_files_properties(${kfsw_libparam_sources}
        PROPERTIES COMPILE_OPTIONS "-Wno-double-promotion"
    )

    target_include_directories(kfsw_libparam
        PUBLIC
            "${KFSW_LIBPARAM_GENERATED_INCLUDE}"
            "${KFSW_LIBPARAM_ROOT}/include"
        PRIVATE
            "${KFSW_LIBPARAM_ROOT}/src"
    )

    target_compile_definitions(kfsw_libparam
        PRIVATE strlcpy=kfsw_libparam_strlcpy
    )

    target_link_libraries(kfsw_libparam
        PUBLIC
            csp
            zephyr_interface
    )

    zephyr_linker_sources(SECTIONS zephyr/libparam-sections.ld)
    list(APPEND kfsw_services_sources src/parameter_csp.c)
endif()

if(CONFIG_KFSW_FTP)
    list(APPEND kfsw_services_sources
        src/ftp_client.c
        src/ftp_common.c
        src/ftp_protocol.c
        src/ftp_server.c
    )
endif()

add_library(kfsw_services STATIC ${kfsw_services_sources})

add_library(kfsw::services ALIAS kfsw_services)

target_include_directories(kfsw_services
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(kfsw_services
    PUBLIC
        kfsw::platform
        zephyr_interface
    PRIVATE
        kernel
)

if(CONFIG_KFSW_PARAM_CSP OR CONFIG_KFSW_FTP)
    target_link_libraries(kfsw_services
        PRIVATE
            kfsw::comms
    )
endif()

if(CONFIG_KFSW_PARAM_CSP)
    target_include_directories(kfsw_services
        PRIVATE
            "${KFSW_LIBPARAM_ROOT}/src/param/list"
    )
    target_link_libraries(kfsw_services
        PRIVATE
            kfsw_libparam
    )
endif()
