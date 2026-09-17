set(kfsw_services_sources
    src/boot.c
    src/log.c
)

# Atomic snapshot writes, shared by parameters and housekeeping.
if(CONFIG_KFSW_PARAM_PERSISTENCE OR CONFIG_KFSW_HK_PERSISTENCE)
    list(APPEND kfsw_services_sources src/snapshot_file.c)
endif()

if(CONFIG_KFSW_PARAM)
    list(APPEND kfsw_services_sources src/boot-parameters/boot_table.c)
endif()

# Image version from git describe of the k-fsw repository. The fallback keeps
# a source tarball buildable.
zephyr_get(KFSW_IMAGE_VERSION SYSBUILD LOCAL)
if(NOT DEFINED KFSW_IMAGE_VERSION)
    find_package(Git QUIET)
    set(KFSW_IMAGE_VERSION "unknown")
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --always --dirty
            WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/../k-fsw"
            OUTPUT_VARIABLE kfsw_git_describe
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE kfsw_git_result
        )
        if(kfsw_git_result EQUAL 0 AND kfsw_git_describe)
            set(KFSW_IMAGE_VERSION "${kfsw_git_describe}")
        endif()

        # Reconfigure when HEAD or the branch it points to moves, so incremental
        # builds report the current commit.
        set(kfsw_git_dir "${CMAKE_CURRENT_LIST_DIR}/../k-fsw/.git")
        if(EXISTS "${kfsw_git_dir}/HEAD")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                "${kfsw_git_dir}/HEAD")
            file(READ "${kfsw_git_dir}/HEAD" kfsw_git_head)
            string(STRIP "${kfsw_git_head}" kfsw_git_head)
            if(kfsw_git_head MATCHES "^ref: (.+)$")
                set(kfsw_git_ref "${kfsw_git_dir}/${CMAKE_MATCH_1}")
                if(EXISTS "${kfsw_git_ref}")
                    set_property(DIRECTORY APPEND PROPERTY
                        CMAKE_CONFIGURE_DEPENDS "${kfsw_git_ref}")
                endif()
            endif()
        endif()
    endif()
endif()
zephyr_get(KFSW_RELEASE_SOURCE SYSBUILD LOCAL)
if(NOT KFSW_RELEASE_SOURCE)
    set(KFSW_RELEASE_SOURCE "development")
endif()
message(STATUS "K-FSW image version: ${KFSW_IMAGE_VERSION}")

if(CONFIG_KFSW_PARAM)
    list(APPEND kfsw_services_sources src/parameter/parameter.c)
    list(APPEND kfsw_services_sources src/param-parameters/param_table.c)
    if(CONFIG_KFSW_PARAM_PERSISTENCE)
        list(APPEND kfsw_services_sources src/parameter/parameter_persistence.c)
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
        src/parameter/libparam_compat.c
        src/parameter/libparam_print.c
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

    # libparam prints diagnostics with printf. They are redirected to debug log
    # lines in src/parameter/libparam_print.c instead of editing the library.
    target_compile_options(kfsw_libparam
        PRIVATE -include "${CMAKE_CURRENT_LIST_DIR}/src/parameter/libparam_print.h"
    )

    target_include_directories(kfsw_libparam
        PUBLIC
            "${KFSW_LIBPARAM_GENERATED_INCLUDE}"
            "${KFSW_LIBPARAM_ROOT}/include"
        PRIVATE
            "${KFSW_LIBPARAM_ROOT}/src"
            "${CMAKE_CURRENT_LIST_DIR}/include"
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
    list(APPEND kfsw_services_sources src/parameter/parameter_csp.c)
endif()

if(CONFIG_KFSW_FTP)
    list(APPEND kfsw_services_sources
        src/ftp/ftp_client.c
        src/ftp/ftp_link_csp.c
        src/ftp/ftp_protocol.c
        src/ftp/ftp_server.c
        src/ftp/ftp_state.c
        src/ftp/ftp_store.c
        src/ftp/ftp_transfer.c
    )
    if(CONFIG_KFSW_PARAM)
        list(APPEND kfsw_services_sources src/ftp-parameters/ftp_table.c)
    endif()
endif()

if(CONFIG_KFSW_EVENT)
    list(APPEND kfsw_services_sources src/event/event.c)
    if(CONFIG_KFSW_PARAM)
        list(APPEND kfsw_services_sources src/event-parameters/event_table.c)
    endif()
endif()

if(CONFIG_KFSW_HEALTH)
    list(APPEND kfsw_services_sources src/health/health.c)
    if(CONFIG_KFSW_PARAM)
        list(APPEND kfsw_services_sources src/health-parameters/health_table.c)
    endif()
endif()

if(CONFIG_KFSW_FWU)
    list(APPEND kfsw_services_sources src/fwu/fwu.c)
    if(CONFIG_KFSW_FWU_FILES)
        list(APPEND kfsw_services_sources src/fwu/fwu_fs.c)
    endif()
    if(CONFIG_KFSW_PARAM)
        list(APPEND kfsw_services_sources src/fwu-parameters/fwu_table.c)
    endif()
    if(CONFIG_KFSW_FWU_LITE)
        list(APPEND kfsw_services_sources src/fwu/fwu_lite_protocol.c)
        if(CONFIG_KFSW_FWU_LITE_CSP)
            list(APPEND kfsw_services_sources src/fwu/fwu_lite_csp.c)
        endif()
    endif()
endif()

if(CONFIG_KFSW_COMMAND)
    list(APPEND kfsw_services_sources src/command/command.c)
    if(CONFIG_KFSW_PARAM)
        list(APPEND kfsw_services_sources src/command-parameters/command_table.c)
    endif()
    if(CONFIG_KFSW_COMMAND_CSP)
        list(APPEND kfsw_services_sources
            src/command/command_csp.c
            src/command/command_protocol.c
        )
    endif()
endif()

if(CONFIG_KFSW_FBO)
    list(APPEND kfsw_services_sources src/fbo/fbo.c)
    if(CONFIG_KFSW_PARAM)
        list(APPEND kfsw_services_sources src/fbo-parameters/fbo_table.c)
    endif()
endif()

if(CONFIG_KFSW_GNDWDT)
    list(APPEND kfsw_services_sources src/gndwdt/gndwdt.c)
    if(CONFIG_KFSW_PARAM)
        list(APPEND kfsw_services_sources src/gndwdt-parameters/gndwdt_table.c)
    endif()
endif()

if(CONFIG_KFSW_HK)
    list(APPEND kfsw_services_sources
        src/hk/hk.c
        src/hk/hk_thread.c
    )
    list(APPEND kfsw_services_sources src/hk-parameters/hk_table.c)
    if(CONFIG_KFSW_PARAM_CSP)
        list(APPEND kfsw_services_sources src/hk/hk_remote.c)
    endif()
    if(CONFIG_KFSW_HK_CSP)
        list(APPEND kfsw_services_sources src/hk/hk_csp.c)
    endif()
    if(CONFIG_KFSW_HK_PERSISTENCE)
        list(APPEND kfsw_services_sources src/hk/hk_persist.c)
    endif()
    if(CONFIG_KFSW_HK_BEACON)
        list(APPEND kfsw_services_sources src/hk/hk_beacon.c)
    endif()
    if(CONFIG_KFSW_HK_STORE)
        list(APPEND kfsw_services_sources src/hk/hk_store.c)
    endif()
endif()

add_library(kfsw_services STATIC ${kfsw_services_sources})

add_library(kfsw::services ALIAS kfsw_services)

if(CONFIG_KFSW_FWU_FILES)
    target_link_libraries(kfsw_services PRIVATE MCUBOOT_BOOTUTIL)
endif()

target_include_directories(kfsw_services
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_compile_definitions(kfsw_services
    PRIVATE
        KFSW_IMAGE_VERSION="${KFSW_IMAGE_VERSION}"
        KFSW_SOURCE_REVISION="${KFSW_RELEASE_SOURCE}"
)

target_link_libraries(kfsw_services
    PUBLIC
        kfsw::platform
        zephyr_interface
    PRIVATE
        kernel
)

if(CONFIG_KFSW_PARAM_CSP OR CONFIG_KFSW_FTP OR CONFIG_KFSW_COMMAND_CSP
   OR CONFIG_KFSW_FWU_LITE_CSP OR CONFIG_KFSW_HK_CSP)
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
