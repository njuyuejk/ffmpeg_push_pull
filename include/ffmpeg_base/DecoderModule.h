#ifndef DECODER_MODULE_H
#define DECODER_MODULE_H

#include "common/StreamConfig.h"
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
}

/**
 * @brief 解码器模块
 */
class DecoderModule {
public:
    /**
     * @brief 构造函数
     * @param config 流配置
     */
    DecoderModule(const StreamConfig& config);

    /**
     * @brief 析构函数
     */
    ~DecoderModule();

    /**
     * @brief 初始化视频流解码器
     * @param inputStream 输入流
     * @param hwDeviceContext 硬件设备上下文（可选）
     * @return AVCodecContext* 初始化的解码器上下文
     */
    AVCodecContext* initializeVideoDecoder(AVStream* inputStream, AVBufferRef* hwDeviceContext);

    /**
     * @brief 初始化音频流解码器
     * @param inputStream 输入流
     * @return AVCodecContext* 初始化的解码器上下文
     */
    AVCodecContext* initializeAudioDecoder(AVStream* inputStream);

    /**
     * @brief 打开输入格式上下文
     * @param url 输入URL
     * @return AVFormatContext* 打开的输入格式上下文
     */
    AVFormatContext* openInput(const std::string& url);

private:
    const StreamConfig& config_;
};

#endif // DECODER_MODULE_H