cmake_minimum_required(VERSION 3.28)
project(ffmpeg_pull_push)

set(CMAKE_CXX_STANDARD 17)

# GDB调试设置
set(CMAKE_BUILD_TYPE Debug)
add_definitions("-Wall -g")

# 设置 FFmpeg 的根目录
set(FFMPEG_ROOT "D:/project/C++/my/ffmpeg-multi/ffmpeg4")

# 设置第三方库的根目录
set(THIRD_PARTY_DIR "D:/project/C++/my/ffmpeg_push_pull/hkvision")

include_directories("D:/project/C++/my/ffmpeg_push_pull/protobuf/include")

# 添加Abseil
include(FetchContent)
FetchContent_Declare(
        absl
        GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
        GIT_TAG master  # 您可以使用特定版本，如20230125.3
)
FetchContent_MakeAvailable(absl)

# 添加包含目录
include_directories(include)
include_directories("${FFMPEG_ROOT}/include")
include_directories("${THIRD_PARTY_DIR}/include")

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


# 设置 opencv 的根目录
set(OPENCV_ROOT "D:/AI/opencv/build/install")

# 添加包含目录
include_directories(include)
include_directories("${FFMPEG_ROOT}/include")
include_directories("${OPENCV_ROOT}/include")

# 设置库目录
link_directories("${OPENCV_ROOT}/x64/mingw/lib")

# 要链接的 FFmpeg 库
set(OPENCV_LIBS
        opencv_world470
        opencv_img_hash470
)


# 查找 FFmpeg 库
foreach(lib ${OPENCV_LIBS})
    find_library(${lib}_LIBRARY NAMES ${lib} PATHS "${OPENCV_ROOT}/x64/mingw/lib")
    if(NOT ${lib}_LIBRARY)
        message(FATAL_ERROR "FFmpeg library ${lib} not found in ${OPENCV_ROOT}/x64/mingw/lib")
    endif()
    list(APPEND FOUND_OPENCV_LIBS ${${lib}_LIBRARY})
endforeach()

# 自动查找和加载指定目录下的所有库文件
file(GLOB THIRD_PARTY_LIBS "${THIRD_PARTY_DIR}/*")

# 遍历所有找到的第三方库目录
foreach(LIB_DIR ${THIRD_PARTY_LIBS})
    if(IS_DIRECTORY ${LIB_DIR})
        # 获取库名称（目录名）
        get_filename_component(LIB_NAME ${LIB_DIR} NAME)
        message(STATUS "Processing third-party library: ${LIB_NAME}")

        # 添加包含目录
        if(EXISTS "${LIB_DIR}/include")
            include_directories("${LIB_DIR}/include")
            message(STATUS "  Added include directory: ${LIB_DIR}/include")
        endif()

        # 添加库目录
        if(EXISTS "${LIB_DIR}/lib")
            link_directories("${LIB_DIR}/lib")
            message(STATUS "  Added library directory: ${LIB_DIR}/lib")

            # 查找并链接静态库和动态库文件
            file(GLOB LIB_FILES "${LIB_DIR}/lib/*.a" "${LIB_DIR}/lib/*.so" "${LIB_DIR}/lib/*.lib" "${LIB_DIR}/lib/*.dll")
            foreach(LIB_FILE ${LIB_FILES})
                get_filename_component(FILE_NAME ${LIB_FILE} NAME_WE)
                string(REGEX REPLACE "^lib" "" FILE_NAME ${FILE_NAME})
                list(APPEND THIRD_PARTY_LIBRARIES ${FILE_NAME})
                message(STATUS "  Found library file: ${LIB_FILE}")
            endforeach()
        endif()

        # 特殊处理 - 查找 CMake 配置文件
        if(EXISTS "${LIB_DIR}/cmake")
            list(APPEND CMAKE_PREFIX_PATH "${LIB_DIR}/cmake")
            message(STATUS "  Added CMake path: ${LIB_DIR}/cmake")
        endif()
    endif()
endforeach()

# 设置 mqtt 的根目录
set(MQTT_ROOT "D:/project/C++/my/mqtt-paho-win64")

# 添加包含目录
include_directories("${MQTT_ROOT}/include")

# 设置库目录
link_directories("${MQTT_ROOT}/lib")

# 要链接的 FFmpeg 库
set(MQTT_LIBS
        paho-mqtt3a
        paho-mqtt3c
)


# 查找 FFmpeg 库
foreach(lib ${MQTT_LIBS})
    find_library(${lib}_LIBRARY NAMES ${lib} PATHS "${MQTT_ROOT}/lib")
    if(NOT ${lib}_LIBRARY)
        message(FATAL_ERROR "FFmpeg library ${lib} not found in ${MQTT_ROOT}/lib")
    endif()
    list(APPEND FOUND_MQTT_LIBS ${${lib}_LIBRARY})
endforeach()


# skip 3rd-party lib dependencies
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--allow-shlib-undefined")

# rknn api
#set(RKNN_API_PATH ${CMAKE_SOURCE_DIR}/RKNN/RK3588/${CMAKE_SYSTEM_NAME}/librknn_api)
set(RKNN_API_PATH ${CMAKE_SOURCE_DIR}/RKNN/RK3588/Linux/librknn_api)
set(LIB_ARCH aarch64)
set(RKNN_RT_LIB "D:/project/C++/my/ffmpeg_push_pull/RKNN/RK3588/Linux/librknn_api/aarch64/librknnrt.so")
include_directories("${RKNN_API_PATH}/include")
include_directories("${CMAKE_SOURCE_DIR}/include/3rdparty")

#rga
set(RGA_PATH ${CMAKE_SOURCE_DIR}/include/3rdparty/rga/RK3588)
set(LIB_ARCH aarch64)
set(RGA_LIB ${RGA_PATH}/lib/Linux/${LIB_ARCH}/librga.so)
include_directories( ${RGA_PATH}/include)
set(CMAKE_INSTALL_RPATH "lib")

set(app
        src/app/Application.cpp
        include/app/Application.h)

set(common
        src/common/ThreadPool.cpp
        include/common/ThreadPool.h
        src/common/Watchdog.cpp
        include/common/Watchdog.h
        src/common/StreamConfig.cpp
        include/common/StreamConfig.h
        include/common/opencv2avframe.h
        src/common/opencv2avframe.cpp
        include/http/httplib.h
        include/common/utils.h
        src/common/utils.cpp
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

#set(PTZControl
#        src/PTZControl/HikPTZControl.cpp
#        include/PTZControl/HikPTZControl.h
#)

set(mqtt
        src/mqtt/mqtt_sync_client.cpp
        include/mqtt/mqtt_sync_client.h
        include/mqtt/PTZControl.pb.h
        src/mqtt/PTZControl.pb.cc
        include/mqtt/PTZControlHandler.h
        src/mqtt/PTZControlHandler.cpp
        include/mqtt/AIDataResponse.pb.h
        src/mqtt/AIDataResponse.pb.cc
)

set(ai_service
        include/AIService/postprocess.h
        include/AIService/rknnPool.h
        src/AIService/postprocess.cpp
        src/AIService/rknnPool.cpp
        include/AIService/preprocess.h
        src/AIService/preprocess.cpp
        include/AIService/lprnetPostprocess.h
        src/AIService/lprnetPostprocess.cpp
        src/AIService/lprnetPool.cpp
        include/AIService/lprnetPool.h
        include/AIService/lprnetPool.h
        include/common/plateCorners.h
        src/common/plateCorners.cpp
        src/AIService/postprocess_p2.cpp
        include/AIService/postprocess_p2.h
        include/AIService/postprocess_pose.h
        src/AIService/postprocess_pose.cpp
        include/AIService/rknnPool_Pose.h
        src/AIService/rknnPool_Pose.cpp
        include/AIService/postprocess_seg.h
        src/AIService/postprocess_seg.cpp
        include/AIService/rknnPool_Seg.h
        src/AIService/rknnPool_Seg.cpp
)

# 所有库源文件
set(LIB_SOURCES
        ${app}
        ${common}
        ${ffmpeg_base}
        ${logger}
        ${mqtt}
        ${ai_service}
)

# 创建静态库 (可根据需要改为 SHARED 库)
add_library(ffmpeg_processor SHARED ${LIB_SOURCES})

# 为库设置属性
set_target_properties(ffmpeg_processor PROPERTIES
        VERSION 1.0.0
        SOVERSION 1
        PUBLIC_HEADER "include/app/Application.h;include/common/StreamConfig.h;include/ffmpeg_base/MultiStreamManager.h;include/AIService/lprnetPool.h"
)

# 链接库依赖
target_link_libraries(ffmpeg_processor PRIVATE
        ${FOUND_FFMPEG_LIBS}
        ${FOUND_OPENCV_LIBS}
        ${FOUND_MQTT_LIBS}
        ${Protobuf_LIBRARIES}
        ${RKNN_RT_LIB}
        ${RGA_LIB}
        ${ai_service}
        pthread
)

add_executable(ffmpeg_pull_push main.cpp ${app} ${common} ${ffmpeg_base} ${logger} ${mqtt} ${ai_service})

# 链接 FFmpeg 库
target_link_libraries(ffmpeg_pull_push PRIVATE ffmpeg_processor ${FOUND_FFMPEG_LIBS} ${FOUND_OPENCV_LIBS} ${FOUND_MQTT_LIBS} ${RKNN_RT_LIB} ${RGA_LIB} ws2_32 pthread)
