//
// Created by YJK on 2025/3/13.
//
#include "common/opencv2avframe.h"

cv::Mat AVFrameToMat(const AVFrame* frame) {
    // 使用sws_scale进行像素格式转换（从FFmpeg的格式转为BGR格式，OpenCV常用）
    SwsContext* swsContext = sws_getContext(
            frame->width, frame->height, (AVPixelFormat)frame->format,  // 源格式
            frame->width, frame->height, AV_PIX_FMT_BGR24,              // 目标格式为BGR24
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsContext) {
        throw std::runtime_error("Could not initialize swscale context");
    }

    // 创建OpenCV Mat对象
    cv::Mat mat(frame->height, frame->width, CV_8UC3);

    // 设置目标数据指针和行大小
    uint8_t* destData[4] = { mat.data, nullptr, nullptr, nullptr };
    int destLinesize[4] = { static_cast<int>(mat.step), 0, 0, 0 };

    // 执行转换
    sws_scale(swsContext, frame->data, frame->linesize, 0,
              frame->height, destData, destLinesize);

    // 释放swscale上下文
    sws_freeContext(swsContext);

    return mat;
}

void CvMatToAVFrame(const cv::Mat& input_mat, AVFrame* out_avframe)
{
    if (input_mat.empty() || !out_avframe) {
        LOGGER_ERROR("Invalid input for Mat to AVFrame conversion");
        return;
    }

    int image_width = input_mat.cols;
    int image_height = input_mat.rows;

    if (image_width <= 0 || image_height <= 0) {
        LOGGER_ERROR("Invalid image dimensions: " + std::to_string(image_width) + "x" + std::to_string(image_height));
        return;
    }

    // 设置AVFrame的基本属性
    out_avframe->width = image_width;
    out_avframe->height = image_height;
    out_avframe->format = AV_PIX_FMT_NV12;

    // 为AVFrame分配内存
    int ret = av_frame_get_buffer(out_avframe, 32); // 32字节对齐
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOGGER_ERROR("Failed to allocate buffer for AVFrame: " + std::string(errbuf));
        return;
    }

    // 确保帧是可写的
    ret = av_frame_make_writable(out_avframe);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOGGER_ERROR("Failed to make AVFrame writable: " + std::string(errbuf));
        return;
    }

    // 创建swscale上下文
    SwsContext* swsContext = sws_getContext(
            image_width, image_height, AV_PIX_FMT_BGR24,  // 源格式
            image_width, image_height, AV_PIX_FMT_NV12,   // 目标格式
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsContext) {
        LOGGER_ERROR("Could not initialize swscale context for Mat to AVFrame conversion");
        return;
    }

    // 设置源数据指针和行大小
    const uint8_t* srcData[4] = { input_mat.data, nullptr, nullptr, nullptr };
    int srcLinesize[4] = { static_cast<int>(input_mat.step), 0, 0, 0 };

    // 执行转换
    ret = sws_scale(swsContext,
                    srcData, srcLinesize,
                    0, image_height,
                    out_avframe->data, out_avframe->linesize);

    // 释放swscale上下文
    sws_freeContext(swsContext);

    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        LOGGER_ERROR("Failed to convert Mat to AVFrame, sws_scale returned: " + std::string(errbuf));
        return;
    }

    LOGGER_DEBUG("Successfully converted Mat to AVFrame: " + std::to_string(image_width) + "x" + std::to_string(image_height));

}
