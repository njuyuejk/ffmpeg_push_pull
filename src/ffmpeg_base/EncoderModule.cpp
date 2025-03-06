#include "ffmpeg_base/EncoderModule.h"
#include "ffmpeg_base/FFmpegException.h"
#include "logger/Logger.h"
#include <algorithm>

extern "C" {
#include <libavutil/opt.h>
}

EncoderModule::EncoderModule(const StreamConfig& config)
        : config_(config) {
}

EncoderModule::~EncoderModule() {
}

AVFormatContext* EncoderModule::openOutput(const std::string& url, const std::string& format) {
    int ret;
    AVFormatContext* outputFormatContext = nullptr;

    // 分配输出上下文
    ret = avformat_alloc_output_context2(&outputFormatContext, nullptr, format.c_str(), url.c_str());
    if (ret < 0 || !outputFormatContext) {
        throw FFmpegException("Failed to allocate output context", ret);
    }

    // 打开输出文件/URL
    if (!(outputFormatContext->oformat->flags & AVFMT_NOFILE)) {
        // 设置额外输出选项
        AVDictionary* options = nullptr;
        for (const auto& [key, value] : config_.extraOptions) {
            if (key.find("output_") == 0) {
                std::string optionName = key.substr(7); // 去除"output_"前缀
                av_dict_set(&options, optionName.c_str(), value.c_str(), 0);
            }
        }

        ret = avio_open2(&outputFormatContext->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, &options);
        av_dict_free(&options);

        if (ret < 0) {
            avformat_free_context(outputFormatContext);
            throw FFmpegException("Failed to open output URL: " + url, ret);
        }
    }

    Logger::info("Successfully prepared output: " + url);
    return outputFormatContext;
}

AVBufferRef* EncoderModule::createHardwareDevice() {
    if (!config_.enableHardwareAccel || config_.hwaccelType.empty() ||
        config_.hwaccelType == "none") {
        return nullptr;
    }

    Logger::debug("Attempting to create hardware device of type: " + config_.hwaccelType);

    // 如果是"auto"，尝试不同的硬件加速类型
    if (config_.hwaccelType == "auto") {
        static const AVHWDeviceType hwTypes[] = {
                AV_HWDEVICE_TYPE_CUDA,       // NVIDIA GPU
                AV_HWDEVICE_TYPE_QSV,        // Intel Quick Sync
                AV_HWDEVICE_TYPE_VAAPI,      // VA-API (Intel/AMD)
                AV_HWDEVICE_TYPE_VDPAU,      // VDPAU (NVIDIA)
                AV_HWDEVICE_TYPE_D3D11VA,    // DirectX 11 (Windows)
                AV_HWDEVICE_TYPE_DXVA2,      // DirectX (Windows)
                AV_HWDEVICE_TYPE_VIDEOTOOLBOX // Apple
#if 0
                ,AV_HWDEVICE_TYPE_RKMPP       // RK3588
#endif
        };

        for (AVHWDeviceType hwType : hwTypes) {
            AVBufferRef* hwDeviceContext = nullptr;

            // 跳过未知类型
            if (hwType == AV_HWDEVICE_TYPE_NONE)
                continue;

            const char* typeName = av_hwdevice_get_type_name(hwType);
            Logger::debug("Trying hardware type: " + std::string(typeName ? typeName : "Unknown"));

            int ret = av_hwdevice_ctx_create(&hwDeviceContext, hwType, nullptr, nullptr, 0);
            if (ret >= 0) {
                const char* hwName = av_hwdevice_get_type_name(hwType);
                Logger::info("Auto-detected hardware acceleration: " + std::string(hwName));
                return hwDeviceContext;
            } else {
                char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errBuf, AV_ERROR_MAX_STRING_SIZE);
                Logger::debug("Failed to initialize " + std::string(typeName ? typeName : "Unknown") +
                              " hardware: " + errBuf);
            }
        }

        Logger::warning("No hardware acceleration device found, falling back to software");
        return nullptr;
    }

    // 使用指定的硬件加速类型
    AVBufferRef* hwDeviceContext = nullptr;
    AVHWDeviceType hwType = av_hwdevice_find_type_by_name(config_.hwaccelType.c_str());

    if (hwType == AV_HWDEVICE_TYPE_NONE) {
        Logger::warning("Hardware acceleration type not found: " + config_.hwaccelType);
        return nullptr;
    }

    int ret = av_hwdevice_ctx_create(&hwDeviceContext, hwType, nullptr, nullptr, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errBuf, AV_ERROR_MAX_STRING_SIZE);
        Logger::warning("Failed to create hardware device context (" + config_.hwaccelType +
                        "): " + errBuf);
        return nullptr;
    }

    Logger::info("Created hardware acceleration context: " + config_.hwaccelType);
    return hwDeviceContext;
}

const AVCodec* EncoderModule::getEncoderByHardwareType(AVHWDeviceType hwType, AVCodecID codecId) {
    std::vector<std::string> encoderCandidates;

    // 根据硬件类型和编解码器ID选择合适的编码器
    if (codecId == AV_CODEC_ID_H264) {
        switch (hwType) {
            case AV_HWDEVICE_TYPE_CUDA:
                encoderCandidates = {"h264_cuvid"};
                break;
            case AV_HWDEVICE_TYPE_QSV:
                encoderCandidates = {"h264_qsv"};
                break;
            case AV_HWDEVICE_TYPE_VAAPI:
                encoderCandidates = {"h264_vaapi"};
                break;
            case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
                encoderCandidates = {"h264_videotoolbox"};
                break;
            case AV_HWDEVICE_TYPE_D3D11VA:
            case AV_HWDEVICE_TYPE_DXVA2:
                encoderCandidates = {"h264_amf", "h264_nvenc"};  // 尝试AMD和NVIDIA
                break;
#if 0
                case AV_HWDEVICE_TYPE_RKMPP:
                encoderCandidates = {"h264_rkmpp"};
                break;
#endif
            default:
                // 如果是未知硬件类型，尝试所有可能的硬件编码器
                encoderCandidates = {"h264_nvenc", "h264_qsv", "h264_vaapi", "h264_videotoolbox", "h264_amf", "h264_cuvid"};
        }
        // 添加软件编码器作为候选
        encoderCandidates.push_back("libx264");
    } else if (codecId == AV_CODEC_ID_HEVC) {
        switch (hwType) {
            case AV_HWDEVICE_TYPE_CUDA:
                encoderCandidates = {"hevc_cuvid"};
                break;
            case AV_HWDEVICE_TYPE_QSV:
                encoderCandidates = {"hevc_qsv"};
                break;
            case AV_HWDEVICE_TYPE_VAAPI:
                encoderCandidates = {"hevc_vaapi"};
                break;
            case AV_HWDEVICE_TYPE_VIDEOTOOLBOX:
                encoderCandidates = {"hevc_videotoolbox"};
                break;
            case AV_HWDEVICE_TYPE_D3D11VA:
            case AV_HWDEVICE_TYPE_DXVA2:
                encoderCandidates = {"hevc_amf", "hevc_nvenc"};
                break;
#if 0
                case AV_HWDEVICE_TYPE_RKMPP:
                encoderCandidates = {"hevc_rkmpp"};
                break;
#endif
            default:
                // 如果是未知硬件类型，尝试所有可能的硬件编码器
                encoderCandidates = {"hevc_nvenc", "hevc_qsv", "hevc_vaapi", "hevc_videotoolbox", "hevc_amf", "hevc_cuvid"};
        }
        // 添加软件编码器作为候选
        encoderCandidates.push_back("libx265");
    } else {
        // 对于其他编解码器类型，尝试使用默认编码器
        return avcodec_find_encoder(codecId);
    }

    // 尝试所有候选编码器
    const AVCodec* selectedEncoder = nullptr;
    for (const auto& candidate : encoderCandidates) {
        const AVCodec* encoder = avcodec_find_encoder_by_name(candidate.c_str());
        if (encoder) {
            Logger::debug("Found encoder: " + candidate);
            // 验证此编码器是否与硬件类型兼容
            if (hwType != AV_HWDEVICE_TYPE_NONE &&
                candidate != "libx264" && candidate != "libx265") {

                // 验证编码器的硬件加速能力
                bool supportsHW = false;
                const AVCodecHWConfig* hwConfig = nullptr;
                int i = 0;
                while ((hwConfig = avcodec_get_hw_config(encoder, i++)) != nullptr) {
                    if (hwConfig->device_type == hwType) {
                        supportsHW = true;
                        break;
                    }
                }

                if (!supportsHW) {
                    Logger::debug("Encoder " + candidate + " does not support hardware type " +
                                  av_hwdevice_get_type_name(hwType));
                    continue;  // 尝试下一个候选编码器
                }
            }

            selectedEncoder = encoder;
            break;
        }
    }

    if (!selectedEncoder) {
        // 如果找不到特定编码器，尝试使用默认编码器
        Logger::warning("No suitable hardware encoder found, falling back to default");
        selectedEncoder = avcodec_find_encoder(codecId);
    }

    if (selectedEncoder) {
        Logger::info("Selected encoder: " + std::string(selectedEncoder->name) +
                     (hwType != AV_HWDEVICE_TYPE_NONE ?
                      " for hardware type: " + std::string(av_hwdevice_get_type_name(hwType)) :
                      " (software)"));
    }

    return selectedEncoder;
}

bool EncoderModule::isCodecSupported(const AVCodec* codec, AVPixelFormat pixFmt) {
    if (!codec || !codec->pix_fmts) return false;

    int i = 0;
    while (codec->pix_fmts[i] != AV_PIX_FMT_NONE) {
        if (codec->pix_fmts[i] == pixFmt) {
            return true;
        }
        i++;
    }
    return false;
}

AVCodecContext* EncoderModule::initializeVideoEncoder(AVCodecContext* decoderContext,
                                                      AVBufferRef* hwDeviceContext,
                                                      AVStream* outputStream) {
    int ret;
    AVHWDeviceType hwType = AV_HWDEVICE_TYPE_NONE;

    if (hwDeviceContext) {
        hwType = ((AVHWDeviceContext*)hwDeviceContext->data)->type;
    }

    // 从配置中获取编解码器ID或使用默认的H.264
    AVCodecID codecId = AV_CODEC_ID_H264; // 默认H.264

    // 在配置中查找指定的编解码器ID
    for (const auto& [key, value] : config_.extraOptions) {
        if (key == "video_codec_id") {
            if (value == "h264") {
                codecId = AV_CODEC_ID_H264;
            } else if (value == "hevc" || value == "h265") {
                codecId = AV_CODEC_ID_HEVC;
            }
            break;
        }
    }

    // 根据硬件类型获取适合的编码器
    const AVCodec* encoder = nullptr;
    if (hwType != AV_HWDEVICE_TYPE_NONE) {
        encoder = getEncoderByHardwareType(hwType, codecId);
    }

    // 如果没有找到硬件编码器，尝试软件编码器
    if (!encoder) {
        if (codecId == AV_CODEC_ID_H264) {
            encoder = avcodec_find_encoder_by_name("libx264");
        } else if (codecId == AV_CODEC_ID_HEVC) {
            encoder = avcodec_find_encoder_by_name("libx265");
        }

        if (!encoder) {
            // 最后尝试使用默认编码器
            encoder = avcodec_find_encoder(codecId);
        }
    }

    if (!encoder) {
        throw FFmpegException("Failed to find suitable video encoder");
    }

    Logger::info("Using video encoder: " + std::string(encoder->name));

    // 分配编码器上下文
    AVCodecContext* encoderContext = avcodec_alloc_context3(encoder);
    if (!encoderContext) {
        throw FFmpegException("Failed to allocate encoder context");
    }

    // 设置编码参数
    encoderContext->codec_id = encoder->id;
    encoderContext->codec_type = AVMEDIA_TYPE_VIDEO;

    // 设置分辨率
    if (config_.width > 0 && config_.height > 0) {
        encoderContext->width = config_.width;
        encoderContext->height = config_.height;
    } else {
        encoderContext->width = decoderContext->width;
        encoderContext->height = decoderContext->height;
    }

    // 检查配置中是否明确指定了像素格式
    std::string pixFmtStr = "nv12";  // 默认使用NV12
    for (const auto& [key, value] : config_.extraOptions) {
        if (key == "hwaccel_pix_fmt") {
            pixFmtStr = value;
            std::transform(pixFmtStr.begin(), pixFmtStr.end(), pixFmtStr.begin(), ::tolower);
            Logger::debug("User specified hardware pixel format: " + pixFmtStr);
        }
    }

    AVPixelFormat preferredFormat = AV_PIX_FMT_NV12;  // 默认
    if (pixFmtStr == "yuv420p") preferredFormat = AV_PIX_FMT_YUV420P;
    else if (pixFmtStr == "nv12") preferredFormat = AV_PIX_FMT_NV12;
    else if (pixFmtStr == "p010le") preferredFormat = AV_PIX_FMT_P010LE;

    if (hwDeviceContext) {
        // 对于硬件编码，使用首选格式
        encoderContext->pix_fmt = preferredFormat;

        // 对于NVIDIA NVENC，检查是否支持首选格式
        if (std::string(encoder->name).find("nvenc") != std::string::npos) {
            bool formatSupported = false;
            int i = 0;
            while (encoder->pix_fmts && encoder->pix_fmts[i] != AV_PIX_FMT_NONE) {
                if (encoder->pix_fmts[i] == preferredFormat) {
                    formatSupported = true;
                    break;
                }
                i++;
            }

            if (!formatSupported && encoder->pix_fmts) {
                Logger::warning("Preferred format not supported by encoder");
                encoderContext->pix_fmt = encoder->pix_fmts[0];
            }
        }

        Logger::info("Using hardware encoding with pixel format");
    } else {
        // 对于软件编码，使用编码器支持的第一个格式
        if (encoder->pix_fmts) {
            encoderContext->pix_fmt = encoder->pix_fmts[0];
            Logger::info("Using software encoding with pixel format");
        } else {
            encoderContext->pix_fmt = AV_PIX_FMT_YUV420P; // 默认
            Logger::info("Using default pixel format: YUV420P");
        }
    }

    // 设置码率
    encoderContext->bit_rate = config_.videoBitrate;

    // 设置时间基准
    if (decoderContext->framerate.num > 0 && decoderContext->framerate.den > 0) {
        // 如果解码器有有效的帧率，使用其倒数作为时间基准
        encoderContext->time_base = av_inv_q(decoderContext->framerate);
    } else if (decoderContext->time_base.num > 0 && decoderContext->time_base.den > 0) {
        // 如果解码器有有效的时间基准，直接使用
        encoderContext->time_base = decoderContext->time_base;
    } else {
        // 使用默认值：25帧每秒
        encoderContext->time_base = (AVRational){1, 25};
        Logger::warning("Using default timebase 1/25 for video encoder");
    }
    outputStream->time_base = encoderContext->time_base;

    // 设置GOP（关键帧间隔）
    encoderContext->gop_size = config_.keyframeInterval;
    encoderContext->max_b_frames = 1;

    // 设置低延迟参数
    if (config_.lowLatencyMode) {
        encoderContext->flags |= AV_CODEC_FLAG_LOW_DELAY;

        // 特定编码器的低延迟参数
        if (encoder->id == AV_CODEC_ID_H264 || encoder->id == AV_CODEC_ID_HEVC) {
            av_opt_set(encoderContext->priv_data, "preset", "ultrafast", 0);
            av_opt_set(encoderContext->priv_data, "tune", "zerolatency", 0);
            // 对于H.264使用baseline配置以提高兼容性
            if (encoder->id == AV_CODEC_ID_H264) {
                av_opt_set(encoderContext->priv_data, "profile", "baseline", 0);
            }
        } else if (encoder->id == AV_CODEC_ID_VP8 || encoder->id == AV_CODEC_ID_VP9) {
            av_opt_set_int(encoderContext->priv_data, "lag-in-frames", 0, 0);
            av_opt_set_int(encoderContext->priv_data, "deadline", 1, 0);  // 实时模式
        }
    }

    // 设置缓冲区大小
    encoderContext->rc_buffer_size = config_.bufferSize;
    encoderContext->rc_max_rate = config_.videoBitrate * 2;

    // 将硬件帧上下文连接到编码器（如果使用硬件加速）
    if (hwDeviceContext) {
        AVBufferRef* hwFramesContext = av_hwframe_ctx_alloc(hwDeviceContext);
        if (hwFramesContext) {
            AVHWFramesContext* framesCtx = (AVHWFramesContext*)hwFramesContext->data;
            framesCtx->format = encoderContext->pix_fmt;
            framesCtx->sw_format = encoderContext->pix_fmt;  // 使用相同格式
            framesCtx->width = encoderContext->width;
            framesCtx->height = encoderContext->height;
            framesCtx->initial_pool_size = 20;  // 预分配足够的帧池

            ret = av_hwframe_ctx_init(hwFramesContext);
            if (ret < 0) {
                av_buffer_unref(&hwFramesContext);
                Logger::warning("Failed to initialize hardware frames context, falling back to software");
            } else {
                encoderContext->hw_frames_ctx = hwFramesContext;
                Logger::info("Hardware frame context initialized with format");
            }
        }
    }

    // 设置编码器选项
    AVDictionary* encoderOptions = nullptr;
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("encoder_") == 0) {
            std::string optionName = key.substr(8); // 去除"encoder_"前缀
            av_dict_set(&encoderOptions, optionName.c_str(), value.c_str(), 0);
        }
    }

    // 打开编码器
    ret = avcodec_open2(encoderContext, encoder, &encoderOptions);
    av_dict_free(&encoderOptions);
    if (ret < 0) {
        avcodec_free_context(&encoderContext);
        throw FFmpegException("Failed to open video encoder", ret);
    }

    // 从编码器到输出流复制参数
    ret = avcodec_parameters_from_context(outputStream->codecpar, encoderContext);
    if (ret < 0) {
        avcodec_free_context(&encoderContext);
        throw FFmpegException("Failed to copy encoder parameters to output stream", ret);
    }

    return encoderContext;
}

AVCodecContext* EncoderModule::initializeAudioEncoder(AVCodecContext* decoderContext,
                                                      AVStream* outputStream) {
    int ret;

    // 自动选择音频编码器
    const AVCodec* encoder = nullptr;
    AVCodecID audioCodecId = AV_CODEC_ID_AAC; // 默认AAC

    // 在配置中查找指定的编解码器ID
    for (const auto& [key, value] : config_.extraOptions) {
        if (key == "audio_codec_id") {
            if (value == "aac") {
                audioCodecId = AV_CODEC_ID_AAC;
            } else if (value == "mp3") {
                audioCodecId = AV_CODEC_ID_MP3;
            } else if (value == "opus") {
                audioCodecId = AV_CODEC_ID_OPUS;
            }
            break;
        }
    }

    // 基于输出格式检查是否需要特定编解码器
    // 注意：由于我们不直接访问AVFormatContext，这里简化处理
    // 大多数流媒体格式（如FLV和HLS）通常使用AAC作为音频编解码器
    audioCodecId = AV_CODEC_ID_AAC; // 为流媒体应用选择AAC作为默认选项

    // 尝试找到合适的编码器
    if (audioCodecId == AV_CODEC_ID_AAC) {
        // 按优先级尝试不同的AAC编码器
        const char* aacEncoders[] = {"aac", "libfdk_aac", "libfaac"};
        for (const char* encoderName : aacEncoders) {
            encoder = avcodec_find_encoder_by_name(encoderName);
            if (encoder) break;
        }
    }

    // 如果没有找到特定编码器，使用默认编码器
    if (!encoder) {
        encoder = avcodec_find_encoder(audioCodecId);
    }

    if (!encoder) {
        // 最后尝试使用AAC
        encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!encoder) {
            throw FFmpegException("Failed to find suitable audio encoder");
        }
    }

    Logger::info("Using audio encoder: " + std::string(encoder->name));

    // 分配编码器上下文
    AVCodecContext* encoderContext = avcodec_alloc_context3(encoder);
    if (!encoderContext) {
        throw FFmpegException("Failed to allocate audio encoder context");
    }

    // 设置编码参数
    encoderContext->codec_id = encoder->id;
    encoderContext->codec_type = AVMEDIA_TYPE_AUDIO;
    encoderContext->sample_rate = decoderContext->sample_rate;

    // 设置声道布局
    if (encoder->channel_layouts) {
        // 使用编码器支持的声道布局
        uint64_t inputLayout = decoderContext->channel_layout ?
                               decoderContext->channel_layout :
                               av_get_default_channel_layout(decoderContext->channels);

        encoderContext->channel_layout = inputLayout;

        // 检查编码器是否支持此布局
        int i = 0;
        while (encoder->channel_layouts[i] != 0) {
            if (encoder->channel_layouts[i] == inputLayout) {
                break;
            }
            i++;
        }

        // 如果不支持，使用编码器支持的第一个布局
        if (encoder->channel_layouts[i] == 0) {
            encoderContext->channel_layout = encoder->channel_layouts[0];
        }
    } else {
        // 使用输入的声道布局
        encoderContext->channel_layout =
                decoderContext->channel_layout ?
                decoderContext->channel_layout :
                av_get_default_channel_layout(decoderContext->channels);
    }

    encoderContext->channels = av_get_channel_layout_nb_channels(
            encoderContext->channel_layout);

    // 设置采样格式
    if (encoder->sample_fmts) {
        encoderContext->sample_fmt = encoder->sample_fmts[0];
    } else {
        encoderContext->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }

    // 设置码率
    encoderContext->bit_rate = config_.audioBitrate;

    // 设置时间基准
    encoderContext->time_base = {1, encoderContext->sample_rate};
    outputStream->time_base = encoderContext->time_base;

    // 设置编码器选项
    AVDictionary* encoderOptions = nullptr;
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("audio_encoder_") == 0) {
            std::string optionName = key.substr(14); // 去除"audio_encoder_"前缀
            av_dict_set(&encoderOptions, optionName.c_str(), value.c_str(), 0);
        }
    }

    // 打开编码器
    ret = avcodec_open2(encoderContext, encoder, &encoderOptions);
    av_dict_free(&encoderOptions);
    if (ret < 0) {
        avcodec_free_context(&encoderContext);
        throw FFmpegException("Failed to open audio encoder", ret);
    }

    // 从编码器到输出流复制参数
    ret = avcodec_parameters_from_context(outputStream->codecpar, encoderContext);
    if (ret < 0) {
        avcodec_free_context(&encoderContext);
        throw FFmpegException("Failed to copy encoder parameters to output stream", ret);
    }

    return encoderContext;
}