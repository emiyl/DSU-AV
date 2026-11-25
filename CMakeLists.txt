cmake_minimum_required(VERSION 3.20)
project(dsu_switch LANGUAGES C)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "Use the Switch 1 or Switch 2 devkit toolchain; see README.md.")
endif()

if(NINTENDO_SWITCH2)
    set(DSU_SWITCH_TARGET "dsu-switch2")
    set(DSU_SWITCH_PLATFORM_NAME "Nintendo Switch 2")
else()
    set(DSU_SWITCH_TARGET "dsu-switch1")
    set(DSU_SWITCH_PLATFORM_NAME "Nintendo Switch")
endif()

add_library(dsu_server STATIC
    dsu/source/dsu_server.c
)
add_library(dsu::server ALIAS dsu_server)
target_include_directories(dsu_server
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/dsu/include"
)
target_compile_options(dsu_server PRIVATE -Wall -Wextra -Werror)
target_link_libraries(dsu_server PUBLIC nx)

add_executable(${DSU_SWITCH_TARGET} source/main.c)

target_compile_definitions(${DSU_SWITCH_TARGET} PRIVATE
    DSU_SWITCH_PLATFORM_NAME="${DSU_SWITCH_PLATFORM_NAME}"
)
target_compile_options(${DSU_SWITCH_TARGET} PRIVATE -Wall -Wextra -Werror)
target_link_libraries(${DSU_SWITCH_TARGET} PRIVATE dsu::server)

nx_create_nro(${DSU_SWITCH_TARGET}
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${DSU_SWITCH_TARGET}.nro"
)
