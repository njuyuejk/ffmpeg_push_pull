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
          decoderModule_(std::make_unique<DecoderModule>(config)),
          encoderModule_(std::make_unique<EncoderModule>(config)) {

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
    Logger::info("Request to stop stream: " + config_.id);

    // 首先将运行标志设置为false，通知处理线程退出
    bool wasRunning = isRunning_.exchange(false);

    // 如果之前不是运行状态，直接返回
    if (!wasRunning) {
        Logger::info("Stream " + config_.id + " already stopped, no action needed");
        return;
    }

    // 检查处理线程是否存在和可加入
    if (!processingThread_.joinable()) {
        Logger::info("Processing thread for stream " + config_.id + " is not joinable");
        return;
    }

    try {
        // 设置线程分离标志
        bool shouldDetach = false;

        // 尝试温和地等待线程结束
        {
            Logger::debug("Waiting for processing thread to end for stream: " + config_.id);

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
                    Logger::error("Exception joining processing thread: " + std::string(e.what()));
                } catch (...) {
                    Logger::error("Unknown exception joining processing thread");
                }
            });

            // 等待最多5秒
            for (int i = 0; i < 10 && !threadJoined; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (!threadJoined) {
                Logger::warning("Timeout joining processing thread for stream: " + config_.id);
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
                Logger::info("Detached processing thread for stream " + config_.id);
            } catch (const std::exception& e) {
                Logger::error("Exception detaching processing thread: " + std::string(e.what()));
            }
        }

        // 确保进行完整的资源清理
        cleanup();

        Logger::info("Stream " + config_.id + " successfully stopped");
    } catch (const std::exception& e) {
        Logger::error("Exception stopping stream " + config_.id + ": " + std::string(e.what()));

        // 最后的安全措施 - 尝试分离线程而不是让它挂起
        if (processingThread_.joinable()) {
            try {
                processingThread_.detach();
            } catch (...) {
                Logger::error("Final attempt to detach processing thread failed");
            }
        }
    } catch (...) {
        Logger::error("Unknown exception stopping stream " + config_.id);

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
    try {
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

        isContextValid_ = true;
    } catch (const FFmpegException& e) {
        cleanup();
        throw; // 清理后重新抛出异常
    } catch (const std::exception& e) {
        cleanup();
        throw FFmpegException("Initialization error: " + std::string(e.what()));
    }
}

void StreamProcessor::openInput() {
    // 使用DecoderModule打开输入
    inputFormatContext_ = decoderModule_->openInput(config_.inputUrl);
    if (!inputFormatContext_) {
        throw FFmpegException("Failed to open input: " + config_.inputUrl);
    }
}

void StreamProcessor::openOutput() {
    // 使用EncoderModule打开输出
    outputFormatContext_ = encoderModule_->openOutput(config_.outputUrl, config_.outputFormat);
    if (!outputFormatContext_) {
        throw FFmpegException("Failed to open output: " + config_.outputUrl);
    }
}

AVBufferRef* StreamProcessor::createHardwareDevice() {
    // 使用EncoderModule创建硬件设备
    return encoderModule_->createHardwareDevice();
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
    StreamContext streamCtx;
    streamCtx.streamIndex = inputStream->index;
    streamCtx.inputStream = inputStream;

    // 如果启用了硬件加速，创建硬件设备上下文
    if (config_.enableHardwareAccel && !config_.hwaccelType.empty() && config_.hwaccelType != "none") {
        streamCtx.hwDeviceContext = createHardwareDevice();
    }

    // 初始化解码器
    streamCtx.decoderContext = decoderModule_->initializeVideoDecoder(inputStream, streamCtx.hwDeviceContext);
    if (!streamCtx.decoderContext) {
        Logger::warning("Failed to initialize video decoder");
        return;
    }

    // 创建输出流
    AVStream* outputStream = avformat_new_stream(outputFormatContext_, nullptr);
    if (!outputStream) {
        throw FFmpegException("Failed to create output video stream");
    }

    streamCtx.outputStream = outputStream;

    // 初始化编码器
    streamCtx.encoderContext = encoderModule_->initializeVideoEncoder(
            streamCtx.decoderContext,
            streamCtx.hwDeviceContext,
            outputStream);

    if (!streamCtx.encoderContext) {
        throw FFmpegException("Failed to initialize video encoder");
    }

    // 分配数据包和帧
    streamCtx.packet = av_packet_alloc();
    streamCtx.frame = av_frame_alloc();
    streamCtx.hwFrame = av_frame_alloc();

    if (!streamCtx.packet || !streamCtx.frame || !streamCtx.hwFrame) {
        throw FFmpegException("Failed to allocate packet or frame");
    }

    // 添加到视频流列表
    videoStreams_.push_back(streamCtx);
    Logger::info("Set up video stream: input#" + std::to_string(inputStream->index) +
                 " -> output#" + std::to_string(outputStream->index));
}

void StreamProcessor::setupAudioStream(AVStream* inputStream) {
    StreamContext streamCtx;
    streamCtx.streamIndex = inputStream->index;
    streamCtx.inputStream = inputStream;

    // 初始化解码器
    streamCtx.decoderContext = decoderModule_->initializeAudioDecoder(inputStream);
    if (!streamCtx.decoderContext) {
        Logger::warning("Failed to initialize audio decoder");
        return;
    }

    // 创建输出流
    AVStream* outputStream = avformat_new_stream(outputFormatContext_, nullptr);
    if (!outputStream) {
        throw FFmpegException("Failed to create output audio stream");
    }

    streamCtx.outputStream = outputStream;

    // 初始化编码器
    streamCtx.encoderContext = encoderModule_->initializeAudioEncoder(
            streamCtx.decoderContext,
            outputStream);

    if (!streamCtx.encoderContext) {
        throw FFmpegException("Failed to initialize audio encoder");
    }

    // 分配数据包和帧
    streamCtx.packet = av_packet_alloc();
    streamCtx.frame = av_frame_alloc();

    if (!streamCtx.packet || !streamCtx.frame) {
        throw FFmpegException("Failed to allocate packet or frame");
    }

    // 添加到音频流列表
    audioStreams_.push_back(streamCtx);
    Logger::info("Set up audio stream: input#" + std::to_string(inputStream->index) +
                 " -> output#" + std::to_string(outputStream->index));
}

// 添加到processLoop方法中，增强调试信息输出

void StreamProcessor::processLoop() {
    AVPacket* packet = nullptr;
    try {
        packet = av_packet_alloc();
        if (!packet) {
            Logger::error("Failed to allocate packet memory");
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
        Logger::info("Starting process loop for stream: " + config_.id);

        // 处理循环
        while (isRunning_) {
            try {
                // 检查退出信号
                if (!isRunning_) {
                    Logger::info("Exit signal detected for stream: " + config_.id);
                    break;
                }

                // 检查流是否停滞
                if (isStreamStalled() && !reconnectInProgress) {
                    Logger::warning("Stream " + config_.id + " appears stalled (no data for "
                                    + std::to_string(noDataTimeout_ / 1000000) + " seconds)");

                    reconnectInProgress = true;
                    if (!reconnect()) {
                        Logger::error("Failed to reconnect stream " + config_.id + ", entering wait mode");
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
                    Logger::error("Input context invalid for stream: " + config_.id);
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
                        Logger::info("End of input stream: " + config_.id);
                        break;
                    } else if (ret == AVERROR(EAGAIN)) {
                        av_usleep(10000);  // 休眠10毫秒
                        continue;
                    } else {
                        streamErrorCount++;
                        if (streamErrorCount > MAX_ERRORS_BEFORE_RECONNECT && !reconnectInProgress) {
                            Logger::warning("Too many consecutive errors (" + std::to_string(streamErrorCount)
                                            + "), attempting reconnection");

                            reconnectInProgress = true;
                            if (!reconnect()) {
                                Logger::error("Failed to reconnect stream " + config_.id + ", entering wait mode");
                                hasError_ = true;
                                av_usleep(5000000); // 5秒
                                reconnectInProgress = false;
                                continue;
                            }
                            reconnectInProgress = false;
                            streamErrorCount = 0;
                            continue;
                        }

                        Logger::warning("Error reading frame (attempt " + std::to_string(streamErrorCount) +
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
                            Logger::error("Error processing video packet: " + std::string(e.what()));
                            // If error is related to writing, attempt reconnection
                            if (std::string(e.what()).find("Error writing encoded packet") != std::string::npos) {
                                if (!reconnectInProgress) {
                                    Logger::warning("Output error detected, attempting reconnection");
                                    reconnectInProgress = true;
                                    if (!reconnect()) {
                                        // Just log and continue - don't exit the loop
                                        Logger::error("Failed to reconnect stream " + config_.id + " after output error");
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
                                Logger::error("Error processing audio packet: " + std::string(e.what()));
                                // Handle output errors similar to video
                                if (std::string(e.what()).find("Error writing encoded packet") != std::string::npos) {
                                    if (!reconnectInProgress) {
                                        Logger::warning("Output error detected, attempting reconnection");
                                        reconnectInProgress = true;
                                        if (!reconnect()) {
                                            Logger::error("Failed to reconnect stream " + config_.id + " after output error");
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

                    Logger::debug("Stream " + config_.id + " processed " +
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

                Logger::error("Stream processing error: " + std::string(e.what()));

                if (!reconnectInProgress && isRunning_) {
                    Logger::warning("Stream error detected, attempting reconnection");
                    reconnectInProgress = true;
                    if (!reconnect()) {
                        Logger::error("Failed to reconnect stream " + config_.id + ", entering wait mode");
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

                Logger::error("Standard exception in stream " + config_.id + ": " + std::string(e.what()));
                av_usleep(1000000); // 1秒
            } catch (...) {
                // 安全地释放包
                if (packet) {
                    av_packet_unref(packet);
                }

                Logger::error("Unknown exception in stream " + config_.id);
                av_usleep(1000000); // 1秒
            }
        }

        // 计算总处理时间和平均帧率
        int64_t endTime = av_gettime() / 1000000;
        int64_t totalTime = endTime - startTime;
        double avgFps = totalTime > 0 ? (double)frameCount / totalTime : 0;

        Logger::info("Exiting process loop for stream: " + config_.id + " after " +
                     std::to_string(totalTime) + " seconds, processed " +
                     std::to_string(frameCount) + " frames (avg. " +
                     std::to_string(avgFps) + " fps)");

    } catch (const std::exception& e) {
        Logger::error("Critical exception in stream processing loop: " + std::string(e.what()));
        hasError_ = true;
    } catch (...) {
        Logger::error("Unknown critical exception in stream processing loop");
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

            // 接收解码后的帧
            ret = avcodec_receive_frame(streamCtx.decoderContext, streamCtx.frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            } else if (ret < 0) {
                throw FFmpegException("Error receiving frame from decoder", ret);
            }

            frameAcquired = true;
            AVFrame* frameToEncode = streamCtx.frame;

            // 处理硬件加速帧
            if (streamCtx.frame->format == AV_PIX_FMT_CUDA ||
                streamCtx.frame->format == AV_PIX_FMT_VAAPI ||
                streamCtx.frame->format == AV_PIX_FMT_QSV ||
                streamCtx.frame->format == AV_PIX_FMT_D3D11 ||
                streamCtx.frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {

                // 如果需要，将硬件帧转换为软件帧
                if (!streamCtx.encoderContext->hw_frames_ctx) {
                    ret = av_hwframe_transfer_data(streamCtx.hwFrame, streamCtx.frame, 0);
                    if (ret < 0) {
                        throw FFmpegException("Error transferring data from hardware frame", ret);
                    }
                    frameToEncode = streamCtx.hwFrame;
                    hwFrameUsed = true;
                }
            }

            // 如果需要缩放
            if (streamCtx.encoderContext->width != frameToEncode->width ||
                streamCtx.encoderContext->height != frameToEncode->height ||
                streamCtx.encoderContext->pix_fmt != frameToEncode->format) {

                // 如果需要，初始化缩放上下文
                if (!streamCtx.swsContext) {
                    streamCtx.swsContext = sws_getContext(
                            frameToEncode->width, frameToEncode->height,
                            (AVPixelFormat)frameToEncode->format,
                            streamCtx.encoderContext->width, streamCtx.encoderContext->height,
                            streamCtx.encoderContext->pix_fmt,
                            SWS_BILINEAR, nullptr, nullptr, nullptr
                    );

                    if (!streamCtx.swsContext) {
                        throw FFmpegException("Failed to create scaling context");
                    }
                }

                // 为缩放输出分配帧缓冲区
                if (!streamCtx.hwFrame->data[0]) {
                    streamCtx.hwFrame->format = streamCtx.encoderContext->pix_fmt;
                    streamCtx.hwFrame->width = streamCtx.encoderContext->width;
                    streamCtx.hwFrame->height = streamCtx.encoderContext->height;
                    ret = av_frame_get_buffer(streamCtx.hwFrame, 0);
                    if (ret < 0) {
                        throw FFmpegException("Failed to allocate frame buffer for scaling", ret);
                    }
                }

                // 缩放帧
                ret = sws_scale(streamCtx.swsContext, frameToEncode->data,
                                frameToEncode->linesize, 0, frameToEncode->height,
                                streamCtx.hwFrame->data, streamCtx.hwFrame->linesize);
                if (ret < 0) {
                    throw FFmpegException("Error during frame scaling", ret);
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
        }
    } catch (const FFmpegException& e) {
        // 即使在异常情况下也清理帧
        if (frameAcquired) {
            av_frame_unref(streamCtx.frame);
        }

        if (hwFrameUsed) {
            av_frame_unref(streamCtx.hwFrame);
        }

        // 重新抛出异常
        throw;
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
        if (outputFormatContext_) {
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

        Logger::debug("Resources cleanup completed for stream " + config_.id);
    } catch (const std::exception& e) {
        Logger::error("Exception during resource cleanup: " + std::string(e.what()));
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
        Logger::error("Stream " + config_.id + " exceeded maximum reconnection attempts ("
                      + std::to_string(maxReconnectAttempts_) + ")");
        return false;
    }

    Logger::warning("Attempting to reconnect stream: " + config_.id + " (attempt "
                    + std::to_string(reconnectAttempts_ + 1) + "/"
                    + std::to_string(maxReconnectAttempts_) + ")");

    isReconnecting_ = true;
    reconnectAttempts_++;

    // 计算重连延迟（指数退避）
    int64_t reconnectDelay = 1000000 * (1 << (reconnectAttempts_ - 1));
    reconnectDelay = std::min(reconnectDelay, (int64_t)30000000); // 最多30秒

    Logger::info("Waiting " + std::to_string(reconnectDelay / 1000000) + " seconds before reconnecting");
    av_usleep(reconnectDelay);

    try {
        // 重要：首先清理所有旧资源
        cleanup();

        // 然后初始化新连接
        initialize();

        // 成功后重置状态变量
        Logger::info("Successfully reconnected stream: " + config_.id);
        resetErrorState();
        isReconnecting_ = false;
        return true;
    } catch (const FFmpegException& e) {
        Logger::error("Failed to reconnect stream " + config_.id + ": " + std::string(e.what()));

        // 确保即使在失败后也处于一致状态
        try {
            // 额外的清理尝试，确保资源被释放
            cleanup();
        } catch (...) {
            Logger::error("Error during cleanup after failed reconnection");
        }

        isReconnecting_ = false;

        // 只有在达到最大尝试次数时才设置错误标志
        if (reconnectAttempts_ >= maxReconnectAttempts_) {
            hasError_ = true;
        }

        return false;
    }
}
