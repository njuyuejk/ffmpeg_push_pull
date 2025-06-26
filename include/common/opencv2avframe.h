//
// Created by YJK on 2025/3/13.
//

#ifndef FFMPEG_PULL_PUSH_OPENCV2AVFRAME_H
#define FFMPEG_PULL_PUSH_OPENCV2AVFRAME_H
#include "opencv2/opencv.hpp"
#include "logger/Logger.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
};

//AVFrame 转 cv::Mat
cv::Mat AVFrameToMat(const AVFrame* frame);

//cv::Mat 转 AVFrame
void CvMatToAVFrame(const cv::Mat& input_mat, AVFrame* out_avframe);

#endif //FFMPEG_PULL_PUSH_OPENCV2AVFRAME_H
