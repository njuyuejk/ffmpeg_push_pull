#ifndef ENCODER_MODULE_H
#define ENCODER_MODULE_H

#include "common/StreamConfig.h"
#include <memory>
#include <vector>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

/**
 * @brief 编码器模块
 */
class EncoderModule {
public:
    /**
     * @brief 构造函数
     * @param config 流配置
     */
    EncoderModule(const StreamConfig& config);

    /**
     * @brief 析构函数
     */
    ~EncoderModule();

    /**
     * @brief 初始化视频流编码器
     * @param decoderContext 解码器上下文（用于参数）
     * @param hwDeviceContext 硬件设备上下文（可选）
     * @param outputStream 输出流
     * @return AVCodecContext* 初始化的编码器上下文
     */
    AVCodecContext* initializeVideoEncoder(AVCodecContext* decoderContext,
                                           AVBufferRef* hwDeviceContext,
                                           AVStream* outputStream);

    /**
     * @brief 初始化音频流编码器
     * @param decoderContext 解码器上下文（用于参数）
     * @param outputStream 输出流
     * @return AVCodecContext* 初始化的编码器上下文
     */
    AVCodecContext* initializeAudioEncoder(AVCodecContext* decoderContext,
                                           AVStream* outputStream);

    /**
     * @brief 打开输出格式上下文
     * @param url 输出URL
     * @param format 输出格式
     * @return AVFormatContext* 打开的输出格式上下文
     */
    AVFormatContext* openOutput(const std::string& url, const std::string& format);

    /**
     * @brief 创建硬件加速设备
     * @return AVBufferRef* 硬件设备上下文
     */
    AVBufferRef* createHardwareDevice();

    /**
     * @brief 根据硬件类型获取编码器
     * @param hwType 硬件设备类型
     * @param codecId 编解码器ID
     * @return const AVCodec* 编码器
     */
    const AVCodec* getEncoderByHardwareType(AVHWDeviceType hwType, AVCodecID codecId);

private:
    const StreamConfig& config_;

    /**
     * @brief 检查编解码器是否支持指定的像素格式
     * @param codec 编解码器
     * @param pixFmt 像素格式
     * @return bool 是否支持该格式
     */
    bool isCodecSupported(const AVCodec* codec, AVPixelFormat pixFmt);
};

#endif // ENCODER_MODULE_H