cmake_minimum_required(VERSION 3.20)
project(dsu_switch LANGUAGES C)

add_subdirectory(dsu)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)

add_library(av_codecs STATIC
    media/source/av_codecs.c
)
add_library(av::codecs ALIAS av_codecs)
target_include_directories(av_codecs
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/media/include"
)
target_compile_options(av_codecs PRIVATE -Wall -Wextra -Werror)
if(NOT NINTENDO_SWITCH2)
    target_compile_definitions(av_codecs PRIVATE AV_CODECS_SOFTWARE_H264)
endif()
target_link_libraries(av_codecs
    PUBLIC
        dsu::server
        PkgConfig::SWSCALE
    PRIVATE
        PkgConfig::OPUS
        PkgConfig::AVCODEC
)

if(NOT NINTENDO_SWITCH)
    find_package(SDL2 REQUIRED CONFIG)

    add_executable(dsu-controller
        app/main.c
        app/sdl_display.c
        platform/desktop.c
    )
    target_link_libraries(dsu-controller
        PRIVATE
            av::codecs
            SDL2::SDL2
            SDL2::SDL2main
    )
    target_compile_options(dsu-controller PRIVATE -Wall -Wextra -Werror)
    target_compile_features(dsu-controller PRIVATE c_std_11)
    return()
endif()

if(NINTENDO_SWITCH2)
    set(DSU_SWITCH_TARGET "dsu-controller-switch2")
    set(DSU_SWITCH_PLATFORM_NAME "Nintendo Switch 2")
else()
    set(DSU_SWITCH_TARGET "dsu-controller-switch1")
    set(DSU_SWITCH_PLATFORM_NAME "Nintendo Switch")
endif()

add_executable(${DSU_SWITCH_TARGET}
    app/main.c
    app/sdl_display.c
    platform/switch.c
    platform/switch_av.c
)

target_compile_definitions(${DSU_SWITCH_TARGET} PRIVATE
    DSU_SWITCH_PLATFORM_NAME="${DSU_SWITCH_PLATFORM_NAME}"
)
target_compile_options(${DSU_SWITCH_TARGET} PRIVATE -Wall -Wextra -Werror)
target_include_directories(${DSU_SWITCH_TARGET}
    PRIVATE
        "${DEVKITPRO}/portlibs/switch/include"
        "${DEVKITPRO}/portlibs/switch/include/SDL2"
)
target_link_libraries(${DSU_SWITCH_TARGET}
    PRIVATE
        av::codecs
        nx
        SDL2
        EGL
        drm_nouveau
        glapi
        pthread
        stdc++
)

nx_create_nro(${DSU_SWITCH_TARGET}
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${DSU_SWITCH_TARGET}.nro"
)
