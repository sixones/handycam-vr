cmake_minimum_required(VERSION 3.15)
project(handycam_vr)

set(CMAKE_CXX_STANDARD 23)

add_executable(handycam_vr handycam-vr.cpp)

option(FFMPEG_STATIC "Link FFmpeg statically" OFF)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    # Detect without STATIC so this works on all cmake versions and platforms.
    pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
        libavformat libavcodec libavutil libswscale libswresample)
endif()

if(FFMPEG_FOUND)
    message(STATUS "Building with FFmpeg support (static=${FFMPEG_STATIC})")
    target_compile_definitions(handycam_vr PRIVATE HAS_FFMPEG)
    target_include_directories(handycam_vr PRIVATE ${FFMPEG_INCLUDE_DIRS})

    if(FFMPEG_STATIC)
        # pkg_check_modules without STATIC only reports Libs, not Libs.private,
        # so transitive deps (libx264 etc.) are missing from the link. Call
        # pkg-config --static directly to get the full set of flags.
        find_program(PKG_CONFIG_PROG pkg-config REQUIRED)
        execute_process(
            COMMAND ${PKG_CONFIG_PROG} --static --libs
                libavformat libavcodec libavutil libswscale libswresample
            OUTPUT_VARIABLE FFMPEG_STATIC_LINK_FLAGS
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        separate_arguments(FFMPEG_STATIC_LINK_LIST UNIX_COMMAND "${FFMPEG_STATIC_LINK_FLAGS}")

        # Rejoin "-framework Foo" pairs that separate_arguments splits into two
        # tokens, otherwise target_link_libraries misinterprets them.
        set(FFMPEG_PROCESSED_FLAGS)
        set(FFMPEG_NEXT_IS_FRAMEWORK FALSE)
        foreach(flag IN LISTS FFMPEG_STATIC_LINK_LIST)
            if(FFMPEG_NEXT_IS_FRAMEWORK)
                list(APPEND FFMPEG_PROCESSED_FLAGS "-framework ${flag}")
                set(FFMPEG_NEXT_IS_FRAMEWORK FALSE)
            elseif(flag STREQUAL "-framework")
                set(FFMPEG_NEXT_IS_FRAMEWORK TRUE)
            else()
                list(APPEND FFMPEG_PROCESSED_FLAGS "${flag}")
            endif()
        endforeach()

        target_link_libraries(handycam_vr PRIVATE ${FFMPEG_PROCESSED_FLAGS})
    else()
        target_link_libraries(handycam_vr PRIVATE PkgConfig::FFMPEG)
    endif()
else()
    message(STATUS "Building without FFmpeg support")
endif()

include(FetchContent)
FetchContent_Declare(
        cxxopts
        GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
        GIT_TAG        v3.2.0
)
FetchContent_MakeAvailable(cxxopts)

target_link_libraries(handycam_vr PRIVATE cxxopts::cxxopts)