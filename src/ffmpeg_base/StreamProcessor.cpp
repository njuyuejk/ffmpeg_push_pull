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
          outputFormatContext_(nullptr),
          videoFrameCallback_(nullptr), audioFrameCallback_(nullptr) {

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
        LOGGER_ERROR("Exception in StreamProcessor destructor: " + std::string(e.what()));
    } catch (...) {
        LOGGER_ERROR("Unknown exception in StreamProcessor destructor");
    }
}

void StreamProcessor::start() {
    if (isRunning_) {
        LOGGER_WARNING("Stream already running: " + config_.id);
        return;
    }

    try {
        initialize();
        isRunning_ = true;
        hasError_ = false;
        reconnectAttempts_ = 0;  // 重置重连计数器
        lastFrameTime_ = av_gettime(); // 初始化最后一帧时间

        processingThread_ = std::thread(&StreamProcessor::processLoop, this);
        LOGGER_INFO("Started stream processing: " + config_.id + " (" +
                     config_.inputUrl + " -> " + config_.outputUrl + ")");
    } catch (const FFmpegException& e) {
        // 改变这里的处理方式，标记为错误但不重新抛出异常
        LOGGER_ERROR("Failed to start stream " + config_.id + ": " + std::string(e.what()));
        cleanup();
        hasError_ = true;
        isRunning_ = false;
        // 不要在这里重新抛出异常，让调用者认为流已启动，
        // 然后由监控系统检测到错误并尝试重连
    }
}

void StreamProcessor::stop() {
    LOGGER_INFO("Request to stop stream: " + config_.id);

    // 首先将运行标志设置为false，通知处理线程退出
    bool wasRunning = isRunning_.exchange(false);

    // 如果之前不是运行状态，直接返回
    if (!wasRunning) {
        LOGGER_INFO("Stream " + config_.id + " already stopped, no action needed");
        return;
    }

    // 检查处理线程是否存在和可加入
    if (!processingThread_.joinable()) {
        LOGGER_INFO("Processing thread for stream " + config_.id + " is not joinable");
        return;
    }

    try {
        // 设置线程分离标志
        bool shouldDetach = false;

        // 尝试温和地等待线程结束
        {
            LOGGER_DEBUG("Waiting for processing thread to end for stream: " + config_.id);

            // 使用单独的线程和超时来等待处理线程
            std::atomic<bool> threadJoined{false};

            // 创建用于等待的线程
            std::thread joinThread([this, &threadJoined]() {
                try {
                    if (this->processingThread_.joinable()) {
                        this->processingThread_.join();
                        threadJoined = true;
                    }
                } catch (const std::exception& e) {
                    LOGGER_ERROR("Exception joining processing thread: " + std::string(e.what()));
                } catch (...) {
                    LOGGER_ERROR("Unknown exception joining processing thread");
                }
            });

            // 等待最多5秒
            for (int i = 0; i < 10 && !threadJoined; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (!threadJoined) {
                LOGGER_WARNING("Timeout joining processing thread for stream: " + config_.id);
                shouldDetach = true;
            }

            // 确保等待线程完成或分离
            if (joinThread.joinable()) {
                if (threadJoined) {
                    joinThread.join();
                } else {
                    joinThread.detach();
                }
            }
        }

        // 如果需要，分离处理线程
        if (shouldDetach && processingThread_.joinable()) {
            try {
                processingThread_.detach();
                LOGGER_INFO("Detached processing thread for stream " + config_.id);
            } catch (const std::exception& e) {
                LOGGER_ERROR("Exception detaching processing thread: " + std::string(e.what()));
            }
        }

        // 确保进行完整的资源清理
        cleanup();

        LOGGER_INFO("Stream " + config_.id + " successfully stopped");
    } catch (const std::exception& e) {
        LOGGER_ERROR("Exception stopping stream " + config_.id + ": " + std::string(e.what()));

        // 最后的安全措施 - 尝试分离线程而不是让它挂起
        if (processingThread_.joinable()) {
            try {
                processingThread_.detach();
            } catch (...) {
                LOGGER_ERROR("Final attempt to detach processing thread failed");
            }
        }
    } catch (...) {
        LOGGER_ERROR("Unknown exception stopping stream " + config_.id);

        // 类似的最终安全措施
        if (processingThread_.joinable()) {
            try {
                processingThread_.detach();
            } catch (...) {
                // 不再记录，因为日志系统也可能有问题
            }
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
        LOGGER_WARNING("Cannot update config while stream is running: " + config_.id);
        return false;
    }

    if (!config.validate()) {
        LOGGER_ERROR("Invalid stream configuration for: " + config.id);
        return false;
    }

    config_ = config;
    return true;
}

void StreamProcessor::initialize() {
    // 打开输入
    openInput();

    if (config_.pushEnabled) {
        // 打开输出
        openOutput();

        // 为每个流设置编解码器
        setupStreams();

        // 写输出头
        int ret = avformat_write_header(outputFormatContext_, nullptr);
        if (ret < 0) {
            throw FFmpegException("Failed to write output header", ret);
        }
    } else {
        setupInputStreams();
        LOGGER_INFO("Stream " + config_.id + " initialized in pull-only mode (no pushing)");
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
//            av_dict_set(&options, "rtsp_transport", "udp", 0);  // 使用TCP传输RTSP (比UDP更可靠)
//            av_dict_set_int(&options, "reorder_queue_size", 1024, 0);
            av_dict_set(&options, "stimeout", "5000000", 0);    // Socket超时 5秒
            av_dict_set(&options, "max_delay", "500000", 0);    // 最大延迟500毫秒
        }
    }

    // 添加额外选项
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("input_") == 0) {
            std::string optionName = key.substr(6); // 去除"input_"前缀
            if (optionName == "rtsp_transport") {
                if (value == "udp") {
                    av_dict_set_int(&options, "reorder_queue_size", 1024, 0);
                }
            }
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
            LOGGER_WARNING("Unused input options: " + std::string(unused));
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
    LOGGER_DEBUG("Input stream information:");
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

        LOGGER_DEBUG("  Stream #" + std::to_string(i) + ": " + streamType + " - " + codecName);

        // 如果是视频流，打印更多信息
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            LOGGER_DEBUG("    Resolution: " + std::to_string(stream->codecpar->width) + "x" +
                          std::to_string(stream->codecpar->height));

            if (stream->r_frame_rate.num && stream->r_frame_rate.den) {
                double fps = (double)stream->r_frame_rate.num / stream->r_frame_rate.den;
                LOGGER_DEBUG("    Frame rate: " + std::to_string(fps) + " fps");
            } else {
                LOGGER_WARNING("    Frame rate could not be determined");
            }

            // 获取视频编码器详细信息
            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                LOGGER_DEBUG("    Codec: " + std::string(codec->name) + " (" + codec->long_name + ")");
            }
        }

        // 如果是音频流，打印更多信息
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            LOGGER_DEBUG("    Sample rate: " + std::to_string(stream->codecpar->sample_rate) + " Hz");
            LOGGER_DEBUG("    Channels: " + std::to_string(stream->codecpar->channels));

            // 获取音频编码器详细信息
            const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
            if (codec) {
                LOGGER_DEBUG("    Codec: " + std::string(codec->name) + " (" + codec->long_name + ")");
            }
        }
    }

    LOGGER_INFO("Successfully opened input: " + config_.inputUrl);
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

    LOGGER_INFO("Successfully prepared output: " + config_.outputUrl);
}

AVBufferRef* StreamProcessor::createHardwareDevice() {
    if (!config_.enableHardwareAccel || config_.hwaccelType.empty() ||
        config_.hwaccelType == "none") {
        return nullptr;
    }

    LOGGER_DEBUG("Attempting to create hardware device of type: " + config_.hwaccelType);

    // 如果是"auto"，尝试不同的硬件加速类型
    if (config_.hwaccelType == "auto") {
        static const AVHWDeviceType hwTypes[] = {
                AV_HWDEVICE_TYPE_CUDA,       // NVIDIA GPU
                AV_HWDEVICE_TYPE_QSV,        // Intel Quick Sync
                AV_HWDEVICE_TYPE_VAAPI,      // VA-API (Intel/AMD)
                AV_HWDEVICE_TYPE_VDPAU,      // VDPAU (NVIDIA)
                AV_HWDEVICE_TYPE_D3D11VA,    // DirectX 11 (Windows)
                AV_HWDEVICE_TYPE_DXVA2,      // DirectX (Windows)
                AV_HWDEVICE_TYPE_VIDEOTOOLBOX, // Apple
#if 1
                AV_HWDEVICE_TYPE_RKMPP       // RK3588
#endif
        };

        for (AVHWDeviceType hwType : hwTypes) {
            AVBufferRef* hwDeviceContext = nullptr;

            // 跳过未知类型
            if (hwType == AV_HWDEVICE_TYPE_NONE)
                continue;

            const char* typeName = av_hwdevice_get_type_name(hwType);
            LOGGER_DEBUG("Trying hardware type: " + std::string(typeName ? typeName : "Unknown"));

            int ret = av_hwdevice_ctx_create(&hwDeviceContext, hwType, nullptr, nullptr, 0);
            if (ret >= 0) {
                const char* hwName = av_hwdevice_get_type_name(hwType);
                LOGGER_INFO("Auto-detected hardware acceleration: " + std::string(hwName));
                return hwDeviceContext;
            } else {
                char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, errBuf, AV_ERROR_MAX_STRING_SIZE);
                LOGGER_DEBUG("Failed to initialize " + std::string(typeName ? typeName : "Unknown") +
                              " hardware: " + errBuf);
            }
        }

        LOGGER_WARNING("No hardware acceleration device found, falling back to software");
        return nullptr;
    }

    // 使用指定的硬件加速类型
    AVBufferRef* hwDeviceContext = nullptr;
    AVHWDeviceType hwType = av_hwdevice_find_type_by_name(config_.hwaccelType.c_str());

    if (hwType == AV_HWDEVICE_TYPE_NONE) {
        LOGGER_WARNING("Hardware acceleration type not found: " + config_.hwaccelType);
        return nullptr;
    }

    int ret = av_hwdevice_ctx_create(&hwDeviceContext, hwType, nullptr, nullptr, 0);
    if (ret < 0) {
        char errBuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, errBuf, AV_ERROR_MAX_STRING_SIZE);
        LOGGER_WARNING("Failed to create hardware device context (" + config_.hwaccelType +
                        "): " + errBuf);
        return nullptr;
    }

    LOGGER_INFO("Created hardware acceleration context: " + config_.hwaccelType);
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
#if 1
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
#if 1
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
            LOGGER_DEBUG("Found encoder: " + candidate);
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
                    LOGGER_DEBUG("Encoder " + candidate + " does not support hardware type " +
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
        LOGGER_WARNING("No suitable hardware encoder found, falling back to default");
        selectedEncoder = avcodec_find_encoder(codecId);
    }

    if (selectedEncoder) {
        LOGGER_INFO("Selected encoder: " + std::string(selectedEncoder->name) +
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
            LOGGER_DEBUG("Ignoring stream of type: " +
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
//    const AVCodec* codec = avcodec_find_decoder(inputStream->codecpar->codec_id);

    std::string base_name;
    if (inputStream->codecpar->codec_id == AV_CODEC_ID_H264) {
        base_name = "h264_rkmpp";
    } else if (inputStream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        base_name = "hevc_rkmpp";
    } else {
        // 对其他编码直接软解
        base_name =  std::string(avcodec_get_name(inputStream->codecpar->codec_id));
    }
    const AVCodec* decoder = avcodec_find_decoder_by_name(base_name.c_str());
    if (!decoder) {
        LOGGER_WARNING("Unsupported video codec");
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
                LOGGER_WARNING("Decoder context not initialized, cannot attach hardware device");
            }
        }
    }

    // 打开解码器
    AVDictionary* decoderOptions = nullptr;

    if (config_.hwaccelType == "rkmpp") {
        av_dict_set(&decoderOptions, "output_format", "nv12", 0);
        av_dict_set(&decoderOptions, "zero_copy_mode", "0", 0);  // Disable zero copy to avoid DRM_PRIME format

        // Additional RKMPP specific options
        av_dict_set(&decoderOptions, "use_direct_mode", "0", 0);
        LOGGER_INFO("Set up RKMPP decoder to use NV12 output format");
    }

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
            LOGGER_WARNING("Hardware-accelerated decoding failed, falling back to software decoding");
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

    LOGGER_INFO("Using video encoder: " + std::string(encoder->name));

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

    // 检查配置中是否明确指定了像素格式
    std::string pixFmtStr = "nv12";  // 默认使用NV12
    for (const auto& [key, value] : config_.extraOptions) {
        if (key == "hwaccel_pix_fmt") {
            pixFmtStr = value;
            std::transform(pixFmtStr.begin(), pixFmtStr.end(), pixFmtStr.begin(), ::tolower);
            LOGGER_DEBUG("User specified hardware pixel format: " + pixFmtStr);
        }
    }

    AVPixelFormat preferredFormat = AV_PIX_FMT_NV12;  // 默认
    if (pixFmtStr == "yuv420p") preferredFormat = AV_PIX_FMT_YUV420P;
    else if (pixFmtStr == "nv12") preferredFormat = AV_PIX_FMT_NV12;
    else if (pixFmtStr == "p010le") preferredFormat = AV_PIX_FMT_P010LE;
    else if (pixFmtStr == "drm_prime") preferredFormat = AV_PIX_FMT_DRM_PRIME;

    if (streamCtx.hwDeviceContext) {

        bool isRKMPPEncoder = (std::string(encoder->name).find("rkmpp") != std::string::npos);

        if (isRKMPPEncoder) {
            // RKMPP编码器必须使用DRM_PRIME格式
            streamCtx.encoderContext->pix_fmt = AV_PIX_FMT_DRM_PRIME;
            LOGGER_INFO("Using RKMPP encoder with DRM_PRIME format");
        } else {
            // 对于其他硬件编码器，使用首选格式
            streamCtx.encoderContext->pix_fmt = preferredFormat;

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
                    LOGGER_WARNING("Preferred format not supported by NVENC encoder");
                    streamCtx.encoderContext->pix_fmt = encoder->pix_fmts[0];
                }
            }
        }

//        // 对于硬件编码，设置为首选格式
//        streamCtx.encoderContext->pix_fmt = preferredFormat;
//
//        // 对于NVIDIA NVENC，检查是否支持首选格式
//        if (std::string(encoder->name).find("nvenc") != std::string::npos) {
//            bool formatSupported = false;
//            int i = 0;
//            while (encoder->pix_fmts && encoder->pix_fmts[i] != AV_PIX_FMT_NONE) {
//                if (encoder->pix_fmts[i] == preferredFormat) {
//                    formatSupported = true;
//                    break;
//                }
//                i++;
//            }
//
//            if (!formatSupported && encoder->pix_fmts) {
//                LOGGER_WARNING(" not supported by encoder");
//                streamCtx.encoderContext->pix_fmt = encoder->pix_fmts[0];
//            }
//        }
//
        LOGGER_INFO("Using hardware encoding with pixel format");
    } else {
        // 对于软件编码，使用编码器支持的第一个格式
        if (encoder->pix_fmts) {
            streamCtx.encoderContext->pix_fmt = encoder->pix_fmts[0];
            LOGGER_INFO("Using software encoding with pixel format");
        } else {
            streamCtx.encoderContext->pix_fmt = AV_PIX_FMT_YUV420P; // 默认
            LOGGER_INFO("Using default pixel format: YUV420P");
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

            // 对于RKMPP，软件格式应该是NV12
            if (streamCtx.encoderContext->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
                framesCtx->sw_format = AV_PIX_FMT_NV12;  // DRM_PRIME的底层软件格式
                LOGGER_INFO("Set DRM_PRIME hardware frames context with NV12 software format");
            } else {
                framesCtx->sw_format = streamCtx.encoderContext->pix_fmt;
            }

//            framesCtx->sw_format = streamCtx.encoderContext->pix_fmt;  // 使用相同格式
            framesCtx->width = streamCtx.encoderContext->width;
            framesCtx->height = streamCtx.encoderContext->height;
            framesCtx->initial_pool_size = 20;  // 预分配足够的帧池

            ret = av_hwframe_ctx_init(hwFramesContext);
            if (ret < 0) {
                av_buffer_unref(&hwFramesContext);
                LOGGER_WARNING("Failed to initialize hardware frames context, falling back to software");
            } else {
                streamCtx.encoderContext->hw_frames_ctx = hwFramesContext;
                LOGGER_INFO("Hardware frame context initialized with format");
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
    LOGGER_INFO("Set up video stream: input#" + std::to_string(inputStream->index) +
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
        LOGGER_WARNING("Unsupported audio codec");
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

    LOGGER_INFO("Using audio encoder: " + std::string(encoder->name));

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
    LOGGER_INFO("Set up audio stream: input#" + std::to_string(inputStream->index) +
                 " -> output#" + std::to_string(outputStream->index) +
                 " (" + std::string(encoder->name) + ")");
}

void StreamProcessor::setupInputStreams() {
    // 为输入中的每个流
    for (unsigned int i = 0; i < inputFormatContext_->nb_streams; i++) {
        AVStream* inputStream = inputFormatContext_->streams[i];

        if (inputStream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            setupInputVideoStream(inputStream);
        } else if (inputStream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            setupInputAudioStream(inputStream);
        } else {
            // 忽略其他类型的流（字幕等）
            LOGGER_DEBUG("Ignoring stream of type: " +
                          std::to_string(inputStream->codecpar->codec_type));
        }
    }

    if (videoStreams_.empty() && audioStreams_.empty()) {
        throw FFmpegException("No supported streams found in input");
    }
}

void StreamProcessor::setupInputVideoStream(AVStream* inputStream) {
    int ret;
    StreamContext streamCtx;
    streamCtx.streamIndex = inputStream->index;
    streamCtx.inputStream = inputStream;
    streamCtx.inputStream = inputStream;

    // 查找解码器
//    const AVCodec* codec = avcodec_find_decoder(inputStream->codecpar->codec_id);

    std::string base_name;
    if (inputStream->codecpar->codec_id == AV_CODEC_ID_H264) {
        base_name = "h264_rkmpp";
    } else if (inputStream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        base_name = "hevc_rkmpp";
    } else {
        // 对其他编码直接软解
        base_name =  std::string(avcodec_get_name(inputStream->codecpar->codec_id));
    }
    const AVCodec* decoder = avcodec_find_decoder_by_name(base_name.c_str());
    if (!decoder) {
        LOGGER_WARNING("Unsupported video codec");
        return;
    }

    // 分配解码器上下文
    streamCtx.decoderContext = avcodec_alloc_context3(decoder);
    if (!streamCtx.decoderContext) {
        throw FFmpegException("Failed to allocate decoder context");
    }

    // 从输入流复制参数
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
            // 确保解码器上下文已分配
            if (streamCtx.decoderContext) {
                streamCtx.decoderContext->hw_device_ctx = av_buffer_ref(streamCtx.hwDeviceContext);
                hwType = ((AVHWDeviceContext*)streamCtx.hwDeviceContext->data)->type;
            } else {
                LOGGER_WARNING("Decoder context not initialized, cannot attach hardware device");
            }
        }
    }

    // 使用选项打开解码器
    AVDictionary* decoderOptions = nullptr;

    if (config_.hwaccelType == "rkmpp") {
        av_dict_set(&decoderOptions, "output_format", "nv12", 0);
        av_dict_set(&decoderOptions, "zero_copy_mode", "0", 0);  // Disable zero copy to avoid DRM_PRIME format

        // Additional RKMPP specific options
        av_dict_set(&decoderOptions, "use_direct_mode", "0", 0);
        LOGGER_INFO("Set up RKMPP decoder to use NV12 output format");
    }

    // 设置解码器选项
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("decoder_") == 0) {
            std::string optionName = key.substr(8); // 移除 "decoder_" 前缀
            av_dict_set(&decoderOptions, optionName.c_str(), value.c_str(), 0);
        }
    }

    ret = avcodec_open2(streamCtx.decoderContext, decoder, &decoderOptions);
    av_dict_free(&decoderOptions);
    if (ret < 0) {
        if (streamCtx.decoderContext->hw_device_ctx) {
            // 如果硬件解码失败，尝试回退到软件解码
            LOGGER_WARNING("Hardware-accelerated decoding failed, falling back to software decoding");
            av_buffer_unref(&streamCtx.decoderContext->hw_device_ctx);
            streamCtx.decoderContext->hw_device_ctx = nullptr;

            // 不使用硬件加速重新打开解码器
            ret = avcodec_open2(streamCtx.decoderContext, decoder, nullptr);
            if (ret < 0) {
                throw FFmpegException("Failed to open video decoder even with software fallback", ret);
            }
        } else {
            throw FFmpegException("Failed to open video decoder", ret);
        }
    }

    LOGGER_INFO("stream fps is: " + std::to_string(av_q2d(inputStream->avg_frame_rate)));

    // 由于我们不推流，将输出流设为 nullptr
    streamCtx.outputStream = nullptr;
    streamCtx.encoderContext = nullptr;

    // 分配包和帧
    streamCtx.packet = av_packet_alloc();
    streamCtx.frame = av_frame_alloc();
    streamCtx.hwFrame = av_frame_alloc();

    if (!streamCtx.packet || !streamCtx.frame || !streamCtx.hwFrame) {
        throw FFmpegException("Failed to allocate packet or frame");
    }

    // 添加到视频流列表
    videoStreams_.push_back(streamCtx);
    LOGGER_INFO("Set up input-only video stream: input#" + std::to_string(inputStream->index));
}

void StreamProcessor::setupInputAudioStream(AVStream* inputStream) {
    int ret;
    StreamContext streamCtx;
    streamCtx.streamIndex = inputStream->index;
    streamCtx.inputStream = inputStream;

    // 查找解码器
    const AVCodec* decoder = avcodec_find_decoder(inputStream->codecpar->codec_id);
    if (!decoder) {
        LOGGER_WARNING("Unsupported audio codec");
        return;
    }

    // 分配解码器上下文
    streamCtx.decoderContext = avcodec_alloc_context3(decoder);
    if (!streamCtx.decoderContext) {
        throw FFmpegException("Failed to allocate audio decoder context");
    }

    // 从输入流复制参数
    ret = avcodec_parameters_to_context(streamCtx.decoderContext, inputStream->codecpar);
    if (ret < 0) {
        throw FFmpegException("Failed to copy audio decoder parameters", ret);
    }

    // 设置解码器选项
    AVDictionary* decoderOptions = nullptr;
    for (const auto& [key, value] : config_.extraOptions) {
        if (key.find("audio_decoder_") == 0) {
            std::string optionName = key.substr(14); // 移除 "audio_decoder_" 前缀
            av_dict_set(&decoderOptions, optionName.c_str(), value.c_str(), 0);
        }
    }

    // 打开解码器
    ret = avcodec_open2(streamCtx.decoderContext, decoder, &decoderOptions);
    av_dict_free(&decoderOptions);
    if (ret < 0) {
        throw FFmpegException("Failed to open audio decoder", ret);
    }

    // 由于我们不推流，将输出流设为 nullptr
    streamCtx.outputStream = nullptr;
    streamCtx.encoderContext = nullptr;

    // 分配包和帧
    streamCtx.packet = av_packet_alloc();
    streamCtx.frame = av_frame_alloc();

    if (!streamCtx.packet || !streamCtx.frame) {
        throw FFmpegException("Failed to allocate packet or frame");
    }

    // 添加到音频流列表
    audioStreams_.push_back(streamCtx);
    LOGGER_INFO("Set up input-only audio stream: input#" + std::to_string(inputStream->index));
}

// 添加到processLoop方法中，增强调试信息输出
void StreamProcessor::processLoop() {
    AVPacket* packet = nullptr;
    try {
        packet = av_packet_alloc();
        if (!packet) {
            LOGGER_ERROR("Failed to allocate packet memory");
            hasError_ = true;
            return;
        }

        int64_t lastLogTime = 0;
        int frameCount = 0;
        int streamErrorCount = 0;
        const int MAX_ERRORS_BEFORE_RECONNECT = 20;
        bool reconnectInProgress = false;

        // 记录开始时间
        int64_t startTime = av_gettime() / 1000000;
        lastFrameTime_ = av_gettime();
        LOGGER_INFO("Starting process loop for stream: " + config_.id);

        // 处理循环
        while (isRunning_) {
            try {
                // 检查退出信号
                if (!isRunning_) {
                    LOGGER_INFO("Exit signal detected for stream: " + config_.id);
                    break;
                }

                // 检查流是否停滞
                if (isStreamStalled() && !reconnectInProgress) {
                    LOGGER_WARNING("Stream " + config_.id + " appears stalled (no data for "
                                    + std::to_string(noDataTimeout_ / 1000000) + " seconds)");

                    reconnectInProgress = true;
                    if (!reconnect()) {
                        LOGGER_ERROR("Failed to reconnect stream " + config_.id + ", entering wait mode");
                        hasError_ = true;
                        av_usleep(5000000); // 5秒
                        reconnectInProgress = false;
                        continue;
                    }
                    reconnectInProgress = false;
                    streamErrorCount = 0;
                    continue;
                }

                // 检查输入上下文是否有效
                if (!inputFormatContext_) {
                    LOGGER_ERROR("Input context invalid for stream: " + config_.id);
                    hasError_ = true;
                    av_usleep(1000000); // 1秒
                    continue;
                }

                // 读取输入包
                int ret = av_read_frame(inputFormatContext_, packet);
                // 读取输入包
                if (ret < 0) {
                    // 处理各种错误情况
                    // ... (与之前相同的代码)
                    if (ret == AVERROR_EOF) {

                        if (config_.isLocalFile) {
                            // 对于本地MP4文件，循环回到开始位置
                            LOGGER_INFO("End of local file reached, seeking back to beginning: " + config_.id);

                            // 定位到文件开始位置
                            ret = av_seek_frame(inputFormatContext_, -1, 0, AVSEEK_FLAG_BACKWARD);
                            if (ret < 0) {
                                LOGGER_ERROR("Failed to seek to the beginning of the file: " + config_.id);
                                break;
                            }

                            // 刷新所有解码器以清除其内部状态
                            for (auto& streamCtx : videoStreams_) {
                                if (streamCtx.decoderContext) {
                                    avcodec_flush_buffers(streamCtx.decoderContext);
                                }
                            }

                            for (auto& streamCtx : audioStreams_) {
                                if (streamCtx.decoderContext) {
                                    avcodec_flush_buffers(streamCtx.decoderContext);
                                }
                            }

                            // 更新时间戳以防止停滞检测
                            lastFrameTime_ = av_gettime();
                            continue;
                        } else {
                            LOGGER_INFO("End of input stream: " + config_.id);
                            break;
                        }
                    } else if (ret == AVERROR(EAGAIN)) {
                        av_usleep(10000);  // 休眠10毫秒
                        continue;
                    } else {
                        streamErrorCount++;
                        if (streamErrorCount > MAX_ERRORS_BEFORE_RECONNECT && !reconnectInProgress) {
                            LOGGER_WARNING("Too many consecutive errors (" + std::to_string(streamErrorCount)
                                            + "), attempting reconnection");

                            reconnectInProgress = true;
                            if (!reconnect()) {
                                LOGGER_ERROR("Failed to reconnect stream " + config_.id + ", entering wait mode");
                                hasError_ = true;
                                av_usleep(5000000); // 5秒
                                reconnectInProgress = false;
                                continue;
                            }
                            reconnectInProgress = false;
                            streamErrorCount = 0;
                            continue;
                        }

                        LOGGER_WARNING("Error reading frame (attempt " + std::to_string(streamErrorCount) +
                                        "/" + std::to_string(MAX_ERRORS_BEFORE_RECONNECT) + "): " +
                                        std::to_string(ret));
                        av_usleep(100000);  // 错误后休眠100毫秒
                        continue;
                    }
                }

                lastFrameTime_ = av_gettime();
                streamErrorCount = 0;

                // 处理视频流包
                bool packetProcessed = false;
                for (auto& streamCtx : videoStreams_) {
                    if (packet->stream_index == streamCtx.streamIndex) {
                        // Wrap packet processing in try-catch to handle output write errors
                        try {
                            processVideoPacket(packet, streamCtx);
                            packetProcessed = true;
                            frameCount++;
                        } catch (const FFmpegException& e) {
                            LOGGER_ERROR("Error processing video packet: " + std::string(e.what()));
                            // If error is related to writing, attempt reconnection
                            if (std::string(e.what()).find("Error writing encoded packet") != std::string::npos) {
                                if (!reconnectInProgress) {
                                    LOGGER_WARNING("Output error detected, attempting reconnection");
                                    reconnectInProgress = true;
                                    if (!reconnect()) {
                                        // Just log and continue - don't exit the loop
                                        LOGGER_ERROR("Failed to reconnect stream " + config_.id + " after output error");
                                        hasError_ = true;
                                        av_usleep(5000000); // 5 seconds
                                    }
                                    reconnectInProgress = false;
                                }
                            }
                        }
                        break;
                    }
                }

                // Process audio packet with similar error handling
                if (!packetProcessed) {
                    for (auto& streamCtx : audioStreams_) {
                        if (packet->stream_index == streamCtx.streamIndex) {
                            try {
                                processAudioPacket(packet, streamCtx);
                                packetProcessed = true;
                            } catch (const FFmpegException& e) {
                                LOGGER_ERROR("Error processing audio packet: " + std::string(e.what()));
                                // Handle output errors similar to video
                                if (std::string(e.what()).find("Error writing encoded packet") != std::string::npos) {
                                    if (!reconnectInProgress) {
                                        LOGGER_WARNING("Output error detected, attempting reconnection");
                                        reconnectInProgress = true;
                                        if (!reconnect()) {
                                            LOGGER_ERROR("Failed to reconnect stream " + config_.id + " after output error");
                                            hasError_ = true;
                                            av_usleep(5000000); // 5 seconds
                                        }
                                        reconnectInProgress = false;
                                    }
                                }
                            }
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

                    LOGGER_DEBUG("Stream " + config_.id + " processed " +
                                  std::to_string(frameCount) + " frames, avg. " +
                                  std::to_string(avgFps) + " fps (running for " +
                                  std::to_string(runTime) + " seconds)");

                    lastLogTime = currentTime;
                }

                // 释放包
                av_packet_unref(packet);

            } catch (const FFmpegException& e) {
                // 安全地释放包
                if (packet) {
                    av_packet_unref(packet);
                }

                LOGGER_ERROR("Stream processing error: " + std::string(e.what()));

                if (!reconnectInProgress && isRunning_) {
                    LOGGER_WARNING("Stream error detected, attempting reconnection");
                    reconnectInProgress = true;
                    if (!reconnect()) {
                        LOGGER_ERROR("Failed to reconnect stream " + config_.id + ", entering wait mode");
                        hasError_ = true;
                        av_usleep(5000000); // 5秒
                        reconnectInProgress = false;
                        continue;
                    }
                    reconnectInProgress = false;
                    streamErrorCount = 0;
                }
            } catch (const std::exception& e) {
                // 安全地释放包
                if (packet) {
                    av_packet_unref(packet);
                }

                LOGGER_ERROR("Standard exception in stream " + config_.id + ": " + std::string(e.what()));
                av_usleep(1000000); // 1秒
            } catch (...) {
                // 安全地释放包
                if (packet) {
                    av_packet_unref(packet);
                }

                LOGGER_ERROR("Unknown exception in stream " + config_.id);
                av_usleep(1000000); // 1秒
            }
        }

        // 计算总处理时间和平均帧率
        int64_t endTime = av_gettime() / 1000000;
        int64_t totalTime = endTime - startTime;
        double avgFps = totalTime > 0 ? (double)frameCount / totalTime : 0;

        LOGGER_INFO("Exiting process loop for stream: " + config_.id + " after " +
                     std::to_string(totalTime) + " seconds, processed " +
                     std::to_string(frameCount) + " frames (avg. " +
                     std::to_string(avgFps) + " fps)");

    } catch (const std::exception& e) {
        LOGGER_ERROR("Critical exception in stream processing loop: " + std::string(e.what()));
        hasError_ = true;
    } catch (...) {
        LOGGER_ERROR("Unknown critical exception in stream processing loop");
        hasError_ = true;
    }

    // 最后确保释放包资源
    if (packet) {
        av_packet_free(&packet);
    }

    // 确保在退出前设置状态
    isRunning_ = false;
}

void StreamProcessor::processVideoPacket(AVPacket* packet, StreamContext& streamCtx) {
    int ret;
    bool frameAcquired = false;
    bool hwFrameUsed = false;
    bool callbackFrameUsed = false;
    // 准备用于回调的帧（转换为NV12格式供AI处理）
    AVFrame* callbackFrame = nullptr;

    try {
        // 将数据包发送到解码器
        ret = avcodec_send_packet(streamCtx.decoderContext, packet);
        if (ret < 0) {
            throw FFmpegException("Error sending packet to decoder", ret);
        }

        while (ret >= 0) {
            // 在每个循环开始时重置标志
            frameAcquired = false;
            hwFrameUsed = false;
            callbackFrameUsed = false;

            // 重置callbackFrame指针
            if (callbackFrame && callbackFrameUsed) {
                av_frame_free(&callbackFrame);
                callbackFrame = nullptr;
                callbackFrameUsed = false;
            }

            // 接收解码后的帧
            ret = avcodec_receive_frame(streamCtx.decoderContext, streamCtx.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                LOGGER_DEBUG("Decoder frame receive failed, errorCode: " + std::to_string(ret));
                break;
            } else if (ret < 0) {
                throw FFmpegException("Error receiving frame from decoder", ret);
            }

            frameAcquired = true;

            // 更新最后一帧时间以防止重连
            lastFrameTime_ = av_gettime();

            // 计算时间戳
            int64_t pts = streamCtx.frame->pts;
            if (streamCtx.inputStream) {
                pts = av_rescale_q(pts, streamCtx.inputStream->time_base, {1, 1000});
            }

            if (streamCtx.frame->format == AV_PIX_FMT_DRM_PRIME) {
                LOGGER_DEBUG("Processing DRM_PRIME frame for callback");

                // 为回调分配NV12帧
                callbackFrame = av_frame_alloc();
                if (!callbackFrame) {
                    throw FFmpegException("Failed to allocate callback frame");
                }

                callbackFrame->width = streamCtx.frame->width;
                callbackFrame->height = streamCtx.frame->height;
                callbackFrame->format = AV_PIX_FMT_NV12;

                ret = av_frame_get_buffer(callbackFrame, 32);
                if (ret < 0) {
                    av_frame_free(&callbackFrame);
                    throw FFmpegException("Failed to allocate buffer for callback frame", ret);
                }

                // 从DRM_PRIME转换到NV12
                ret = av_hwframe_transfer_data(callbackFrame, streamCtx.frame, 0);
                if (ret < 0) {
                    LOGGER_WARNING("Failed to transfer DRM_PRIME data to NV12 for callback");
                    av_frame_free(&callbackFrame);
                    callbackFrame = nullptr;
                } else {
                    callbackFrame->pts = streamCtx.frame->pts;
                    callbackFrameUsed = true;
                    LOGGER_DEBUG("Successfully converted DRM_PRIME to NV12 for callback");
                }
            } else if (streamCtx.frame->format == AV_PIX_FMT_NV12) {
                // 直接使用NV12帧
                callbackFrame = streamCtx.frame;
            } else {
                // 对于其他格式，可能需要转换
                LOGGER_DEBUG("Frame format: " + std::to_string(streamCtx.frame->format));
                callbackFrame = streamCtx.frame;
            }

            // 调用帧处理回调函数（如果设置了）
            if (callbackFrame) {
                int fps = streamCtx.inputStream ? (int)av_q2d(streamCtx.inputStream->avg_frame_rate) : 25;
                handleVideoFrame(callbackFrame, pts, fps);
            }

            // 如果是仅拉流模式，我们不需要编码或写入帧
            if (!config_.pushEnabled) {
                // 释放帧
                if (frameAcquired) {
                    av_frame_unref(streamCtx.frame);
                }
                if (callbackFrameUsed && callbackFrame) {
                    av_frame_free(&callbackFrame);
                }
                continue;
            }

            // 推流处理 - 使用原始帧进行编码
            AVFrame* frameToEncode = streamCtx.frame;

            // 对于DRM_PRIME格式的帧，可以直接用于硬件编码
            if (streamCtx.frame->format == AV_PIX_FMT_DRM_PRIME &&
                streamCtx.encoderContext->hw_frames_ctx) {
                // 直接使用DRM_PRIME帧进行硬件编码
                LOGGER_DEBUG("Using DRM_PRIME frame directly for hardware encoding");
                frameToEncode = streamCtx.frame;
            } else if (streamCtx.frame->format == AV_PIX_FMT_DRM_PRIME &&
                       !streamCtx.encoderContext->hw_frames_ctx) {
                // 需要转换DRM_PRIME到软件格式
                if (!streamCtx.hwFrame->data[0]) {
                    streamCtx.hwFrame->width = streamCtx.frame->width;
                    streamCtx.hwFrame->height = streamCtx.frame->height;
                    streamCtx.hwFrame->format = streamCtx.encoderContext->pix_fmt;
                    ret = av_frame_get_buffer(streamCtx.hwFrame, 32);
                    if (ret < 0) {
                        throw FFmpegException("Failed to allocate buffer for encoding frame", ret);
                    }
                }

                ret = av_hwframe_transfer_data(streamCtx.hwFrame, streamCtx.frame, 0);
                if (ret < 0) {
                    throw FFmpegException("Error transferring DRM_PRIME data for encoding", ret);
                }
                frameToEncode = streamCtx.hwFrame;
                hwFrameUsed = true;
            }

            // 检查是否需要缩放或格式转换
            bool needsConversion = false;

            // 如果分辨率不同，需要缩放
            if (streamCtx.encoderContext->width != frameToEncode->width ||
                streamCtx.encoderContext->height != frameToEncode->height) {
                needsConversion = true;
            }

            // 如果格式不同且不是硬件到硬件的转换
            if (streamCtx.encoderContext->pix_fmt != frameToEncode->format) {
                // 对于DRM_PRIME到DRM_PRIME，通常不需要转换
                if (!(frameToEncode->format == AV_PIX_FMT_DRM_PRIME &&
                      streamCtx.encoderContext->pix_fmt == AV_PIX_FMT_DRM_PRIME)) {
                    needsConversion = true;
                }
            }

            // 如果使用硬件帧上下文，可以跳过某些转换
            if (streamCtx.encoderContext->hw_frames_ctx &&
                frameToEncode->format == AV_PIX_FMT_DRM_PRIME &&
                streamCtx.encoderContext->pix_fmt == AV_PIX_FMT_DRM_PRIME) {
                LOGGER_DEBUG("Using direct DRM_PRIME to DRM_PRIME encoding, skipping conversion");
                needsConversion = false;
            }

            // 如果需要缩放或格式转换
            if (needsConversion) {
                LOGGER_DEBUG("Frame conversion needed for encoding");

                // 如果需要，初始化缩放上下文
                if (!streamCtx.swsContext) {
                    AVPixelFormat srcFormat = (AVPixelFormat)frameToEncode->format;
                    if (srcFormat == AV_PIX_FMT_DRM_PRIME) {
                        srcFormat = AV_PIX_FMT_NV12;  // DRM_PRIME的底层格式通常是NV12
                        LOGGER_WARNING("Converting DRM_PRIME to NV12 for scaling context");
                    }

                    streamCtx.swsContext = sws_getContext(
                            frameToEncode->width, frameToEncode->height,
                            srcFormat,
                            streamCtx.encoderContext->width, streamCtx.encoderContext->height,
                            streamCtx.encoderContext->pix_fmt,
                            SWS_BILINEAR, nullptr, nullptr, nullptr
                    );

                    if (!streamCtx.swsContext) {
                        throw FFmpegException("Failed to create scaling context for encoding");
                    }
                    LOGGER_INFO("Created scaling context for encoding conversion");
                }

                // 分配转换输出帧
                if (!streamCtx.hwFrame->data[0] || hwFrameUsed) {
                    if (hwFrameUsed) {
                        av_frame_unref(streamCtx.hwFrame);
                    }
                    streamCtx.hwFrame->format = streamCtx.encoderContext->pix_fmt;
                    streamCtx.hwFrame->width = streamCtx.encoderContext->width;
                    streamCtx.hwFrame->height = streamCtx.encoderContext->height;
                    ret = av_frame_get_buffer(streamCtx.hwFrame, 0);
                    if (ret < 0) {
                        throw FFmpegException("Failed to allocate frame buffer for conversion", ret);
                    }
                }

                // 执行转换
                ret = sws_scale(streamCtx.swsContext, frameToEncode->data,
                                frameToEncode->linesize, 0, frameToEncode->height,
                                streamCtx.hwFrame->data, streamCtx.hwFrame->linesize);
                if (ret < 0) {
                    throw FFmpegException("Error during frame conversion for encoding", ret);
                }

                streamCtx.hwFrame->pts = frameToEncode->pts;
                frameToEncode = streamCtx.hwFrame;
                hwFrameUsed = true;
            }

            // 重新计算时间戳
            frameToEncode->pts = av_rescale_q(
                    frameToEncode->pts,
                    streamCtx.decoderContext->time_base,
                    streamCtx.encoderContext->time_base
            );

            // 将帧发送到编码器
            ret = avcodec_send_frame(streamCtx.encoderContext, frameToEncode);
            if (ret < 0) {
                throw FFmpegException("Error sending frame to encoder", ret);
            }

            // 处理编码后的数据包
            while (ret >= 0) {
                AVPacket* encodedPacket = av_packet_alloc();
                if (!encodedPacket) {
                    throw FFmpegException("Failed to allocate encoded packet");
                }

                ret = avcodec_receive_packet(streamCtx.encoderContext, encodedPacket);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_free(&encodedPacket);
                    break;
                } else if (ret < 0) {
                    av_packet_free(&encodedPacket);
                    throw FFmpegException("Error receiving packet from encoder", ret);
                }

                // 准备输出数据包
                encodedPacket->stream_index = streamCtx.outputStream->index;

                // 重新计算时间戳
                av_packet_rescale_ts(
                        encodedPacket,
                        streamCtx.encoderContext->time_base,
                        streamCtx.outputStream->time_base
                );

                // 写入数据包
                ret = av_interleaved_write_frame(outputFormatContext_, encodedPacket);

                // 无论写入是否成功，都释放数据包
                av_packet_free(&encodedPacket);

                if (ret < 0) {
                    throw FFmpegException("Error writing encoded packet", ret);
                }
            }

            // 完成后始终取消引用帧
            if (frameAcquired) {
                av_frame_unref(streamCtx.frame);
            }

            if (hwFrameUsed) {
                av_frame_unref(streamCtx.hwFrame);
            }

            if (callbackFrameUsed && callbackFrame) {
                av_frame_free(&callbackFrame);
            }
        }
    } catch (const FFmpegException& e) {
        // 即使在异常情况下也清理帧
        if (frameAcquired) {
            av_frame_unref(streamCtx.frame);
        }

        if (hwFrameUsed) {
            av_frame_unref(streamCtx.hwFrame);
        }

        if (callbackFrameUsed && callbackFrame) {
            av_frame_free(&callbackFrame);
        }

        // 重新抛出异常
        throw;
    }
}

//void StreamProcessor::processVideoPacket(AVPacket* packet, StreamContext& streamCtx) {
//    int ret;
//    bool frameAcquired = false;
//    bool hwFrameUsed = false;
//
//    try {
//        // 将数据包发送到解码器
//        ret = avcodec_send_packet(streamCtx.decoderContext, packet);
//        if (ret < 0) {
//            throw FFmpegException("Error sending packet to decoder", ret);
//        }
//
//        while (ret >= 0) {
//            // 在每个循环开始时重置标志
//            frameAcquired = false;
//            hwFrameUsed = false;
//
//            // 接收解码后的帧
//            ret = avcodec_receive_frame(streamCtx.decoderContext, streamCtx.frame);
//            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
//                LOGGER_DEBUG("decoder frame is failed, errorCode is: " + std::to_string(ret));
//                break;
//            } else if (ret < 0) {
//                throw FFmpegException("Error receiving frame from decoder", ret);
//            }
//
//            frameAcquired = true;
//
//            // 更新最后一帧时间以防止重连
//            lastFrameTime_ = av_gettime();
//
//            // 仅拉流模式或正常模式都可以处理帧
//            int64_t pts = streamCtx.frame->pts;
//            if (streamCtx.inputStream) {
//                // 转换为毫秒时间戳
//                pts = av_rescale_q(pts, streamCtx.inputStream->time_base, {1, 1000});
//            }
//
////            LOGGER_DEBUG("帧格式为: " + std::to_string(streamCtx.frame->format) + " 硬件帧格式为: " + std::to_string(streamCtx.hwFrame->format));
//
//            if (streamCtx.frame->format == AV_PIX_FMT_DRM_PRIME) {
////                LOGGER_DEBUG("Detected DRM_PRIME format (179), converting to NV12 for processing");
//
//                // Prepare the hw frame for NV12 format
//                streamCtx.hwFrame->width = streamCtx.frame->width;
//                streamCtx.hwFrame->height = streamCtx.frame->height;
//                streamCtx.hwFrame->format = AV_PIX_FMT_NV12;  // Force NV12 format
//
//                // Allocate frame buffer if needed
//                if (!streamCtx.hwFrame->data[0]) {
//                    ret = av_frame_get_buffer(streamCtx.hwFrame, 32);  // 32-byte alignment
//                    if (ret < 0) {
//                        throw FFmpegException("Failed to allocate buffer for NV12 frame conversion", ret);
//                    }
//                }
//
//                // Try standard hardware frame transfer
//                ret = av_hwframe_transfer_data(streamCtx.hwFrame, streamCtx.frame, 0);
//                if (ret < 0) {
//                    // If standard transfer fails, we need to handle this in a specialized way
//                    LOGGER_WARNING("Standard hardware frame transfer failed for DRM_PRIME, using fallback method");
//
//                    // RKMPP specific handling - If needed, implement custom mapping here
//                    // This is placeholder code - actual implementation depends on your SDK
//
//                    // Just to avoid throwing an exception in this example:
//                    if (streamCtx.swsContext == nullptr) {
//                        streamCtx.swsContext = sws_getContext(
//                                streamCtx.frame->width, streamCtx.frame->height,
//                                AV_PIX_FMT_YUV420P,  // Pretend it's YUV420P as an intermediate
//                                streamCtx.frame->width, streamCtx.frame->height,
//                                AV_PIX_FMT_NV12,
//                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
//                        );
//
//                        if (!streamCtx.swsContext) {
//                            throw FFmpegException("Failed to create scaling context for DRM_PRIME conversion");
//                        }
//                    }
//
//                    // This is a last resort fallback - it might not work correctly
//                    // depending on how RKMPP exposes the actual pixel data
//                    sws_scale(streamCtx.swsContext, streamCtx.frame->data,
//                              streamCtx.frame->linesize, 0, streamCtx.frame->height,
//                              streamCtx.hwFrame->data, streamCtx.hwFrame->linesize);
//                }
//
//                hwFrameUsed = true;
//
////                LOGGER_DEBUG("DRM_PRIME frame converted to NV12 format");
//            }
//
//            // 调用帧处理函数
//            handleVideoFrame(streamCtx.hwFrame, pts, (int)av_q2d(streamCtx.inputStream->avg_frame_rate));
//
//            // 如果是仅拉流模式，我们不需要编码或写入帧
//            if (!config_.pushEnabled) {
//                // 释放帧
//                if (frameAcquired) {
//                    av_frame_unref(streamCtx.frame);
//                }
//
//                if (hwFrameUsed) {
//                    av_frame_unref(streamCtx.hwFrame);
//                }
//
//                continue;
//            }
//
//            AVFrame* frameToEncode = streamCtx.frame;
//
//            // 处理硬件加速帧
//            if (streamCtx.frame->format == AV_PIX_FMT_CUDA ||
//                streamCtx.frame->format == AV_PIX_FMT_VAAPI ||
//                streamCtx.frame->format == AV_PIX_FMT_QSV ||
//                streamCtx.frame->format == AV_PIX_FMT_D3D11 ||
//                streamCtx.frame->format == AV_PIX_FMT_VIDEOTOOLBOX ||
//                streamCtx.frame->format == AV_PIX_FMT_DRM_PRIME ||
//                streamCtx.frame->format == AV_PIX_FMT_NV12
//                ) {
//                // 如果需要，将硬件帧转换为软件帧
//                if (!streamCtx.encoderContext->hw_frames_ctx) {
//                    ret = av_hwframe_transfer_data(streamCtx.hwFrame, streamCtx.frame, 0);
//                    if (ret < 0) {
//                        throw FFmpegException("Error transferring data from hardware frame", ret);
//                    }
//                    frameToEncode = streamCtx.hwFrame;
//                    hwFrameUsed = true;
//
//                    // 处理可能的无效格式
//                    if (frameToEncode->format == AV_PIX_FMT_NONE ||
//                        frameToEncode->format == AV_PIX_FMT_DRM_PRIME) {
//                        // 转换为编码器期望的格式，通常是NV12或YUV420P
//                        frameToEncode->format = streamCtx.encoderContext->pix_fmt;
//                        LOGGER_DEBUG("Corrected invalid pixel format");
//                    }
//                }
//            }
//
//            // 确定是否需要缩放或格式转换
//            bool needsConversion = false;
//
//            // 如果分辨率不同，需要缩放
//            if (streamCtx.encoderContext->width != frameToEncode->width ||
//                streamCtx.encoderContext->height != frameToEncode->height) {
//                needsConversion = true;
//            }
//
//            // 如果格式不同，可能需要转换（除非都是硬件格式）
//            if (streamCtx.encoderContext->pix_fmt != frameToEncode->format) {
//                // 检查是否都是硬件格式
//                bool bothHardwareFormats =
//                        (frameToEncode->format >= AV_PIX_FMT_CUDA &&
//                         streamCtx.encoderContext->pix_fmt >= AV_PIX_FMT_CUDA);
//
//                if (!bothHardwareFormats) {
//                    needsConversion = true;
//                } else {
//                    // 硬件格式之间的转换由硬件处理
//                    LOGGER_DEBUG("Both source and target are hardware formats, skipping software conversion");
//                    needsConversion = false;
//                }
//            }
//
//            // 如果使用硬件帧上下文，可以跳过转换
//            if (streamCtx.encoderContext->hw_frames_ctx && frameToEncode->format == AV_PIX_FMT_NV12) {
//                // 检查编码器是否支持NV12直接输入
//                bool directNV12Support = false;
//                if (std::string(avcodec_get_name(streamCtx.encoderContext->codec_id)).find("nvenc") != std::string::npos ||
//                    std::string(avcodec_get_name(streamCtx.encoderContext->codec_id)).find("qsv") != std::string::npos) {
//                    directNV12Support = true;
//                }
//
//                if (directNV12Support) {
//                    LOGGER_DEBUG("Using direct NV12 input to hardware encoder, skipping conversion");
//                    needsConversion = false;
//                }
//            }
//
//            // 如果需要缩放或格式转换
//            if (needsConversion) {
//                // 如果需要，初始化缩放上下文
//                if (!streamCtx.swsContext) {
//                    // 确保源格式是有效的
//                    AVPixelFormat srcFormat = (AVPixelFormat)frameToEncode->format;
//                    if (srcFormat == AV_PIX_FMT_NONE || srcFormat == AV_PIX_FMT_DRM_PRIME) {
//                        srcFormat = AV_PIX_FMT_NV12;  // 默认假设NV12
//                        LOGGER_WARNING("Invalid source format, assuming NV12");
//                    }
//
//                    streamCtx.swsContext = sws_getContext(
//                            frameToEncode->width, frameToEncode->height,
//                            srcFormat,
//                            streamCtx.encoderContext->width, streamCtx.encoderContext->height,
//                            streamCtx.encoderContext->pix_fmt,
//                            SWS_BILINEAR, nullptr, nullptr, nullptr
//                    );
//
//                    if (!streamCtx.swsContext) {
//                        LOGGER_ERROR("Failed to create scaling context: source format");
//                        throw FFmpegException("Failed to create scaling context");
//                    }
//
//                    LOGGER_INFO("Created scaling context");
//                }
//
//                // 为缩放输出分配帧缓冲区
//                if (!streamCtx.hwFrame->data[0]) {
//                    streamCtx.hwFrame->format = streamCtx.encoderContext->pix_fmt;
//                    streamCtx.hwFrame->width = streamCtx.encoderContext->width;
//                    streamCtx.hwFrame->height = streamCtx.encoderContext->height;
//                    ret = av_frame_get_buffer(streamCtx.hwFrame, 0);
//                    if (ret < 0) {
//                        throw FFmpegException("Failed to allocate frame buffer for scaling", ret);
//                    }
//                }
//
//                // 缩放帧
//                ret = sws_scale(streamCtx.swsContext, frameToEncode->data,
//                                frameToEncode->linesize, 0, frameToEncode->height,
//                                streamCtx.hwFrame->data, streamCtx.hwFrame->linesize);
//                if (ret < 0) {
//                    throw FFmpegException("Error during frame scaling", ret);
//                }
//
//                streamCtx.hwFrame->pts = frameToEncode->pts;
//                frameToEncode = streamCtx.hwFrame;
//                hwFrameUsed = true;
//            }
//
//            // 重新计算时间戳
//            frameToEncode->pts = av_rescale_q(
//                    frameToEncode->pts,
//                    streamCtx.decoderContext->time_base,
//                    streamCtx.encoderContext->time_base
//            );
//
//            // 将帧发送到编码器
//            ret = avcodec_send_frame(streamCtx.encoderContext, frameToEncode);
//            if (ret < 0) {
//                throw FFmpegException("Error sending frame to encoder", ret);
//            }
//
//            // 处理编码后的数据包
//            while (ret >= 0) {
//                AVPacket* encodedPacket = av_packet_alloc();
//                if (!encodedPacket) {
//                    throw FFmpegException("Failed to allocate encoded packet");
//                }
//
//                ret = avcodec_receive_packet(streamCtx.encoderContext, encodedPacket);
//                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
//                    av_packet_free(&encodedPacket);
//                    break;
//                } else if (ret < 0) {
//                    av_packet_free(&encodedPacket);
//                    throw FFmpegException("Error receiving packet from encoder", ret);
//                }
//
//                // 准备输出数据包
//                encodedPacket->stream_index = streamCtx.outputStream->index;
//
//                // 重新计算时间戳
//                av_packet_rescale_ts(
//                        encodedPacket,
//                        streamCtx.encoderContext->time_base,
//                        streamCtx.outputStream->time_base
//                );
//
//                // 写入数据包
//                ret = av_interleaved_write_frame(outputFormatContext_, encodedPacket);
//
//                // 无论写入是否成功，都释放数据包
//                av_packet_free(&encodedPacket);
//
//                if (ret < 0) {
//                    throw FFmpegException("Error writing encoded packet", ret);
//                }
//            }
//
//            // 完成后始终取消引用帧
//            if (frameAcquired) {
//                av_frame_unref(streamCtx.frame);
//            }
//
//            if (hwFrameUsed) {
//                av_frame_unref(streamCtx.hwFrame);
//            }
//        }
//    } catch (const FFmpegException& e) {
//        // 即使在异常情况下也清理帧
//        if (frameAcquired) {
//            av_frame_unref(streamCtx.frame);
//        }
//
//        if (hwFrameUsed) {
//            av_frame_unref(streamCtx.hwFrame);
//        }
//
//        // 重新抛出异常
//        throw;
//    }
//}

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

        // 更新最后一帧时间
        lastFrameTime_ = av_gettime();

        // 处理音频帧
        int64_t pts = streamCtx.frame->pts;
        if (streamCtx.inputStream) {
            // 转换为毫秒时间戳
            pts = av_rescale_q(pts, streamCtx.inputStream->time_base, {1, 1000});
        }

        // 调用帧处理函数
        handleAudioFrame(streamCtx.frame, pts, (int)av_q2d(streamCtx.inputStream->avg_frame_rate));

        // 如果是仅拉流模式，我们不需要编码或写入帧
        if (!config_.pushEnabled) {
            // 释放帧
            av_frame_unref(streamCtx.frame);
            continue;
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
    std::lock_guard<std::mutex> lock(ffmpegMutex_); // 为清理过程添加互斥锁保护

    // 首先将上下文标记为无效，防止其他线程使用它
    isContextValid_ = false;

    try {
        // 清理视频流
        for (auto& streamCtx : videoStreams_) {
            // 安全地清理数据包
            if (streamCtx.packet) {
                av_packet_free(&streamCtx.packet);
                streamCtx.packet = nullptr;
            }

            // 安全地清理帧
            if (streamCtx.frame) {
                av_frame_free(&streamCtx.frame);
                streamCtx.frame = nullptr;
            }

            // 安全地清理硬件帧
            if (streamCtx.hwFrame) {
                av_frame_free(&streamCtx.hwFrame);
                streamCtx.hwFrame = nullptr;
            }

            // 安全地清理缩放上下文
            if (streamCtx.swsContext) {
                sws_freeContext(streamCtx.swsContext);
                streamCtx.swsContext = nullptr;
            }

            // 安全地清理编解码器上下文
            if (streamCtx.decoderContext) {
                avcodec_free_context(&streamCtx.decoderContext);
                streamCtx.decoderContext = nullptr;
            }

            if (streamCtx.encoderContext) {
                avcodec_free_context(&streamCtx.encoderContext);
                streamCtx.encoderContext = nullptr;
            }

            // 安全地清理硬件设备上下文
            if (streamCtx.hwDeviceContext) {
                av_buffer_unref(&streamCtx.hwDeviceContext);
                streamCtx.hwDeviceContext = nullptr;
            }
        }

        // 清理音频流（类似的模式）
        for (auto& streamCtx : audioStreams_) {
            if (streamCtx.packet) {
                av_packet_free(&streamCtx.packet);
                streamCtx.packet = nullptr;
            }

            if (streamCtx.frame) {
                av_frame_free(&streamCtx.frame);
                streamCtx.frame = nullptr;
            }

            if (streamCtx.decoderContext) {
                avcodec_close(streamCtx.decoderContext);
                avcodec_free_context(&streamCtx.decoderContext);
                streamCtx.decoderContext = nullptr;
            }

            if (streamCtx.encoderContext) {
                avcodec_close(streamCtx.encoderContext);
                avcodec_free_context(&streamCtx.encoderContext);
                streamCtx.encoderContext = nullptr;
            }
        }

        // 清理格式上下文
        if (config_.pushEnabled && outputFormatContext_) {
            // 如果需要，写入尾部
            if (isContextValid_ && outputFormatContext_->pb) {
                av_write_trailer(outputFormatContext_);
            }

            if (!(outputFormatContext_->oformat->flags & AVFMT_NOFILE) &&
                outputFormatContext_->pb) {
                avio_closep(&outputFormatContext_->pb);
            }

            avformat_free_context(outputFormatContext_);
            outputFormatContext_ = nullptr;
        }

        if (inputFormatContext_) {
            avformat_close_input(&inputFormatContext_);
            inputFormatContext_ = nullptr;
        }

        // 清空流容器
        videoStreams_.clear();
        audioStreams_.clear();

        LOGGER_DEBUG("Resources cleanup completed for stream " + config_.id);
    } catch (const std::exception& e) {
        LOGGER_ERROR("Exception during resource cleanup: " + std::string(e.what()));
        // 即使发生异常，也确保将指针置空以防止释放后使用
        inputFormatContext_ = nullptr;
        outputFormatContext_ = nullptr;
        videoStreams_.clear();
        audioStreams_.clear();
    }
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
    // 如果已经达到最大重连尝试次数，不要继续
    if (reconnectAttempts_ >= maxReconnectAttempts_) {
        LOGGER_ERROR("Stream " + config_.id + " exceeded maximum reconnection attempts ("
                      + std::to_string(maxReconnectAttempts_) + ")");
        return false;
    }

    LOGGER_WARNING("Attempting to reconnect stream: " + config_.id + " (attempt "
                    + std::to_string(reconnectAttempts_ + 1) + "/"
                    + std::to_string(maxReconnectAttempts_) + ")");

    isReconnecting_ = true;
    reconnectAttempts_++;

    // 计算重连延迟（指数退避）
    int64_t reconnectDelay = 1000000 * (1 << (reconnectAttempts_ - 1));
    reconnectDelay = std::min(reconnectDelay, (int64_t)30000000); // 最多30秒

    LOGGER_INFO("Waiting " + std::to_string(reconnectDelay / 1000000) + " seconds before reconnecting");
    av_usleep(reconnectDelay);

    try {
        // 重要：首先清理所有旧资源
        cleanup();

        // 然后初始化新连接
        initialize();

        // 成功后重置状态变量
        LOGGER_INFO("Successfully reconnected stream: " + config_.id);
        resetErrorState();
        isReconnecting_ = false;
        return true;
    } catch (const FFmpegException& e) {
        LOGGER_ERROR("Failed to reconnect stream " + config_.id + ": " + std::string(e.what()));

        // 确保即使在失败后也处于一致状态
        try {
            // 额外的清理尝试，确保资源被释放
            cleanup();
        } catch (...) {
            LOGGER_ERROR("Error during cleanup after failed reconnection");
        }

        isReconnecting_ = false;

        // 只有在达到最大尝试次数时才设置错误标志
        if (reconnectAttempts_ >= maxReconnectAttempts_) {
            hasError_ = true;
        }

        return false;
    }
}

// 设置视频帧回调函数
void StreamProcessor::setVideoFrameCallback(VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    videoFrameCallback_ = callback;
    LOGGER_INFO("Video frame callback set for stream: " + config_.id);
}

// 设置音频帧回调函数
void StreamProcessor::setAudioFrameCallback(AudioFrameCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    audioFrameCallback_ = callback;
    LOGGER_INFO("Audio frame callback set for stream: " + config_.id);
}

// 处理视频帧的方法
void StreamProcessor::handleVideoFrame(const AVFrame* frame, int64_t pts, int fps) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (videoFrameCallback_) {
        try {
            videoFrameCallback_(frame, pts, fps);
        } catch (const std::exception& e) {
            LOGGER_ERROR("Exception in video frame callback: " + std::string(e.what()));
        } catch (...) {
            LOGGER_ERROR("Unknown exception in video frame callback");
        }
    }
}

// 处理音频帧的方法
void StreamProcessor::handleAudioFrame(const AVFrame* frame, int64_t pts, int fps) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    if (audioFrameCallback_) {
        try {
            audioFrameCallback_(frame, pts, fps);
        } catch (const std::exception& e) {
            LOGGER_ERROR("Exception in audio frame callback: " + std::string(e.what()));
        } catch (...) {
            LOGGER_ERROR("Unknown exception in audio frame callback");
        }
    }
}
