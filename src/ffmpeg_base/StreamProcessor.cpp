#include "ffmpeg_base/StreamProcessor.h"
#include "ffmpeg_base/FFmpegException.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

StreamProcessor::StreamProcessor(const StreamConfig& config)
        : config_(config), isRunning_(false), hasError_(false),
          reconnectAttempts_(0), maxReconnectAttempts_(5),
          lastFrameTime_(0), noDataTimeout_(10000000), // 10秒无数据视为停滞
          isReconnecting_(false), inputFormatContext_(nullptr),
          outputFormatContext_(nullptr) {

    // 验证配置
    if (!config.validate()) {
        throw FFmpegException("Invalid stream configuration for: " + config.id);
    }

    // 检查配置中是否有重连相关参数
    for (const auto& [key, value] : config.extraOptions) {
        if (key == "maxReconnectAttempts") {
            maxReconnectAttempts_ = std::stoi(value);
        } else if (key == "noDataTimeout") {
            // 配置中以毫秒为单位，转换为微秒
            noDataTimeout_ = std::stoll(value) * 1000;
        }
    }

    // 初始化FFmpeg网络功能
    static bool ffmpegInitialized = false;
    if (!ffmpegInitialized) {
        avformat_network_init();
        ffmpegInitialized = true;
    }
}

StreamProcessor::~StreamProcessor() {
    try {
        stop();
        cleanup();
    } catch (const std::exception& e) {
        Logger::error("Exception in StreamProcessor destructor: " + std::string(e.what()));
    } catch (...) {
        Logger::error("Unknown exception in StreamProcessor destructor");
    }
}

void StreamProcessor::start() {
    if (isRunning_) {
        Logger::warning("Stream already running: " + config_.id);
        return;
    }

    try {
        initialize();
        isRunning_ = true;
        hasError_ = false;
        reconnectAttempts_ = 0;  // 重置重连计数器
        lastFrameTime_ = av_gettime(); // 初始化最后一帧时间

        processingThread_ = std::thread(&StreamProcessor::processLoop, this);
        Logger::info("Started stream processing: " + config_.id + " (" +
                     config_.inputUrl + " -> " + config_.outputUrl + ")");
    } catch (const FFmpegException& e) {
        // 改变这里的处理方式，标记为错误但不重新抛出异常
        Logger::error("Failed to start stream " + config_.id + ": " + std::string(e.what()));
        cleanup();
        hasError_ = true;
        isRunning_ = false;
        // 不要在这里重新抛出异常，让调用者认为流已启动，
        // 然后由监控系统检测到错误并尝试重连
    }
}

void StreamProcessor::stop() {
    // 使用局部变量，避免多次设置原子变量
    bool wasRunning = isRunning_.exchange(false);

    if (wasRunning && processingThread_.joinable()) {
        try {
            processingThread_.join();
            Logger::info("Processing thread joined for stream: " + config_.id);
        } catch (const std::exception& e) {
            Logger::error("Exception joining processing thread: " + std::string(e.what()));
            // 不抛出异常，因为我们正在清理
        }
    }
}

bool StreamProcessor::isRunning() const {
    return isRunning_;
}

bool StreamProcessor::hasError() const {
    return hasError_;
}

const StreamConfig& StreamProcessor::getConfig() const {
    return config_;
}

bool StreamProcessor::updateConfig(const StreamConfig& config) {
    if (isRunning_) {
        Logger::warning("Cannot update config while stream is running: " + config_.id);
        return false;
    }

    if (!config.validate()) {
        Logger::error("Invalid stream configuration for: " + config.id);
        return false;
    }

    config_ = config;
    return true;
}

void StreamProcessor::initialize() {
    // 打开输入
    openInput();

    // 打开输出
    openOutput();

    // 为每个流设置编解码器
    setupStreams();

    // 写输出头
    int ret = avformat_write_header(outputFormatContext_, nullptr);
    if (ret < 0) {
        throw FFmpegException("Failed to write output header", ret);
    }
}

void StreamProcessor::openInput() {
    int ret;

    // 分配输入上下文
    inputFormatContext_ = avformat_alloc_context();
    if (!inputFormatContext_) {
        throw FFmpegException("Failed to allocate input format context");
    }

    // 创建选项字典
    AVDictionary* options = nullptr;

    // 增加探测大小和分析持续时间，解决"not enough frames to estimate rate"问题
    // 默认值 - 可以根据需要增大这些值
    av_dict_set(&options, "probesize", "10485760", 0);     // 10MB (默认是5MB)
    av_dict_set(&options, "analyzeduration", "5000000", 0); // 5秒 (默认是0.5秒)

    // 设置低延迟选项
    if (config_.lowLatencyMode) {
        inputFormatContext_->flags |= AVFMT_FLAG_NOBUFFER | AVFMT_FLAG_FLUSH_PACKETS;

        // 针对RTSP流增加额外选项
        if (config_.inputUrl.find("rtsp://") == 0) {
            // RTSP特定选项
            av_dict_set(&options, "rtsp_transport", "tcp", 0);  // 使用TCP传输RTSP (比UDP更可靠)
            av_dict_set(&options, "stimeout", "5000000", 0);    // Socket超时 5秒
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
    ret = avformat_open_input(&inputFormatContext_, config_.inputUrl.c_str(), nullptr, &options);

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
        throw FFmpegException("Failed to open input: " + config_.inputUrl, ret);
    }

    // 查找流信息 - 增加选项以更好地检测流
    AVDictionary* streamInfoOptions = nullptr;
    if (config_.inputUrl.find("rtsp://") == 0) {
        // 对于RTSP流，可能需要更多帧来准确检测流信息
        av_dict_set(&streamInfoOptions, "analyzeduration", "10000000", 0); // 10秒
    }

    ret = avformat_find_stream_info(inputFormatContext_, streamInfoOptions ? &streamInfoOptions : nullptr);
    av_dict_free(&streamInfoOptions);

    if (ret < 0) {
        throw FFmpegException("Failed to find stream info", ret);
    }

    // 输出找到的流信息
    Logger::debug("Input stream information:");
    for (unsigned int i = 0; i < inputFormatContext_->nb_streams; i++) {
        AVStream* stream = inputFormatContext_->streams[i];

        // 获取解码器名称 - 使用avcodec_get_name而不是avcodec_string
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

    Logger::info("Successfully opened input: " + config_.inputUrl);
}

void StreamProcessor::openOutput() {
    int ret;

    // 分配输出上下文
    ret = avformat_alloc_output_context2(&outputFormatContext_, nullptr,
                                         config_.outputFormat.c_str(),
                                         config_.outputUrl.c_str());
    if (ret < 0 || !outputFormatContext_) {
        throw FFmpegException("Failed to allocate output context", ret);
    }

    // 打开输出文件/URL
    if (!(outputFormatContext_->oformat->flags & AVFMT_NOFILE)) {
        // 设置额外输出选项
        AVDictionary* options = nullptr;
        for (const auto& [key, value] : config_.extraOptions) {
            if (key.find("output_") == 0) {
                std::string optionName = key.substr(7); // 去除"output_"前缀
                av_dict_set(&options, optionName.c_str(), value.c_str(), 0);
            }
        }

        ret = avio_open2(&outputFormatContext_->pb, config_.outputUrl.c_str(),
                         AVIO_FLAG_WRITE, nullptr, &options);
        av_dict_free(&options);

        if (ret < 0) {
            throw FFmpegException("Failed to open output URL: " + config_.outputUrl, ret);
        }
    }

    Logger::info("Successfully prepared output: " + config_.outputUrl);
}

AVBufferRef* StreamProcessor::createHardwareDevice() {
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

// getEncoderByHardwareType方法修改 - 保证兼容性
const AVCodec* StreamProcessor::getEncoderByHardwareType(AVHWDeviceType hwType, AVCodecID codecId) {
    std::string encoderName;
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

void StreamProcessor::setupStreams() {
    // 为每个流创建上下文
    for (unsigned int i = 0; i < inputFormatContext_->nb_streams; i++) {
        AVStream* inputStream = inputFormatContext_->streams[i];

        if (inputStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            setupVideoStream(inputStream);
        } else if (inputStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            setupAudioStream(inputStream);
        } else {
            // 其他类型的流（字幕等）暂不处理
            Logger::debug("Ignoring stream of type: " +
                          std::to_string(inputStream->codecpar->codec_type));
        }
    }

    if (videoStreams_.empty() && audioStreams_.empty()) {
        throw FFmpegException("No supported streams found in input");
    }
}

void StreamProcessor::setupVideoStream(AVStream* inputStream) {
    int ret;
    StreamContext streamCtx;
    streamCtx.streamIndex = inputStream->index;
    streamCtx.inputStream = inputStream;

    // 查找解码器
    const AVCodec* decoder = avcodec_find_decoder(inputStream->codecpar->codec_id);
    if (!decoder) {
        Logger::warning("Unsupported video codec");
        return;
    }

    // 分配解码器上下文
    streamCtx.decoderContext = avcodec_alloc_context3(decoder);
    if (!streamCtx.decoderContext) {
        throw FFmpegException("Failed to allocate decoder context");
    }

    // 从输入流复制编解码器参数
    ret = avcodec_parameters_to_context(streamCtx.decoderContext, inputStream->codecpar);
    if (ret < 0) {
        throw FFmpegException("Failed to copy decoder parameters", ret);
    }

    // 设置硬件加速（如果启用）
    AVHWDeviceType hwType = AV_HWDEVICE_TYPE_NONE;
    if (config_.enableHardwareAccel && !config_.hwaccelType.empty() &&
        config_.hwaccelType != "none") {

        streamCtx.hwDeviceContext = createHardwareDevice();

        if (streamCtx.hwDeviceContext) {
            // 确保解码上下文已经分配
            if (streamCtx.decoderContext) {
                streamCtx.decoderContext->hw_device_ctx = av_buffer_ref(streamCtx.hwDeviceContext);
                hwType = ((AVHWDeviceContext*)streamCtx.hwDeviceContext->data)->type;
            } else {
                Logger::warning("Decoder context not initialized, cannot attach hardware device");
            }
        }
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

    ret = avcodec_open2(streamCtx.decoderContext, decoder, &decoderOptions);
    av_dict_free(&decoderOptions);
    if (ret < 0) {
        if (streamCtx.decoderContext->hw_device_ctx) {
            // 如果硬件解码失败，尝试回退到软件解码
            Logger::warning("Hardware-accelerated decoding failed, falling back to software decoding");
            av_buffer_unref(&streamCtx.decoderContext->hw_device_ctx);
            streamCtx.decoderContext->hw_device_ctx = nullptr;

            // 重新打开解码器，这次不使用硬件加速
            ret = avcodec_open2(streamCtx.decoderContext, decoder, nullptr);
            if (ret < 0) {
                throw FFmpegException("Failed to open video decoder even with software fallback", ret);
            }
        } else {
            throw FFmpegException("Failed to open video decoder", ret);
        }
    }

    // 创建输出流
    AVStream* outputStream = avformat_new_stream(outputFormatContext_, nullptr);
    if (!outputStream) {
        throw FFmpegException("Failed to create output video stream");
    }

    streamCtx.outputStream = outputStream;

    // 根据硬件加速类型自动选择适合的编码器
    const AVCodec* encoder = nullptr;
    AVCodecID codecId = AV_CODEC_ID_H264; // 默认H.264

    // 从配置中查找指定的编解码器ID
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
    streamCtx.encoderContext = avcodec_alloc_context3(encoder);
    if (!streamCtx.encoderContext) {
        throw FFmpegException("Failed to allocate encoder context");
    }

    // 设置编码参数
    streamCtx.encoderContext->codec_id = encoder->id;
    streamCtx.encoderContext->codec_type = AVMEDIA_TYPE_VIDEO;

    // 设置分辨率
    if (config_.width > 0 && config_.height > 0) {
        streamCtx.encoderContext->width = config_.width;
        streamCtx.encoderContext->height = config_.height;
    } else {
        streamCtx.encoderContext->width = streamCtx.decoderContext->width;
        streamCtx.encoderContext->height = streamCtx.decoderContext->height;
    }

    // 设置像素格式 - 修改为更兼容的方法
    if (streamCtx.hwDeviceContext) {
        AVHWFramesConstraints* constraints =
                av_hwdevice_get_hwframe_constraints(streamCtx.hwDeviceContext, nullptr);

        if (constraints) {
            // 检查编码器是否支持硬件像素格式
            bool foundCompatibleFormat = false;
            if (constraints->valid_hw_formats) {
                int i = 0;
                while (constraints->valid_hw_formats[i] != AV_PIX_FMT_NONE) {
                    AVPixelFormat hwFormat = constraints->valid_hw_formats[i];

                    // 对于某些编码器，特别检查它们支持的像素格式
                    if (isCodecSupported(encoder, hwFormat)) {
                        streamCtx.encoderContext->pix_fmt = hwFormat;
                        foundCompatibleFormat = true;
                        break;
                    }
//                    if (encoder->pix_fmts) {
//                        int j = 0;
//                        while (encoder->pix_fmts[j] != AV_PIX_FMT_NONE) {
//                            if (encoder->pix_fmts[j] == hwFormat) {
//                                streamCtx.encoderContext->pix_fmt = hwFormat;
//                                foundCompatibleFormat = true;
//                                break;
//                            }
//                            j++;
//                        }
//                        if (foundCompatibleFormat) break;
//                    }
                    i++;
                }
            }

            if (!foundCompatibleFormat) {
                // 如果没有找到兼容的硬件格式，使用软件格式
                streamCtx.encoderContext->pix_fmt = AV_PIX_FMT_YUV420P;
                Logger::warning("No compatible hardware pixel format found, using software format");
            }

            av_hwframe_constraints_free(&constraints);
        } else {
            // 无法获取约束，使用默认软件格式
            streamCtx.encoderContext->pix_fmt = AV_PIX_FMT_YUV420P;
        }
    } else {
        // 使用编码器支持的第一个像素格式
        if (encoder->pix_fmts) {
            streamCtx.encoderContext->pix_fmt = encoder->pix_fmts[0];
        } else {
            streamCtx.encoderContext->pix_fmt = AV_PIX_FMT_YUV420P; // 默认
        }
    }

    // 设置码率
    streamCtx.encoderContext->bit_rate = config_.videoBitrate;

    // 设置时间基准
    streamCtx.encoderContext->time_base = av_inv_q(inputStream->r_frame_rate);
    outputStream->time_base = streamCtx.encoderContext->time_base;

    // 设置GOP（关键帧间隔）
    streamCtx.encoderContext->gop_size = config_.keyframeInterval;
    streamCtx.encoderContext->max_b_frames = 1;

    // 设置低延迟参数
    if (config_.lowLatencyMode) {
        streamCtx.encoderContext->flags |= AV_CODEC_FLAG_LOW_DELAY;

        // 特定编码器的低延迟参数
        if (encoder->id == AV_CODEC_ID_H264 || encoder->id == AV_CODEC_ID_HEVC) {
            av_opt_set(streamCtx.encoderContext->priv_data, "preset", "ultrafast", 0);
            av_opt_set(streamCtx.encoderContext->priv_data, "tune", "zerolatency", 0);
            // 使用baseline profile提高兼容性
            if (encoder->id == AV_CODEC_ID_H264) {
                av_opt_set(streamCtx.encoderContext->priv_data, "profile", "baseline", 0);
            }
        } else if (encoder->id == AV_CODEC_ID_VP8 || encoder->id == AV_CODEC_ID_VP9) {
            av_opt_set_int(streamCtx.encoderContext->priv_data, "lag-in-frames", 0, 0);
            av_opt_set_int(streamCtx.encoderContext->priv_data, "deadline", 1, 0);  // 实时模式
        }
    }

    // 设置缓冲区大小
    streamCtx.encoderContext->rc_buffer_size = config_.bufferSize;
    streamCtx.encoderContext->rc_max_rate = config_.videoBitrate * 2;

    // 将硬件帧上下文连接到编码器（如果使用硬件加速）
    if (streamCtx.hwDeviceContext) {
        AVBufferRef* hwFramesContext = av_hwframe_ctx_alloc(streamCtx.hwDeviceContext);
        if (hwFramesContext) {
            AVHWFramesContext* framesCtx = (AVHWFramesContext*)hwFramesContext->data;
            framesCtx->format = streamCtx.encoderContext->pix_fmt;
            framesCtx->sw_format = AV_PIX_FMT_YUV420P;
            framesCtx->width = streamCtx.encoderContext->width;
            framesCtx->height = streamCtx.encoderContext->height;

            ret = av_hwframe_ctx_init(hwFramesContext);
            if (ret < 0) {
                av_buffer_unref(&hwFramesContext);
                Logger::warning("Failed to initialize hardware frames context, falling back to software");
            } else {
                streamCtx.encoderContext->hw_frames_ctx = hwFramesContext;
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
    ret = avcodec_open2(streamCtx.encoderContext, encoder, &encoderOptions);
    av_dict_free(&encoderOptions);
    if (ret < 0) {
        throw FFmpegException("Failed to open video encoder", ret);
    }

    // 从编码器到输出流复制参数
    ret = avcodec_parameters_from_context(outputStream->codecpar, streamCtx.encoderContext);
    if (ret < 0) {
        throw FFmpegException("Failed to copy encoder parameters to output stream", ret);
    }

    // 分配包和帧
    streamCtx.packet = av_packet_alloc();
    streamCtx.frame = av_frame_alloc();
    streamCtx.hwFrame = av_frame_alloc();

    if (!streamCtx.packet || !streamCtx.frame || !streamCtx.hwFrame) {
        throw FFmpegException("Failed to allocate packet or frame");
    }

    // 添加到视频流列表
    videoStreams_.push_back(streamCtx);
    Logger::info("Set up video stream: input#" + std::to_string(inputStream->index) +
                 " -> output#" + std::to_string(outputStream->index) +
                 " (" + std::string(encoder->name) + ")");
}

void StreamProcessor::setupAudioStream(AVStream* inputStream) {
    int ret;
    StreamContext streamCtx;
    streamCtx.streamIndex = inputStream->index;
    streamCtx.inputStream = inputStream;

    // 查找解码器
    const AVCodec* decoder = avcodec_find_decoder(inputStream->codecpar->codec_id);
    if (!decoder) {
        Logger::warning("Unsupported audio codec");
        return;
    }

    // 分配解码器上下文
    streamCtx.decoderContext = avcodec_alloc_context3(decoder);
    if (!streamCtx.decoderContext) {
        throw FFmpegException("Failed to allocate audio decoder context");
    }

    // 从输入流复制编解码器参数
    ret = avcodec_parameters_to_context(streamCtx.decoderContext, inputStream->codecpar);
    if (ret < 0) {
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
    ret = avcodec_open2(streamCtx.decoderContext, decoder, &decoderOptions);
    av_dict_free(&decoderOptions);
    if (ret < 0) {
        throw FFmpegException("Failed to open audio decoder", ret);
    }

    // 创建输出流
    AVStream* outputStream = avformat_new_stream(outputFormatContext_, nullptr);
    if (!outputStream) {
        throw FFmpegException("Failed to create output audio stream");
    }

    streamCtx.outputStream = outputStream;

    // 自动选择音频编码器
    const AVCodec* encoder = nullptr;
    AVCodecID audioCodecId = AV_CODEC_ID_AAC; // 默认AAC

    // 从配置中查找指定的编解码器ID
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

    // 为特定格式选择合适的编码器
    if (outputFormatContext_->oformat) {
        if (outputFormatContext_->oformat->name &&
            strcmp(outputFormatContext_->oformat->name, "flv") == 0) {
            // FLV格式建议使用AAC
            audioCodecId = AV_CODEC_ID_AAC;
        } else if (outputFormatContext_->oformat->name &&
                   strcmp(outputFormatContext_->oformat->name, "hls") == 0) {
            // HLS也建议使用AAC
            audioCodecId = AV_CODEC_ID_AAC;
        }
    }

    // 尝试找到合适的编码器
    if (audioCodecId == AV_CODEC_ID_AAC) {
        // 按优先级尝试不同的AAC编码器
        const char* aacEncoders[] = {"aac", "libfdk_aac", "libfaac"};
        for (const char* encoderName : aacEncoders) {
            encoder = avcodec_find_encoder_by_name(encoderName);
            if (encoder) break;
        }
    }

    // 如果未找到特定编码器，使用默认编码器
    if (!encoder) {
        encoder = avcodec_find_encoder(audioCodecId);
    }

    if (!encoder) {
        // 最后尝试AAC
        encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!encoder) {
            throw FFmpegException("Failed to find suitable audio encoder");
        }
    }

    Logger::info("Using audio encoder: " + std::string(encoder->name));

    // 分配编码器上下文
    streamCtx.encoderContext = avcodec_alloc_context3(encoder);
    if (!streamCtx.encoderContext) {
        throw FFmpegException("Failed to allocate audio encoder context");
    }

    // 设置编码参数
    streamCtx.encoderContext->codec_id = encoder->id;
    streamCtx.encoderContext->codec_type = AVMEDIA_TYPE_AUDIO;
    streamCtx.encoderContext->sample_rate = streamCtx.decoderContext->sample_rate;

    // 设置声道布局
    if (encoder->channel_layouts) {
        // 使用编码器支持的声道布局
        uint64_t inputLayout = streamCtx.decoderContext->channel_layout ?
                               streamCtx.decoderContext->channel_layout :
                               av_get_default_channel_layout(streamCtx.decoderContext->channels);

        streamCtx.encoderContext->channel_layout = inputLayout;

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
            streamCtx.encoderContext->channel_layout = encoder->channel_layouts[0];
        }
    } else {
        // 使用输入的声道布局
        streamCtx.encoderContext->channel_layout =
                streamCtx.decoderContext->channel_layout ?
                streamCtx.decoderContext->channel_layout :
                av_get_default_channel_layout(streamCtx.decoderContext->channels);
    }

    streamCtx.encoderContext->channels = av_get_channel_layout_nb_channels(
            streamCtx.encoderContext->channel_layout);

    // 设置采样格式
    if (encoder->sample_fmts) {
        streamCtx.encoderContext->sample_fmt = encoder->sample_fmts[0];
    } else {
        streamCtx.encoderContext->sample_fmt = AV_SAMPLE_FMT_FLTP;
    }

    // 设置码率
    streamCtx.encoderContext->bit_rate = config_.audioBitrate;

    // 设置时间基准
    streamCtx.encoderContext->time_base = {1, streamCtx.encoderContext->sample_rate};
    outputStream->time_base = streamCtx.encoderContext->time_base;

    // 设置编码器选项
    AVDictionary* encoderOptions = nullptr;
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("audio_encoder_") == 0) {
            std::string optionName = key.substr(14); // 去除"audio_encoder_"前缀
            av_dict_set(&encoderOptions, optionName.c_str(), value.c_str(), 0);
        }
    }

    // 打开编码器
    ret = avcodec_open2(streamCtx.encoderContext, encoder, &encoderOptions);
    av_dict_free(&encoderOptions);
    if (ret < 0) {
        throw FFmpegException("Failed to open audio encoder", ret);
    }

    // 从编码器到输出流复制参数
    ret = avcodec_parameters_from_context(outputStream->codecpar, streamCtx.encoderContext);
    if (ret < 0) {
        throw FFmpegException("Failed to copy encoder parameters to output stream", ret);
    }

    // 分配包和帧
    streamCtx.packet = av_packet_alloc();
    streamCtx.frame = av_frame_alloc();

    if (!streamCtx.packet || !streamCtx.frame) {
        throw FFmpegException("Failed to allocate packet or frame");
    }

    // 添加到音频流列表
    audioStreams_.push_back(streamCtx);
    Logger::info("Set up audio stream: input#" + std::to_string(inputStream->index) +
                 " -> output#" + std::to_string(outputStream->index) +
                 " (" + std::string(encoder->name) + ")");
}

// 添加到processLoop方法中，增强调试信息输出

void StreamProcessor::processLoop() {
    AVPacket* packet = av_packet_alloc();
    int64_t lastLogTime = 0;
    int frameCount = 0;
    int streamErrorCount = 0;
    const int MAX_ERRORS_BEFORE_RECONNECT = 20; // 连续错误阈值减小，触发重连更敏感
    bool reconnectInProgress = false;

    // 记录开始处理时间
    int64_t startTime = av_gettime() / 1000000;
    lastFrameTime_ = av_gettime(); // 初始化最后一帧时间
    Logger::info("Starting process loop for stream: " + config_.id);

    // 输出更多的流信息
    if (inputFormatContext_ && outputFormatContext_) {
        Logger::debug("Input format: " + std::string(inputFormatContext_->iformat->name) +
                      " (" + (inputFormatContext_->iformat->long_name ? inputFormatContext_->iformat->long_name : "Unknown") + ")");
        Logger::debug("Output format: " + std::string(outputFormatContext_->oformat->name) +
                      " (" + (outputFormatContext_->oformat->long_name ? outputFormatContext_->oformat->long_name : "Unknown") + ")");

        if (!videoStreams_.empty()) {
            Logger::debug("Video streams: " + std::to_string(videoStreams_.size()));
            for (size_t i = 0; i < videoStreams_.size(); i++) {
                const auto& vs = videoStreams_[i];
                if (vs.encoderContext && vs.encoderContext->codec) {
                    Logger::debug("  Video encoder #" + std::to_string(i) + ": " + vs.encoderContext->codec->name +
                                  " (" + (vs.encoderContext->codec->long_name ? vs.encoderContext->codec->long_name : "Unknown") + ")");
                    Logger::debug("    Resolution: " + std::to_string(vs.encoderContext->width) + "x" +
                                  std::to_string(vs.encoderContext->height) + ", bitrate: " + std::to_string(vs.encoderContext->bit_rate) + " bps");
                }
            }
        }

        if (!audioStreams_.empty()) {
            Logger::debug("Audio streams: " + std::to_string(audioStreams_.size()));
            for (size_t i = 0; i < audioStreams_.size(); i++) {
                const auto& as = audioStreams_[i];
                if (as.encoderContext && as.encoderContext->codec) {
                    Logger::debug("  Audio encoder #" + std::to_string(i) + ": " + as.encoderContext->codec->name +
                                  " (" + (as.encoderContext->codec->long_name ? as.encoderContext->codec->long_name : "Unknown") + ")");
                    Logger::debug("    Sample rate: " + std::to_string(as.encoderContext->sample_rate) +
                                  " Hz, channels: " + std::to_string(as.encoderContext->channels) +
                                  ", bitrate: " + std::to_string(as.encoderContext->bit_rate) + " bps");
                }
            }
        }
    }

    while (isRunning_) {
        try {
            // 检查流是否停滞
            if (isStreamStalled() && !reconnectInProgress) {
                Logger::warning("Stream " + config_.id + " appears stalled (no data for "
                                + std::to_string(noDataTimeout_ / 1000000) + " seconds)");

                reconnectInProgress = true;
                if (!reconnect()) {
                    // 重连失败，设置错误并退出循环
                    hasError_ = true;
                    break;
                }
                reconnectInProgress = false;
                streamErrorCount = 0;
                // 重连后继续循环
                continue;
            }

            // 读取输入包
            int ret = av_read_frame(inputFormatContext_, packet);
            if (ret < 0) {
                if (ret == AVERROR_EOF) {
                    // 输入流结束
                    Logger::info("End of input stream: " + config_.id);
                    break;
                } else if (ret == AVERROR(EAGAIN)) {
                    // 暂时没有更多数据，等待一下
                    av_usleep(10000);  // 休眠10ms
                    continue;
                } else {
                    streamErrorCount++;
                    if (streamErrorCount > MAX_ERRORS_BEFORE_RECONNECT && !reconnectInProgress) {
                        Logger::warning("Too many consecutive errors (" + std::to_string(streamErrorCount)
                                        + "), attempting reconnection");

                        reconnectInProgress = true;
                        if (!reconnect()) {
                            // 重连失败，设置错误并退出循环
                            hasError_ = true;
                            break;
                        }
                        reconnectInProgress = false;
                        streamErrorCount = 0;
                        // 重连后继续循环
                        continue;
                    }

                    Logger::warning("Error reading frame (attempt " + std::to_string(streamErrorCount) +
                                    "/" + std::to_string(MAX_ERRORS_BEFORE_RECONNECT) + "): " +
                                    std::to_string(ret));
                    av_usleep(100000);  // 错误后休眠100ms
                    continue;
                }
            }

            // 更新最后一帧时间
            lastFrameTime_ = av_gettime();

            // 重置错误计数
            streamErrorCount = 0;

            // 处理视频包
            bool packetProcessed = false;
            for (auto& streamCtx : videoStreams_) {
                if (packet->stream_index == streamCtx.streamIndex) {
                    processVideoPacket(packet, streamCtx);
                    packetProcessed = true;
                    frameCount++;
                    break;
                }
            }

            // 处理音频包
            if (!packetProcessed) {
                for (auto& streamCtx : audioStreams_) {
                    if (packet->stream_index == streamCtx.streamIndex) {
                        processAudioPacket(packet, streamCtx);
                        packetProcessed = true;
                        break;
                    }
                }
            }

            // 每5秒记录一次状态
            int64_t currentTime = av_gettime() / 1000000;
            if (currentTime - lastLogTime >= 5) {
                // 计算运行时间和平均帧率
                int64_t runTime = currentTime - startTime;
                double avgFps = runTime > 0 ? (double)frameCount / runTime : 0;

                Logger::debug("Stream " + config_.id + " processed " +
                              std::to_string(frameCount) + " frames, avg. " +
                              std::to_string(avgFps) + " fps (running for " +
                              std::to_string(runTime) + " seconds)");

                lastLogTime = currentTime;
            }

            // 释放包
            av_packet_unref(packet);

        } catch (const FFmpegException& e) {
            Logger::error("Stream processing error: " + std::string(e.what()));
            av_packet_unref(packet);

            if (!reconnectInProgress && isRunning_) {
                // 尝试重连
                Logger::warning("Stream error detected, attempting reconnection");
                reconnectInProgress = true;
                if (!reconnect()) {
                    // 重连失败，设置错误并退出循环
                    hasError_ = true;
                    isRunning_ = false;
                    break;
                }
                reconnectInProgress = false;
                streamErrorCount = 0;
            } else {
                // 如果已经在重连中或流已停止，则退出
                hasError_ = true;
                isRunning_ = false;
                break;
            }
        }
    }

    // 清理
    av_packet_free(&packet);

    // 写入尾部
    if (outputFormatContext_) {
        av_write_trailer(outputFormatContext_);
    }

    // 计算总处理时间和平均帧率
    int64_t endTime = av_gettime() / 1000000;
    int64_t totalTime = endTime - startTime;
    double avgFps = totalTime > 0 ? (double)frameCount / totalTime : 0;

    Logger::info("Exiting process loop for stream: " + config_.id + " after " +
                 std::to_string(totalTime) + " seconds, processed " +
                 std::to_string(frameCount) + " frames (avg. " +
                 std::to_string(avgFps) + " fps)");
}

void StreamProcessor::processVideoPacket(AVPacket* packet, StreamContext& streamCtx) {
    int ret;

    // 发送包到解码器
    ret = avcodec_send_packet(streamCtx.decoderContext, packet);
    if (ret < 0) {
        throw FFmpegException("Error sending packet to decoder", ret);
    }

    while (ret >= 0) {
        // 接收解码帧
        ret = avcodec_receive_frame(streamCtx.decoderContext, streamCtx.frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            throw FFmpegException("Error receiving frame from decoder", ret);
        }

        AVFrame* frameToEncode = streamCtx.frame;

        // 处理硬件加速的帧
        if (streamCtx.frame->format == AV_PIX_FMT_CUDA ||
            streamCtx.frame->format == AV_PIX_FMT_VAAPI ||
            streamCtx.frame->format == AV_PIX_FMT_QSV ||
            streamCtx.frame->format == AV_PIX_FMT_D3D11 ||
            streamCtx.frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {

            // 从硬件帧转换为软件帧（如果需要）
            if (!streamCtx.encoderContext->hw_frames_ctx) {
                ret = av_hwframe_transfer_data(streamCtx.hwFrame, streamCtx.frame, 0);
                if (ret < 0) {
                    throw FFmpegException("Error transferring data from hardware frame", ret);
                }
                frameToEncode = streamCtx.hwFrame;
            }
        }

        // 如果需要缩放
        if (streamCtx.encoderContext->width != frameToEncode->width ||
            streamCtx.encoderContext->height != frameToEncode->height ||
            streamCtx.encoderContext->pix_fmt != frameToEncode->format) {

            // 如果swsContext尚未初始化，则初始化
            if (!streamCtx.swsContext) {
                streamCtx.swsContext = sws_getContext(
                        frameToEncode->width, frameToEncode->height, (AVPixelFormat)frameToEncode->format,
                        streamCtx.encoderContext->width, streamCtx.encoderContext->height,
                        streamCtx.encoderContext->pix_fmt,
                        SWS_BILINEAR, nullptr, nullptr, nullptr
                );

                if (!streamCtx.swsContext) {
                    throw FFmpegException("Failed to create scaling context");
                }
            }

            // 分配转换后的帧
            if (!streamCtx.hwFrame->data[0]) {
                streamCtx.hwFrame->format = streamCtx.encoderContext->pix_fmt;
                streamCtx.hwFrame->width = streamCtx.encoderContext->width;
                streamCtx.hwFrame->height = streamCtx.encoderContext->height;
                ret = av_frame_get_buffer(streamCtx.hwFrame, 0);
                if (ret < 0) {
                    throw FFmpegException("Failed to allocate frame buffer for scaling", ret);
                }
            }

            // 执行缩放
            ret = sws_scale(streamCtx.swsContext, frameToEncode->data, frameToEncode->linesize, 0,
                            frameToEncode->height, streamCtx.hwFrame->data, streamCtx.hwFrame->linesize);
            if (ret < 0) {
                throw FFmpegException("Error during frame scaling", ret);
            }

            streamCtx.hwFrame->pts = frameToEncode->pts;
            frameToEncode = streamCtx.hwFrame;
        }

        // 重新计算PTS
        frameToEncode->pts = av_rescale_q(
                frameToEncode->pts,
                streamCtx.decoderContext->time_base,
                streamCtx.encoderContext->time_base
        );

        // 发送帧到编码器
        ret = avcodec_send_frame(streamCtx.encoderContext, frameToEncode);
        if (ret < 0) {
            throw FFmpegException("Error sending frame to encoder", ret);
        }

        while (ret >= 0) {
            // 接收编码包
            AVPacket* encodedPacket = av_packet_alloc();
            ret = avcodec_receive_packet(streamCtx.encoderContext, encodedPacket);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&encodedPacket);
                break;
            } else if (ret < 0) {
                av_packet_free(&encodedPacket);
                throw FFmpegException("Error receiving packet from encoder", ret);
            }

            // 准备要写入的包
            encodedPacket->stream_index = streamCtx.outputStream->index;

            // 重新计算时间戳
            av_packet_rescale_ts(
                    encodedPacket,
                    streamCtx.encoderContext->time_base,
                    streamCtx.outputStream->time_base
            );

            // 写入包
            ret = av_interleaved_write_frame(outputFormatContext_, encodedPacket);
            av_packet_free(&encodedPacket);

            if (ret < 0) {
                throw FFmpegException("Error writing encoded packet", ret);
            }
        }

        // 不再需要帧时释放它
        av_frame_unref(streamCtx.frame);
        if (streamCtx.hwFrame->data[0]) {
            av_frame_unref(streamCtx.hwFrame);
        }
    }
}

void StreamProcessor::processAudioPacket(AVPacket* packet, StreamContext& streamCtx) {
    int ret;

    // 发送包到解码器
    ret = avcodec_send_packet(streamCtx.decoderContext, packet);
    if (ret < 0) {
        throw FFmpegException("Error sending packet to audio decoder", ret);
    }

    while (ret >= 0) {
        // 接收解码帧
        ret = avcodec_receive_frame(streamCtx.decoderContext, streamCtx.frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            throw FFmpegException("Error receiving frame from audio decoder", ret);
        }

        // 重新计算PTS
        streamCtx.frame->pts = av_rescale_q(
                streamCtx.frame->pts,
                streamCtx.decoderContext->time_base,
                streamCtx.encoderContext->time_base
        );

        // 发送帧到编码器
        ret = avcodec_send_frame(streamCtx.encoderContext, streamCtx.frame);
        if (ret < 0) {
            throw FFmpegException("Error sending frame to audio encoder", ret);
        }

        while (ret >= 0) {
            // 接收编码包
            AVPacket* encodedPacket = av_packet_alloc();
            ret = avcodec_receive_packet(streamCtx.encoderContext, encodedPacket);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&encodedPacket);
                break;
            } else if (ret < 0) {
                av_packet_free(&encodedPacket);
                throw FFmpegException("Error receiving packet from audio encoder", ret);
            }

            // 准备要写入的包
            encodedPacket->stream_index = streamCtx.outputStream->index;

            // 重新计算时间戳
            av_packet_rescale_ts(
                    encodedPacket,
                    streamCtx.encoderContext->time_base,
                    streamCtx.outputStream->time_base
            );

            // 写入包
            ret = av_interleaved_write_frame(outputFormatContext_, encodedPacket);
            av_packet_free(&encodedPacket);

            if (ret < 0) {
                throw FFmpegException("Error writing encoded audio packet", ret);
            }
        }

        // 不再需要帧时释放它
        av_frame_unref(streamCtx.frame);
    }
}

void StreamProcessor::cleanup() {
    // 清理视频流
    for (auto& streamCtx : videoStreams_) {
        if (streamCtx.packet) {
            av_packet_free(&streamCtx.packet);
        }

        if (streamCtx.frame) {
            av_frame_free(&streamCtx.frame);
        }

        if (streamCtx.hwFrame) {
            av_frame_free(&streamCtx.hwFrame);
        }

        if (streamCtx.swsContext) {
            sws_freeContext(streamCtx.swsContext);
            streamCtx.swsContext = nullptr;
        }

        if (streamCtx.decoderContext) {
            avcodec_free_context(&streamCtx.decoderContext);
        }

        if (streamCtx.encoderContext) {
            avcodec_free_context(&streamCtx.encoderContext);
        }

        if (streamCtx.hwDeviceContext) {
            av_buffer_unref(&streamCtx.hwDeviceContext);
        }
    }

    // 清理音频流
    for (auto& streamCtx : audioStreams_) {
        if (streamCtx.packet) {
            av_packet_free(&streamCtx.packet);
        }

        if (streamCtx.frame) {
            av_frame_free(&streamCtx.frame);
        }

        if (streamCtx.decoderContext) {
            avcodec_free_context(&streamCtx.decoderContext);
        }

        if (streamCtx.encoderContext) {
            avcodec_free_context(&streamCtx.encoderContext);
        }
    }

    // 清理输入上下文
    if (inputFormatContext_) {
        avformat_close_input(&inputFormatContext_);
    }

    // 清理输出上下文
    if (outputFormatContext_) {
        if (!(outputFormatContext_->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&outputFormatContext_->pb);
        }
        avformat_free_context(outputFormatContext_);
        outputFormatContext_ = nullptr;
    }

    // 清空流列表
    videoStreams_.clear();
    audioStreams_.clear();
}


// 将这个辅助函数添加到StreamProcessor类
bool StreamProcessor::isCodecSupported(const AVCodec* codec, AVPixelFormat pixFmt) {
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

// 添加重置错误状态的方法
void StreamProcessor::resetErrorState() {
    hasError_ = false;
    reconnectAttempts_ = 0;
    lastFrameTime_ = av_gettime(); // 重置最后一帧时间
}

// 添加检查流是否停滞的方法
bool StreamProcessor::isStreamStalled() const {
    if (lastFrameTime_ == 0) return false; // 流尚未开始

    int64_t currentTime = av_gettime();
    return (currentTime - lastFrameTime_) > noDataTimeout_;
}

// 添加重连方法
bool StreamProcessor::reconnect() {
    if (reconnectAttempts_ >= maxReconnectAttempts_) {
        Logger::error("Stream " + config_.id + " exceeded maximum reconnection attempts ("
                      + std::to_string(maxReconnectAttempts_) + ")");
        return false;
    }

    Logger::warning("Attempting to reconnect stream: " + config_.id + " (attempt "
                    + std::to_string(reconnectAttempts_ + 1) + "/"
                    + std::to_string(maxReconnectAttempts_) + ")");

    isReconnecting_ = true;
    reconnectAttempts_++;

    // 计算重连延迟（使用指数退避策略）
    // 1秒，2秒，4秒，8秒...
    int64_t reconnectDelay = 1000000 * (1 << (reconnectAttempts_ - 1));
    if (reconnectDelay > 30000000) reconnectDelay = 30000000; // 最大30秒

    Logger::info("Waiting " + std::to_string(reconnectDelay / 1000000) + " seconds before reconnecting");
    av_usleep(reconnectDelay);

    try {
        // 清理旧资源
        cleanup();

        // 重新初始化连接
        initialize();

        Logger::info("Successfully reconnected stream: " + config_.id);
        resetErrorState();
        isReconnecting_ = false;
        return true;
    } catch (const FFmpegException& e) {
        Logger::error("Failed to reconnect stream " + config_.id + ": " + std::string(e.what()));
        hasError_ = true;
        isReconnecting_ = false;
        return false;
    }
}
