add_library(kfsw_services STATIC
    src/boot.c
)

add_library(kfsw::services ALIAS kfsw_services)

target_include_directories(kfsw_services
    PUBLIC
        ${CMAKE_CURRENT_LIST_DIR}/include
)

target_link_libraries(kfsw_services
    PUBLIC
        kfsw::platform
        zephyr_interface
)
