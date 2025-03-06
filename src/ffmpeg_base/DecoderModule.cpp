#include "ffmpeg_base/DecoderModule.h"
#include "ffmpeg_base/FFmpegException.h"
#include "logger/Logger.h"

DecoderModule::DecoderModule(const StreamConfig& config)
        : config_(config) {
}

DecoderModule::~DecoderModule() {
}

AVFormatContext* DecoderModule::openInput(const std::string& url) {
    int ret;

    // 分配输入上下文
    AVFormatContext* inputFormatContext = avformat_alloc_context();
    if (!inputFormatContext) {
        throw FFmpegException("Failed to allocate input format context");
    }

    // 创建选项字典
    AVDictionary* options = nullptr;

    // 增加探测大小和分析持续时间，解决"not enough frames to estimate rate"问题
    av_dict_set(&options, "probesize", "10485760", 0);     // 10MB (默认是5MB)
    av_dict_set(&options, "analyzeduration", "5000000", 0); // 5秒 (默认是0.5秒)

    // 设置低延迟选项
    if (config_.lowLatencyMode) {
        inputFormatContext->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

        // 针对RTSP流增加额外选项
        if (url.find("rtsp://") == 0) {
            // RTSP特定选项
            av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用TCP传输RTSP (比UDP更可靠)
            av_dict_set(&options, "stimeout", "5000000", 0);    // Socket超时5秒
            av_dict_set(&options, "max_delay", "500000", 0);    // 最大延迟500毫秒
        }
    }

    // 添加额外选项
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("input_") == 0) {
            std::string optionName = key.substr(6); // 去除"input_"前缀
            av_dict_set(&options, optionName.c_str(), value.c_str(), 0);
        }
    }

    // 打开输入
    ret = avformat_open_input(&inputFormatContext, url.c_str(), nullptr, &options);

    // 检查是否有未使用的选项
    if (av_dict_count(options) > 0) {
        char *unused = nullptr;
        av_dict_get_string(options, &unused, '=', ',');
        if (unused) {
            Logger::warning("Unused input options: " + std::string(unused));
            av_free(unused);
        }
    }

    av_dict_free(&options);

    if (ret < 0) {
        avformat_free_context(inputFormatContext);
        throw FFmpegException("Failed to open input: " + url, ret);
    }

    // 查找流信息 - 增加选项以更好地检测流
    AVDictionary* streamInfoOptions = nullptr;
    if (url.find("rtsp://") == 0) {
        // 对于RTSP流，可能需要更多帧来准确检测流信息
        av_dict_set(&streamInfoOptions, "analyzeduration", "10000000", 0); // 10秒
    }

    ret = avformat_find_stream_info(inputFormatContext, streamInfoOptions ? &streamInfoOptions : nullptr);
    av_dict_free(&streamInfoOptions);

    if (ret < 0) {
        avformat_close_input(&inputFormatContext);
        throw FFmpegException("Failed to find stream info", ret);
    }

    // 输出找到的流信息
    Logger::debug("Input stream information:");
    for (unsigned int i = 0; i < inputFormatContext->nb_streams; i++) {
        AVStream* stream = inputFormatContext->streams[i];

        // 获取解码器名称
        const char* codecName = avcodec_get_name(stream->codecpar->codec_id);

        std::string streamType;
        switch(stream->codecpar->codec_type) {
            case AVMEDIA_TYPE_VIDEO: streamType = "Video"; break;
            case AVMEDIA_TYPE_AUDIO: streamType = "Audio"; break;
            case AVMEDIA_TYPE_SUBTITLE: streamType = "Subtitle"; break;
            default: streamType = "Other"; break;
        }

        Logger::debug("  Stream #" + std::to_string(i) + ": " + streamType + " - " + codecName);

        // 如果是视频流，打印更多信息
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            Logger::debug("    Resolution: " + std::to_string(stream->codecpar->width) + "x" +
                          std::to_string(stream->codecpar->height));

            if (stream->r_frame_rate.num && stream->r_frame_rate.den) {
                double fps = (double)stream->r_frame_rate.num / stream->r_frame_rate.den;
                Logger::debug("    Frame rate: " + std::to_string(fps) + " fps");
            } else {
                Logger::warning("    Frame rate could not be determined");
            }

            // 获取视频编码器详细信息
            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                Logger::debug("    Codec: " + std::string(codec->name) + " (" + codec->long_name + ")");
            }
        }

        // 如果是音频流，打印更多信息
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            Logger::debug("    Sample rate: " + std::to_string(stream->codecpar->sample_rate) + " Hz");
            Logger::debug("    Channels: " + std::to_string(stream->codecpar->channels));

            // 获取音频编码器详细信息
            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                Logger::debug("    Codec: " + std::string(codec->name) + " (" + codec->long_name + ")");
            }
        }
    }

    Logger::info("Successfully opened input: " + url);
    return inputFormatContext;
}

AVCodecContext* DecoderModule::initializeVideoDecoder(AVStream* inputStream, AVBufferRef* hwDeviceContext) {
    int ret;

    // 查找解码器
    const AVCodec* decoder = avcodec_find_decoder(inputStream->codecpar->codec_id);
    if (!decoder) {
        Logger::warning("Unsupported video codec");
        return nullptr;
    }

    // 分配解码器上下文
    AVCodecContext* decoderContext = avcodec_alloc_context3(decoder);
    if (!decoderContext) {
        throw FFmpegException("Failed to allocate decoder context");
    }

    // 从输入流复制编解码器参数
    ret = avcodec_parameters_to_context(decoderContext, inputStream->codecpar);
    if (ret < 0) {
        avcodec_free_context(&decoderContext);
        throw FFmpegException("Failed to copy decoder parameters", ret);
    }

    // 设置硬件加速（如果启用）
    if (hwDeviceContext) {
        decoderContext->hw_device_ctx = av_buffer_ref(hwDeviceContext);
    }

    // 打开解码器
    AVDictionary* decoderOptions = nullptr;
    // 设置解码器选项
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("decoder_") == 0) {
            std::string optionName = key.substr(8); // 去除"decoder_"前缀
            av_dict_set(&decoderOptions, optionName.c_str(), value.c_str(), 0);
        }
    }

    ret = avcodec_open2(decoderContext, decoder, &decoderOptions);
    av_dict_free(&decoderOptions);

    if (ret < 0) {
        if (decoderContext->hw_device_ctx) {
            // 如果硬件解码失败，尝试回退到软件解码
            Logger::warning("Hardware-accelerated decoding failed, falling back to software decoding");
            av_buffer_unref(&decoderContext->hw_device_ctx);
            decoderContext->hw_device_ctx = nullptr;

            // 重新打开解码器，这次不使用硬件加速
            ret = avcodec_open2(decoderContext, decoder, nullptr);
            if (ret < 0) {
                avcodec_free_context(&decoderContext);
                throw FFmpegException("Failed to open video decoder even with software fallback", ret);
            }
        } else {
            avcodec_free_context(&decoderContext);
            throw FFmpegException("Failed to open video decoder", ret);
        }
    }

    return decoderContext;
}

AVCodecContext* DecoderModule::initializeAudioDecoder(AVStream* inputStream) {
    int ret;

    // 查找解码器
    const AVCodec* decoder = avcodec_find_decoder(inputStream->codecpar->codec_id);
    if (!decoder) {
        Logger::warning("Unsupported audio codec");
        return nullptr;
    }

    // 分配解码器上下文
    AVCodecContext* decoderContext = avcodec_alloc_context3(decoder);
    if (!decoderContext) {
        throw FFmpegException("Failed to allocate audio decoder context");
    }

    // 从输入流复制编解码器参数
    ret = avcodec_parameters_to_context(decoderContext, inputStream->codecpar);
    if (ret < 0) {
        avcodec_free_context(&decoderContext);
        throw FFmpegException("Failed to copy audio decoder parameters", ret);
    }

    // 设置解码器选项
    AVDictionary* decoderOptions = nullptr;
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("audio_decoder_") == 0) {
            std::string optionName = key.substr(14); // 去除"audio_decoder_"前缀
            av_dict_set(&decoderOptions, optionName.c_str(), value.c_str(), 0);
        }
    }

    // 打开解码器
    ret = avcodec_open2(decoderContext, decoder, &decoderOptions);
    av_dict_free(&decoderOptions);

    if (ret < 0) {
        avcodec_free_context(&decoderContext);
        throw FFmpegException("Failed to open audio decoder", ret);
    }

    return decoderContext;
}