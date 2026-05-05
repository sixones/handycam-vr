cmake_minimum_required(VERSION 3.15)
project(handycam_vr)

set(CMAKE_CXX_STANDARD 23)

add_executable(handycam_vr handycam-vr.cpp)

find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET libavformat libavcodec libavutil libswscale libswresample)
endif()

if(FFMPEG_FOUND)
    message(STATUS "Building with FFmpeg support")

    target_compile_definitions(handycam_vr PRIVATE HAS_FFMPEG)
    target_include_directories(handycam_vr PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_directories(handycam_vr PRIVATE ${FFMPEG_LIBRARY_DIRS})
    target_link_libraries(handycam_vr PRIVATE ${FFMPEG_LIBRARIES})
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