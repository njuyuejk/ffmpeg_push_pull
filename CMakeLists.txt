cmake_minimum_required(VERSION 3.28)
project(ffmpeg_new_pull_push)

set(CMAKE_CXX_STANDARD 17)

# 设置 FFmpeg 的根目录
set(FFMPEG_ROOT "D:/project/C++/my/ffmpeg-multi/ffmpeg4")

# 添加包含目录
include_directories(include)
include_directories("${FFMPEG_ROOT}/include")

# 设置库目录
link_directories("${FFMPEG_ROOT}/libwin64")

# 要链接的 FFmpeg 库
set(FFMPEG_LIBS
        avcodec
        avformat
        avutil
        swscale
        swresample
)


# 查找 FFmpeg 库
foreach(lib ${FFMPEG_LIBS})
    find_library(${lib}_LIBRARY NAMES ${lib} PATHS "${FFMPEG_ROOT}/libwin64")
    if(NOT ${lib}_LIBRARY)
        message(FATAL_ERROR "FFmpeg library ${lib} not found in ${FFMPEG_ROOT}/libwin64")
    endif()
    list(APPEND FOUND_FFMPEG_LIBS ${${lib}_LIBRARY})
endforeach()

set(app
        src/app/Application.cpp
        include/app/Application.h)

set(common
        src/common/ThreadPool.cpp
        include/common/ThreadPool.h
        src/common/StreamConfig.cpp
        include/common/StreamConfig.h
)

set(ffmpeg_base
        src/ffmpeg_base/StreamProcessor.cpp
        include/ffmpeg_base/StreamProcessor.h
        src/ffmpeg_base/MultiStreamManager.cpp
        include/ffmpeg_base/MultiStreamManager.h
        src/ffmpeg_base/FFmpegException.cpp
        include/ffmpeg_base/FFmpegException.h
)

set(logger
        src/logger/Logger.cpp
        include/logger/Logger.h)

add_executable(ffmpeg_new_pull_push main.cpp ${app} ${common} ${ffmpeg_base} ${logger})

# 链接 FFmpeg 库
target_link_libraries(ffmpeg_new_pull_push PRIVATE ${FOUND_FFMPEG_LIBS} pthread)
