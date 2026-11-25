cmake_minimum_required(VERSION 3.20)
project(dsu_server LANGUAGES C)

add_library(dsu_server STATIC
    source/dsu_server.c
)
add_library(dsu::server ALIAS dsu_server)

target_include_directories(dsu_server
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_features(dsu_server PUBLIC c_std_11)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(dsu_server PRIVATE -Wall -Wextra -Werror)
endif()
