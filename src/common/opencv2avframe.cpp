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
    int image_width = input_mat.cols;
    int image_height = input_mat.rows;
    int cvLinesizes[1];
    cvLinesizes[0] = input_mat.step1();

    SwsContext* openCVBGRToAVFrameSwsContext = sws_getContext(
            image_width,
            image_height,
            AVPixelFormat::AV_PIX_FMT_BGR24,
            image_width,
            image_height,
            // AVPixelFormat::AV_PIX_FMT_YUV420P,
            AVPixelFormat::AV_PIX_FMT_NV12,
            SWS_FAST_BILINEAR,
            nullptr, nullptr, nullptr
    );

    sws_scale(openCVBGRToAVFrameSwsContext,
              &input_mat.data,
              cvLinesizes,
              0,
              image_height,
              out_avframe->data,
              out_avframe->linesize);

    if (openCVBGRToAVFrameSwsContext != nullptr)
    {
        sws_freeContext(openCVBGRToAVFrameSwsContext);
        openCVBGRToAVFrameSwsContext = nullptr;
    }

    printf("输出转化后的数据格式: %d", out_avframe->height);

}
