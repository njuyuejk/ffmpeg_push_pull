#include "ffmpeg_base/TrackingStreamProcessor.h"
#include "common/opencv2avframe.h"
#include "logger/Logger.h"
#include <random>
#include <algorithm>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/time.h>
}

TrackingStreamProcessor::TrackingStreamProcessor(const StreamConfig& inputConfig, const StreamConfig& outputConfig)
        : inputConfig_(inputConfig), outputConfig_(outputConfig),
          running_(false), hasError_(false),
          trackerType_("CSRT"), nextTargetId_(1),
          detectionInterval_(5), frameCount_(0), maxTargets_(10),
          outputFormatContext_(nullptr), encoderContext_(nullptr),
          outputStream_(nullptr), swsContext_(nullptr) {

    LOGGER_INFO("创建跟踪流处理器: " + inputConfig_.id + " -> " + outputConfig_.id);
}

TrackingStreamProcessor::~TrackingStreamProcessor() {
    stop();
    cleanup();
}

bool TrackingStreamProcessor::start() {
    if (running_) {
        LOGGER_WARNING("跟踪流处理器已在运行中");
        return true;
    }

    try {
        // 初始化输出流
        if (!initializeOutput()) {
            LOGGER_ERROR("初始化输出流失败");
            return false;
        }

        // 创建输入流处理器（仅拉流模式）
        StreamConfig inputConfigCopy = inputConfig_;
        inputConfigCopy.pushEnabled = false;  // 仅拉流
        inputConfigCopy.aiEnabled = false;    // 不使用内置AI

        inputProcessor_ = std::make_unique<StreamProcessor>(inputConfigCopy);

        // 设置视频帧回调
        inputProcessor_->setVideoFrameCallback([this](const AVFrame* frame, int64_t pts, int fps) {
            // 将AVFrame转换为cv::Mat并加入队列
            cv::Mat mat = AVFrameToMat(frame);
            if (!mat.empty()) {
                std::unique_lock<std::mutex> lock(inputQueueMutex_);

                // 控制队列大小
                while (inputFrameQueue_.size() >= MAX_QUEUE_SIZE) {
                    inputFrameQueue_.pop();
                }

                inputFrameQueue_.emplace(mat, pts, fps);
                lock.unlock();
                inputQueueCond_.notify_one();
            }
        });

        // 启动输入流处理器
        inputProcessor_->start();

        // 启动工作线程
        running_ = true;
        hasError_ = false;

        trackingThread_ = std::thread(&TrackingStreamProcessor::trackingThreadFunc, this);
        outputThread_ = std::thread(&TrackingStreamProcessor::outputThreadFunc, this);

        LOGGER_INFO("跟踪流处理器启动成功");
        return true;

    } catch (const std::exception& e) {
        LOGGER_ERROR("启动跟踪流处理器失败: " + std::string(e.what()));
        hasError_ = true;
        running_ = false;
        cleanup();
        return false;
    }
}

void TrackingStreamProcessor::stop() {
    if (!running_) {
        return;
    }

    LOGGER_INFO("停止跟踪流处理器");
    running_ = false;

    // 通知所有等待的线程
    inputQueueCond_.notify_all();
    outputQueueCond_.notify_all();

    // 停止输入流处理器
    if (inputProcessor_) {
        inputProcessor_->stop();
    }

    // 等待线程结束
    if (trackingThread_.joinable()) {
        trackingThread_.join();
    }

    if (outputThread_.joinable()) {
        outputThread_.join();
    }

    cleanup();
    LOGGER_INFO("跟踪流处理器已停止");
}

void TrackingStreamProcessor::setAIModel(std::unique_ptr<rknn_lite> model) {
    aiModel_ = std::move(model);
    LOGGER_INFO("AI模型已设置");
}

void TrackingStreamProcessor::setTrackerType(const std::string& trackerType) {
    trackerType_ = trackerType;
    LOGGER_INFO("跟踪器类型设置为: " + trackerType);
}

int TrackingStreamProcessor::getCurrentTargetCount() const {
    return static_cast<int>(targets_.size());
}

void TrackingStreamProcessor::setTrackingCallback(std::function<void(const std::vector<TrackingTarget>&)> callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    trackingCallback_ = callback;
}

void TrackingStreamProcessor::trackingThreadFunc() {
    LOGGER_INFO("跟踪线程启动");

    while (running_) {
        try {
            FrameData frameData;

            // 获取输入帧
            {
                std::unique_lock<std::mutex> lock(inputQueueMutex_);
                inputQueueCond_.wait(lock, [this] {
                    return !inputFrameQueue_.empty() || !running_;
                });

                if (!running_) break;

                if (!inputFrameQueue_.empty()) {
                    frameData = inputFrameQueue_.front();
                    inputFrameQueue_.pop();
                } else {
                    continue;
                }
            }

            if (!frameData.isValid || frameData.frame.empty()) {
                continue;
            }

            cv::Mat processedFrame = frameData.frame.clone();
            frameCount_++;

            // 定期进行AI检测
            if (frameCount_ % detectionInterval_ == 0 && aiModel_) {
                auto detections = runAIDetection(processedFrame);
                addNewTargets(processedFrame, detections);
            }

            // 更新跟踪器
            updateTrackers(processedFrame);

            // 移除不活跃的目标
            removeInactiveTargets();

            // 绘制跟踪结果
            drawTrackingResults(processedFrame);

            // 将处理后的帧加入输出队列
            {
                std::unique_lock<std::mutex> lock(outputQueueMutex_);

                // 控制队列大小
                while (outputFrameQueue_.size() >= MAX_QUEUE_SIZE) {
                    outputFrameQueue_.pop();
                }

                outputFrameQueue_.emplace(processedFrame, frameData.pts, frameData.fps);
                lock.unlock();
                outputQueueCond_.notify_one();
            }

            // 定期记录跟踪状态
            if (frameCount_ % 100 == 0) {
                logTrackingStatus();
            }

            // 触发回调
            notifyTrackingCallback();

        } catch (const std::exception& e) {
            LOGGER_ERROR("跟踪线程异常: " + std::string(e.what()));
            hasError_ = true;
        }
    }

    LOGGER_INFO("跟踪线程结束");
}

void TrackingStreamProcessor::outputThreadFunc() {
    LOGGER_INFO("输出线程启动");

    while (running_) {
        try {
            FrameData frameData;

            // 获取处理后的帧
            {
                std::unique_lock<std::mutex> lock(outputQueueMutex_);
                outputQueueCond_.wait(lock, [this] {
                    return !outputFrameQueue_.empty() || !running_;
                });

                if (!running_) break;

                if (!outputFrameQueue_.empty()) {
                    frameData = outputFrameQueue_.front();
                    outputFrameQueue_.pop();
                } else {
                    continue;
                }
            }

            if (!frameData.isValid || frameData.frame.empty()) {
                continue;
            }

            // 编码并发送帧
            if (!encodeAndSendFrame(frameData.frame, frameData.pts)) {
                LOGGER_WARNING("编码发送帧失败");
            }

        } catch (const std::exception& e) {
            LOGGER_ERROR("输出线程异常: " + std::string(e.what()));
            hasError_ = true;
        }
    }

    LOGGER_INFO("输出线程结束");
}

bool TrackingStreamProcessor::initializeOutput() {
    int ret;

    // 分配输出上下文
    ret = avformat_alloc_output_context2(&outputFormatContext_, nullptr,
                                         outputConfig_.outputFormat.c_str(),
                                         outputConfig_.outputUrl.c_str());
    if (ret < 0 || !outputFormatContext_) {
        LOGGER_ERROR("分配输出上下文失败");
        return false;
    }

    // 查找编码器
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        LOGGER_ERROR("找不到H.264编码器");
        return false;
    }

    // 创建输出流
    outputStream_ = avformat_new_stream(outputFormatContext_, encoder);
    if (!outputStream_) {
        LOGGER_ERROR("创建输出流失败");
        return false;
    }

    // 分配编码器上下文
    encoderContext_ = avcodec_alloc_context3(encoder);
    if (!encoderContext_) {
        LOGGER_ERROR("分配编码器上下文失败");
        return false;
    }

    // 设置编码参数
    encoderContext_->codec_id = encoder->id;
    encoderContext_->codec_type = AVMEDIA_TYPE_VIDEO;
    encoderContext_->width = 1920;  // 默认分辨率
    encoderContext_->height = 1080;
    encoderContext_->pix_fmt = AV_PIX_FMT_YUV420P;
    encoderContext_->bit_rate = outputConfig_.videoBitrate;
    encoderContext_->time_base = {1, 25};  // 25fps
    encoderContext_->gop_size = outputConfig_.keyframeInterval;
    encoderContext_->max_b_frames = 0;

    // 低延迟设置
    if (outputConfig_.lowLatencyMode) {
        encoderContext_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        av_opt_set(encoderContext_->priv_data, "preset", "ultrafast", 0);
        av_opt_set(encoderContext_->priv_data, "tune", "zerolatency", 0);
    }

    // 打开编码器
    ret = avcodec_open2(encoderContext_, encoder, nullptr);
    if (ret < 0) {
        LOGGER_ERROR("打开编码器失败");
        return false;
    }

    // 复制参数到输出流
    ret = avcodec_parameters_from_context(outputStream_->codecpar, encoderContext_);
    if (ret < 0) {
        LOGGER_ERROR("复制编码器参数失败");
        return false;
    }

    outputStream_->time_base = encoderContext_->time_base;

    // 打开输出URL
    if (!(outputFormatContext_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&outputFormatContext_->pb, outputConfig_.outputUrl.c_str(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            LOGGER_ERROR("打开输出URL失败: " + outputConfig_.outputUrl);
            return false;
        }
    }

    // 写入头部
    ret = avformat_write_header(outputFormatContext_, nullptr);
    if (ret < 0) {
        LOGGER_ERROR("写入输出头部失败");
        return false;
    }

    LOGGER_INFO("输出流初始化成功: " + outputConfig_.outputUrl);
    return true;
}

std::vector<DetectionResult> TrackingStreamProcessor::runAIDetection(const cv::Mat& frame) {
    std::vector<DetectionResult> results;

    if (!aiModel_) {
        return results;
    }

    try {
        // 设置输入图像
        aiModel_->ori_img = frame;

        // 运行推理
        if (aiModel_->interf()) {
            // 获取检测结果（这里需要根据具体的AI模型输出格式来解析）
            // 假设模型有检测结果的接口

            // 示例：解析YOLO格式的检测结果
            // 这里需要根据实际的rknn_lite接口来实现
            /*
            for (const auto& detection : aiModel_->detections) {
                DetectionResult result;
                result.bbox = detection.bbox;
                result.className = detection.className;
                result.confidence = detection.confidence;
                
                if (isValidDetection(result)) {
                    results.push_back(result);
                }
            }
            */
        }

    } catch (const std::exception& e) {
        LOGGER_ERROR("AI检测异常: " + std::string(e.what()));
    }

    return results;
}

bool TrackingStreamProcessor::isValidDetection(const DetectionResult& detection) {
    return detection.confidence >= MIN_DETECTION_CONFIDENCE / 100.0f &&
           detection.bbox.width > 10 && detection.bbox.height > 10;
}

cv::Ptr<cv::Tracker> TrackingStreamProcessor::createTracker() {
    cv::Ptr<cv::Tracker> tracker;

    if (trackerType_ == "CSRT") {
        tracker = cv::TrackerCSRT::create();
    } else if (trackerType_ == "KCF") {
        tracker = cv::TrackerKCF::create();
    } else {
        // 默认使用CSRT
        tracker = cv::TrackerCSRT::create();
    }

    return tracker;
}

void TrackingStreamProcessor::updateTrackers(cv::Mat& frame) {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    for (auto& target : targets_) {
        if (!target.isActive) continue;

        cv::Rect bbox;
        bool success = target.tracker->update(frame, bbox);

        if (success) {
            target.bbox = bbox;
            target.lostFrames = 0;
            target.lastSeenTime = av_gettime();

            // 更新轨迹
            cv::Point2f center(bbox.x + bbox.width/2, bbox.y + bbox.height/2);
            target.trajectory.push_back(center);

            // 限制轨迹点数量
            if (target.trajectory.size() > 50) {
                target.trajectory.erase(target.trajectory.begin());
            }
        } else {
            target.lostFrames++;
            if (target.lostFrames > MAX_LOST_FRAMES) {
                target.isActive = false;
            }
        }
    }
}

void TrackingStreamProcessor::addNewTargets(const cv::Mat& frame, const std::vector<DetectionResult>& detections) {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    for (const auto& detection : detections) {
        cv::Rect2d detectionRect(detection.bbox);

        // 检查是否与现有目标重叠
        bool overlaps = false;
        for (const auto& target : targets_) {
            if (target.isActive && isOverlapping(target.bbox, detectionRect)) {
                overlaps = true;
                break;
            }
        }

        // 如果不重叠且未达到最大目标数，添加新目标
        if (!overlaps && targets_.size() < maxTargets_) {
            TrackingTarget newTarget;
            newTarget.id = nextTargetId_++;
            newTarget.bbox = detectionRect;
            newTarget.className = detection.className;
            newTarget.confidence = detection.confidence;
            newTarget.lostFrames = 0;
            newTarget.lastSeenTime = av_gettime();
            newTarget.isActive = true;
            newTarget.color = generateColor(newTarget.id);

            // 创建跟踪器
            newTarget.tracker = createTracker();
            newTarget.tracker->init(frame, detectionRect);
            targets_.push_back(newTarget);
            LOGGER_INFO("添加新跟踪目标 ID:" + std::to_string(newTarget.id) +
                        " 类别:" + newTarget.className);

        }
    }
}

void TrackingStreamProcessor::removeInactiveTargets() {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    auto it = targets_.begin();
    while (it != targets_.end()) {
        if (!it->isActive) {
            LOGGER_INFO("移除不活跃目标 ID:" + std::to_string(it->id));
            it = targets_.erase(it);
        } else {
            ++it;
        }
    }
}

float TrackingStreamProcessor::calculateIoU(const cv::Rect2d& rect1, const cv::Rect2d& rect2) {
    cv::Rect2d intersection = rect1 & rect2;
    float intersectionArea = intersection.area();

    if (intersectionArea == 0) return 0.0f;

    float unionArea = rect1.area() + rect2.area() - intersectionArea;
    return intersectionArea / unionArea;
}

bool TrackingStreamProcessor::isOverlapping(const cv::Rect2d& rect1, const cv::Rect2d& rect2, float threshold) {
    return calculateIoU(rect1, rect2) > threshold;
}

void TrackingStreamProcessor::drawTrackingResults(cv::Mat& frame) {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    for (const auto& target : targets_) {
        if (target.isActive) {
            drawTarget(frame, target);
            drawTrajectory(frame, target);
        }
    }

    // 绘制信息
    std::string info = "Targets: " + std::to_string(targets_.size()) +
                       " Frame: " + std::to_string(frameCount_);
    cv::putText(frame, info, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX,
                0.7, cv::Scalar(255, 255, 255), 2);
}

void TrackingStreamProcessor::drawTarget(cv::Mat& frame, const TrackingTarget& target) {
    // 绘制边界框
    cv::rectangle(frame, target.bbox, target.color, 2);

    // 绘制标签
    std::string label = target.className + " ID:" + std::to_string(target.id) +
                        " (" + std::to_string(static_cast<int>(target.confidence * 100)) + "%)";

    int baseline = 0;
    cv::Size textSize = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseline);

    cv::Point labelPos(target.bbox.x, target.bbox.y - 10);
    cv::rectangle(frame,
                  cv::Point(labelPos.x, labelPos.y - textSize.height - baseline),
                  cv::Point(labelPos.x + textSize.width, labelPos.y + baseline),
                  target.color, -1);

    cv::putText(frame, label, labelPos, cv::FONT_HERSHEY_SIMPLEX, 0.5,
                cv::Scalar(255, 255, 255), 1);
}

void TrackingStreamProcessor::drawTrajectory(cv::Mat& frame, const TrackingTarget& target) {
    if (target.trajectory.size() < 2) return;

    for (size_t i = 1; i < target.trajectory.size(); ++i) {
        cv::line(frame, target.trajectory[i-1], target.trajectory[i], target.color, 2);
    }

    // 绘制当前位置点
    if (!target.trajectory.empty()) {
        cv::circle(frame, target.trajectory.back(), 3, target.color, -1);
    }
}

cv::Scalar TrackingStreamProcessor::generateColor(int id) {
    std::mt19937 rng(id);
    std::uniform_int_distribution<int> dist(0, 255);
    return cv::Scalar(dist(rng), dist(rng), dist(rng));
}

bool TrackingStreamProcessor::encodeAndSendFrame(const cv::Mat& frame, int64_t pts) {
    if (!encoderContext_ || !outputFormatContext_) {
        return false;
    }

    // 转换Mat到AVFrame
    AVFrame* avFrame = matToAVFrame(frame);
    if (!avFrame) {
        return false;
    }

    avFrame->pts = pts;

    // 发送帧到编码器
    int ret = avcodec_send_frame(encoderContext_, avFrame);
    if (ret < 0) {
        av_frame_free(&avFrame);
        return false;
    }

    // 接收编码包
    while (ret >= 0) {
        AVPacket* packet = av_packet_alloc();
        ret = avcodec_receive_packet(encoderContext_, packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&packet);
            break;
        } else if (ret < 0) {
            av_packet_free(&packet);
            av_frame_free(&avFrame);
            return false;
        }

        // 设置流索引和时间戳
        packet->stream_index = outputStream_->index;
        av_packet_rescale_ts(packet, encoderContext_->time_base, outputStream_->time_base);

        // 写入包
        ret = av_write_frame(outputFormatContext_, packet);
        av_packet_free(&packet);

        if (ret < 0) {
            av_frame_free(&avFrame);
            return false;
        }
    }

    av_frame_free(&avFrame);
    return true;
}

AVFrame* TrackingStreamProcessor::matToAVFrame(const cv::Mat& mat) {
    AVFrame* frame = av_frame_alloc();
    if (!frame) return nullptr;

    frame->format = AV_PIX_FMT_YUV420P;
    frame->width = mat.cols;
    frame->height = mat.rows;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return nullptr;
    }

    // 初始化转换上下文
    if (!swsContext_) {
        swsContext_ = sws_getContext(
                mat.cols, mat.rows, AV_PIX_FMT_BGR24,
                mat.cols, mat.rows, AV_PIX_FMT_YUV420P,
                SWS_BILINEAR, nullptr, nullptr, nullptr
        );
    }

    if (!swsContext_) {
        av_frame_free(&frame);
        return nullptr;
    }

    // 转换格式
    uint8_t* srcData[4] = { mat.data, nullptr, nullptr, nullptr };
    int srcLinesize[4] = { static_cast<int>(mat.step), 0, 0, 0 };

    sws_scale(swsContext_, srcData, srcLinesize, 0, mat.rows,
              frame->data, frame->linesize);

    return frame;
}

void TrackingStreamProcessor::logTrackingStatus() {
    std::lock_guard<std::mutex> lock(targetsMutex_);

    LOGGER_INFO("跟踪状态 - 帧数:" + std::to_string(frameCount_) +
                " 目标数:" + std::to_string(targets_.size()) +
                " 活跃目标:" + std::to_string(std::count_if(targets_.begin(), targets_.end(),
                                                            [](const TrackingTarget& t) { return t.isActive; })));
}

void TrackingStreamProcessor::notifyTrackingCallback() {
    std::lock_guard<std::mutex> lock(callbackMutex_);

    if (trackingCallback_) {
        std::lock_guard<std::mutex> targetsLock(targetsMutex_);
        trackingCallback_(targets_);
    }
}

void TrackingStreamProcessor::cleanup() {
    // 清理FFmpeg资源
    if (swsContext_) {
        sws_freeContext(swsContext_);
        swsContext_ = nullptr;
    }

    if (encoderContext_) {
        avcodec_free_context(&encoderContext_);
        encoderContext_ = nullptr;
    }

    if (outputFormatContext_) {
        if (outputFormatContext_->pb) {
            av_write_trailer(outputFormatContext_);
            avio_closep(&outputFormatContext_->pb);
        }
        avformat_free_context(outputFormatContext_);
        outputFormatContext_ = nullptr;
    }

    // 清理目标
    {
        std::lock_guard<std::mutex> lock(targetsMutex_);
        targets_.clear();
    }

    // 清理队列
    {
        std::lock_guard<std::mutex> lock(inputQueueMutex_);
        while (!inputFrameQueue_.empty()) {
            inputFrameQueue_.pop();
        }
    }

    {
        std::lock_guard<std::mutex> lock(outputQueueMutex_);
        while (!outputFrameQueue_.empty()) {
            outputFrameQueue_.pop();
        }
    }

    LOGGER_INFO("跟踪流处理器资源清理完成");
}

// TrackingStreamManager 实现

TrackingStreamManager::TrackingStreamManager() : nextStreamId_(1) {
    LOGGER_INFO("跟踪流管理器创建");
}

TrackingStreamManager::~TrackingStreamManager() {
    stopAll();
}

std::string TrackingStreamManager::createTrackingStream(const StreamConfig& inputConfig,
                                                        const StreamConfig& outputConfig,
                                                        const std::string& aiModelPath) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    std::string streamId = generateStreamId();

    auto processor = std::make_shared<TrackingStreamProcessor>(inputConfig, outputConfig);

    // 如果提供了AI模型路径，加载模型
    if (!aiModelPath.empty()) {
        try {
            auto aiModel = std::make_unique<rknn_lite>(const_cast<char*>(aiModelPath.c_str()), 0, 2, 0.5);
            processor->setAIModel(std::move(aiModel));
        } catch (const std::exception& e) {
            LOGGER_ERROR("加载AI模型失败: " + std::string(e.what()));
        }
    }

    trackingStreams_[streamId] = processor;

    LOGGER_INFO("创建跟踪流: " + streamId);
    return streamId;
}

bool TrackingStreamManager::startTrackingStream(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = trackingStreams_.find(streamId);
    if (it != trackingStreams_.end()) {
        return it->second->start();
    }

    return false;
}

bool TrackingStreamManager::stopTrackingStream(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = trackingStreams_.find(streamId);
    if (it != trackingStreams_.end()) {
        it->second->stop();
        trackingStreams_.erase(it);
        return true;
    }

    return false;
}

std::shared_ptr<TrackingStreamProcessor> TrackingStreamManager::getTrackingProcessor(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = trackingStreams_.find(streamId);
    if (it != trackingStreams_.end()) {
        return it->second;
    }

    return nullptr;
}

std::vector<std::string> TrackingStreamManager::listTrackingStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    std::vector<std::string> streamIds;
    for (const auto& pair : trackingStreams_) {
        streamIds.push_back(pair.first);
    }

    return streamIds;
}

void TrackingStreamManager::stopAll() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    for (auto& pair : trackingStreams_) {
        pair.second->stop();
    }

    trackingStreams_.clear();
    LOGGER_INFO("所有跟踪流已停止");
}

std::string TrackingStreamManager::generateStreamId() {
    return "tracking_stream_" + std::to_string(nextStreamId_++);
}