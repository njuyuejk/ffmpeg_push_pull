//
// Created by YJK on 2025/6/25.
//

#include "app/AITrackingAlgorithm.h"
#include <algorithm>
#include <chrono>

// 构造函数
AITrackingAlgorithm::AITrackingAlgorithm(const std::map<int, std::string>& models_config,
                                         int detection_interval,
                                         const std::string& tracker_type)
        : models_config_(models_config)
        , detection_interval_(detection_interval)
        , tracker_type_(tracker_type)
        , frame_width_(0)
        , frame_height_(0)
        , fps_(25)
        , initialized_(false)
        , frame_count_(0)
        , should_detect_(true)
        , next_object_id_(1)
        , max_miss_frames_(30)
{
    // 设置默认置信度阈值
    confidence_thresholds_[1] = 0.5f; // 火灾烟雾
    confidence_thresholds_[2] = 0.5f; // 人员
    confidence_thresholds_[3] = 0.5f; // 无人机
    confidence_thresholds_[4] = 0.5f; // 车牌
    confidence_thresholds_[5] = 0.5f; // 仪表
    confidence_thresholds_[6] = 0.5f; // 水表
    confidence_thresholds_[7] = 0.5f; // 裂纹

    // 初始化统计信息
    statistics_ = {};
    last_stats_update_ = std::chrono::steady_clock::now();

    initializeColors();

    LOGGER_INFO("AITrackingAlgorithm created with " + std::to_string(models_config.size()) +
                " models, detection interval: " + std::to_string(detection_interval) +
                ", tracker type: " + tracker_type);
}

// 析构函数
AITrackingAlgorithm::~AITrackingAlgorithm() {
    cleanup();
}

// 初始化算法
bool AITrackingAlgorithm::initialize() {
    if (initialized_) {
        LOGGER_WARNING("AITrackingAlgorithm already initialized");
        return true;
    }

    // 初始化AI模型
    if (!initializeModels()) {
        LOGGER_ERROR("Failed to initialize AI models");
        return false;
    }

    // 重置统计信息
    std::lock_guard<std::mutex> lock(stats_mutex_);
    statistics_ = {};

    initialized_ = true;
    frame_count_ = 0;
    next_object_id_ = 1;

    LOGGER_INFO("AITrackingAlgorithm initialized successfully");
    return true;
}

// 处理帧
AVFrame* AITrackingAlgorithm::processFrame(const AVFrame* frame, TrackingResult& result) {
    if (!initialized_) {
        result.success = false;
        result.error_message = "Algorithm not initialized";
        return nullptr;
    }

    frame_height_ = frame->height;
    frame_width_ = frame->width;

    auto start_time = std::chrono::steady_clock::now();

    try {
        // 将AVFrame转换为OpenCV Mat
        current_frame_ = AVFrameToMat(frame);
        if (current_frame_.empty()) {
            result.success = false;
            result.error_message = "Failed to convert frame format";
            return nullptr;
        }

        frame_count_++;
        should_detect_ = (frame_count_ % detection_interval_ == 0);

        // 执行检测（如果需要）
        std::vector<Detection> detections;
        double detection_time_ms = 0.0;

        if (should_detect_) {
            auto detection_start = std::chrono::steady_clock::now();
            detections = performDetection(current_frame_);
            auto detection_end = std::chrono::steady_clock::now();
            detection_time_ms = std::chrono::duration<double, std::milli>(detection_end - detection_start).count();

            LOGGER_DEBUG("Frame " + std::to_string(frame_count_) + ": detected " +
                         std::to_string(detections.size()) + " objects in " +
                         std::to_string(detection_time_ms) + "ms");
        }

        // 更新跟踪器
        auto tracking_start = std::chrono::steady_clock::now();
        updateTrackers(current_frame_);

        // 关联检测结果（如果有新检测）
        if (should_detect_ && !detections.empty()) {
            associateDetections(detections);
        }

        // 移除丢失的目标
        removeLostTargets();

        auto tracking_end = std::chrono::steady_clock::now();
        double tracking_time_ms = std::chrono::duration<double, std::milli>(tracking_end - tracking_start).count();

        // 在输出图像上绘制跟踪结果
        drawTrackingResults(current_frame_, result);

        // 将处理后的cv::Mat转换为AVFrame

        std::unique_ptr<AVFrame> outputFrame = std::make_unique<AVFrame>();
        CvMatToAVFrame(current_frame_, outputFrame.get());

        // 准备结果
        result.success = true;
        result.objects.clear();
        result.object_ids.clear();

        for (const auto& tracked_obj : tracked_objects_) {
            if (tracked_obj && tracked_obj->is_active) {
                cv::Rect rect(static_cast<int>(tracked_obj->bbox.x),
                              static_cast<int>(tracked_obj->bbox.y),
                              static_cast<int>(tracked_obj->bbox.width),
                              static_cast<int>(tracked_obj->bbox.height));
                result.objects.push_back(rect);
                result.object_ids.push_back(tracked_obj->id);
            }
        }

        // 更新统计信息
        updateStatistics(detection_time_ms, tracking_time_ms);

        auto total_time = std::chrono::steady_clock::now();
        double total_time_ms = std::chrono::duration<double, std::milli>(total_time - start_time).count();

        LOGGER_DEBUG("Frame " + std::to_string(frame_count_) + " processed: " +
                     std::to_string(result.objects.size()) + " active objects, " +
                     std::to_string(total_time_ms) + "ms total");

        if (should_detect_) {
            return outputFrame.release();
        } else {
            return nullptr;
        }

    } catch (const std::exception& e) {
        result.success = false;
        result.error_message = "Processing error: " + std::string(e.what());
        LOGGER_ERROR("AITrackingAlgorithm processing error: " + std::string(e.what()));
        return nullptr;
    }
}

// 清理资源
void AITrackingAlgorithm::cleanup() {
    if (!initialized_) {
        return;
    }

    LOGGER_INFO("Cleaning up AITrackingAlgorithm");

    // 清理跟踪目标
    tracked_objects_.clear();

    // 清理AI模型
    ai_models_.clear();

    initialized_ = false;
    frame_count_ = 0;
    next_object_id_ = 1;

    LOGGER_INFO("AITrackingAlgorithm cleanup completed");
}

// 获取算法名称
std::string AITrackingAlgorithm::getName() const {
    return "AI Detection + " + tracker_type_ + " Tracking";
}

// 设置置信度阈值
void AITrackingAlgorithm::setConfidenceThreshold(int model_type, float threshold) {
    confidence_thresholds_[model_type] = threshold;
    LOGGER_INFO("Set confidence threshold for model type " + std::to_string(model_type) +
                " to " + std::to_string(threshold));
}

// 设置检测间隔
void AITrackingAlgorithm::setDetectionInterval(int interval) {
    detection_interval_ = std::max(1, interval);
    LOGGER_INFO("Set detection interval to " + std::to_string(detection_interval_) + " frames");
}

// 设置最大丢失帧数
void AITrackingAlgorithm::setMaxMissFrames(int frames) {
    max_miss_frames_ = std::max(1, frames);
    LOGGER_INFO("Set max miss frames to " + std::to_string(max_miss_frames_));
}

// 获取活跃目标数量
int AITrackingAlgorithm::getActiveObjectCount() const {
    int count = 0;
    for (const auto& obj : tracked_objects_) {
        if (obj && obj->is_active) {
            count++;
        }
    }
    return count;
}

// 获取统计信息
AITrackingAlgorithm::Statistics AITrackingAlgorithm::getStatistics() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    auto stats = statistics_;
    stats.active_objects = getActiveObjectCount();
    return stats;
}

// 初始化AI模型
bool AITrackingAlgorithm::initializeModels() {
    ai_models_.clear();

    for (const auto& config : models_config_) {
        int model_type = config.first;
        const std::string& model_path = config.second;

        try {
            float threshold = confidence_thresholds_.count(model_type) ?
                              confidence_thresholds_[model_type] : 0.5f;

            std::unique_ptr<TrackeModelPoolEntry> aiPool = std::make_unique<TrackeModelPoolEntry>();

            for (int i = 0; i < 6; ++i) {
                auto rknn_ptr = std::make_unique<rknn_lite>(const_cast<char*>(model_path.c_str()), i % 3, model_type, threshold);
                aiPool->rkpool.push_back(std::move(rknn_ptr));
            }

            aiPool->pool_ = std::make_unique<ThreadPool>(6);
            aiPool->first_ok = true;
            aiPool->rknnFlag = 0;

            ai_models_[model_type] = std::move(aiPool);

            LOGGER_INFO("Initialized AI model type " + std::to_string(model_type) +
                        " from " + model_path);

        } catch (const std::exception& e) {
            LOGGER_ERROR("Failed to initialize model type " + std::to_string(model_type) +
                         ": " + std::string(e.what()));
            return false;
        }
    }

    return !ai_models_.empty();
}

// 执行AI检测
std::vector<Detection> AITrackingAlgorithm::performDetection(const cv::Mat& frame) {
    std::vector<Detection> all_detections;

    for (auto& model_pair : ai_models_) {
        int model_type = model_pair.first;
        auto& model = model_pair.second;

        try {

            if (model->first_ok) {
                model->rkpool[model->rknnFlag]->ori_img = frame;
                model->futs.push(model->pool_->enqueue(&rknn_lite::interf, &(*model->rkpool[model->rknnFlag % 6])));
            } else {
                if (model->futs.front().get() != 0) {
                    LOGGER_ERROR("detect queue failed");
                } else {
                    model->futs.pop();
                    all_detections = model->rkpool[model->rknnFlag % 6]->detection_results;
                    model->rkpool[model->rknnFlag % 6]->ori_img = frame;
                    model->futs.push(model->pool_->enqueue(&rknn_lite::interf, &(*model->rkpool[model->rknnFlag % 6])));
                }
            }

            // 如果有检测结果且触发了警告（即检测到目标）
            if (!all_detections.empty()) {
                float threshold = confidence_thresholds_.count(model_type) ?
                                  confidence_thresholds_[model_type] : 0.5f;

                for (const auto& det : all_detections) {
                    if (det.confidence >= threshold) {

                        // 确保边界框在图像范围内
                        cv::Rect2d bbox = clipBoundingBox(det.bbox);

                        if (isValidBoundingBox(bbox)) {
                            std::string label = getModelLabel(model_type);
                            all_detections.emplace_back(bbox, det.confidence, model_type, label);
                        }
                    }
                }
            }

            model->rknnFlag++;
            if (model->rknnFlag >= 6) {
                if (model->first_ok) {
                    model->first_ok = false;
                }
                model->rknnFlag = 0;
            }

        } catch (const std::exception& e) {
            LOGGER_ERROR("Detection error for model type " + std::to_string(model_type) +
                         ": " + std::string(e.what()));
        }
    }

    return all_detections;
}

// 更新跟踪器
void AITrackingAlgorithm::updateTrackers(const cv::Mat& frame) {
    auto current_time = std::chrono::steady_clock::now();

    for (auto& tracked_obj : tracked_objects_) {
        if (!tracked_obj || !tracked_obj->is_active) {
            continue;
        }

        try {
            cv::Rect new_bbox;
            bool tracking_success = tracked_obj->tracker->update(frame, new_bbox);

            if (tracking_success && isValidBoundingBox(new_bbox)) {
                tracked_obj->bbox = clipBoundingBox(new_bbox);
                tracked_obj->frames_since_detection++;
                tracked_obj->consecutive_misses = 0;
            } else {
                // 跟踪失败
                tracked_obj->consecutive_misses++;
                tracked_obj->frames_since_detection++;

                LOGGER_DEBUG("Tracking failed for object " + std::to_string(tracked_obj->id) +
                             ", consecutive misses: " + std::to_string(tracked_obj->consecutive_misses));
            }

            // 检查是否需要重新检测
            if (tracked_obj->frames_since_detection > detection_interval_ * 2) {
                tracked_obj->needs_redetection = true;
            }

        } catch (const std::exception& e) {
            LOGGER_ERROR("Tracker update error for object " + std::to_string(tracked_obj->id) +
                         ": " + std::string(e.what()));
            tracked_obj->consecutive_misses++;
        }
    }
}

// 关联检测结果
void AITrackingAlgorithm::associateDetections(const std::vector<Detection>& detections) {
    std::vector<bool> detection_matched(detections.size(), false);
    std::vector<bool> tracker_matched(tracked_objects_.size(), false);

    // 使用IoU和距离进行关联
    const double iou_threshold = 0.3;
    const double distance_threshold = 100.0;

    for (size_t i = 0; i < tracked_objects_.size(); i++) {
        auto& tracked_obj = tracked_objects_[i];
        if (!tracked_obj || !tracked_obj->is_active) {
            continue;
        }

        double best_match_score = 0.0;
        int best_detection_idx = -1;

        for (size_t j = 0; j < detections.size(); j++) {
            if (detection_matched[j]) {
                continue;
            }

            const auto& detection = detections[j];

            // 只关联相同模型类型的检测
            if (detection.model_type != tracked_obj->model_type) {
                continue;
            }

            double iou = calculateIoU(tracked_obj->bbox, detection.bbox);
            double distance = calculateCenterDistance(tracked_obj->bbox, detection.bbox);

            // 组合得分：IoU权重更高
            double match_score = iou * 0.7 + (1.0 - std::min(distance / distance_threshold, 1.0)) * 0.3;

            if (match_score > best_match_score && iou > iou_threshold && distance < distance_threshold) {
                best_match_score = match_score;
                best_detection_idx = j;
            }
        }

        if (best_detection_idx >= 0) {
            // 更新跟踪目标
            const auto& detection = detections[best_detection_idx];
            tracked_obj->bbox = detection.bbox;
            tracked_obj->confidence = detection.confidence;
            tracked_obj->last_detected = std::chrono::steady_clock::now();
            tracked_obj->frames_since_detection = 0;
            tracked_obj->consecutive_misses = 0;
            tracked_obj->needs_redetection = false;

            // 重新初始化跟踪器以提高精度
            tracked_obj->tracker = createTracker();
            if (tracked_obj->tracker) {
                tracked_obj->tracker->init(current_frame_, tracked_obj->bbox);
            }

            detection_matched[best_detection_idx] = true;
            tracker_matched[i] = true;

            LOGGER_DEBUG("Associated detection with object " + std::to_string(tracked_obj->id) +
                         ", match score: " + std::to_string(best_match_score));
        }
    }

    // 为未匹配的检测创建新跟踪器
    for (size_t j = 0; j < detections.size(); j++) {
        if (!detection_matched[j]) {
            createNewTracker(detections[j], current_frame_);
        }
    }
}

// 创建新跟踪器
void AITrackingAlgorithm::createNewTracker(const Detection& detection, const cv::Mat& frame) {
    try {
        auto new_object = std::make_unique<TrackedObject>();
        new_object->id = next_object_id_++;
        new_object->bbox = detection.bbox;
        new_object->model_type = detection.model_type;
        new_object->label = detection.label;
        new_object->confidence = detection.confidence;
        new_object->last_detected = std::chrono::steady_clock::now();
        new_object->created_time = new_object->last_detected;
        new_object->is_active = true;

        // 创建跟踪器
        new_object->tracker = createTracker();
        if (!new_object->tracker) {
            LOGGER_ERROR("Failed to create tracker for new object");
            return;
        }

        try {
            new_object->tracker->init(frame, detection.bbox);
        } catch (const std::exception& e) {
            LOGGER_ERROR("Failed to initialize tracker for new object");
            return;
        }

        tracked_objects_.push_back(std::move(new_object));

        // 更新统计信息
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            statistics_.total_tracked_objects++;
        }

        LOGGER_INFO("Created new tracker for object " + std::to_string(next_object_id_ - 1) +
                    " (type: " + detection.label + ")");

    } catch (const std::exception& e) {
        LOGGER_ERROR("Error creating new tracker: " + std::string(e.what()));
    }
}

// 移除丢失的目标
void AITrackingAlgorithm::removeLostTargets() {
    auto it = tracked_objects_.begin();
    while (it != tracked_objects_.end()) {
        auto& tracked_obj = *it;

        if (!tracked_obj || tracked_obj->consecutive_misses >= max_miss_frames_) {
            if (tracked_obj) {
                LOGGER_INFO("Removing lost object " + std::to_string(tracked_obj->id) +
                            " after " + std::to_string(tracked_obj->consecutive_misses) + " misses");

                // 更新统计信息
                {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    statistics_.lost_objects++;
                }
            }

            it = tracked_objects_.erase(it);
        } else {
            ++it;
        }
    }
}

// 计算IoU
double AITrackingAlgorithm::calculateIoU(const cv::Rect2d& rect1, const cv::Rect2d& rect2) {
    double x1 = std::max(rect1.x, rect2.x);
    double y1 = std::max(rect1.y, rect2.y);
    double x2 = std::min(rect1.x + rect1.width, rect2.x + rect2.width);
    double y2 = std::min(rect1.y + rect1.height, rect2.y + rect2.height);

    if (x2 <= x1 || y2 <= y1) {
        return 0.0;
    }

    double intersection = (x2 - x1) * (y2 - y1);
    double area1 = rect1.width * rect1.height;
    double area2 = rect2.width * rect2.height;
    double union_area = area1 + area2 - intersection;

    return union_area > 0.0 ? intersection / union_area : 0.0;
}

// 计算中心距离
double AITrackingAlgorithm::calculateCenterDistance(const cv::Rect2d& rect1, const cv::Rect2d& rect2) {
    double cx1 = rect1.x + rect1.width / 2.0;
    double cy1 = rect1.y + rect1.height / 2.0;
    double cx2 = rect2.x + rect2.width / 2.0;
    double cy2 = rect2.y + rect2.height / 2.0;

    return std::sqrt((cx1 - cx2) * (cx1 - cx2) + (cy1 - cy2) * (cy1 - cy2));
}

// 创建OpenCV跟踪器
cv::Ptr<cv::Tracker> AITrackingAlgorithm::createTracker() {
    try {
        if (tracker_type_ == "CSRT") {
            return cv::TrackerCSRT::create();
        } else if (tracker_type_ == "KCF") {
            return cv::TrackerKCF::create();
        } else if (tracker_type_ == "MIL") {
            return cv::TrackerMIL::create();
        } else {
            LOGGER_WARNING("Unknown tracker type: " + tracker_type_ + ", using CSRT");
            return cv::TrackerCSRT::create();
        }
    } catch (const std::exception& e) {
        LOGGER_ERROR("Failed to create tracker: " + std::string(e.what()));
        return nullptr;
    }
}

// 更新统计信息
void AITrackingAlgorithm::updateStatistics(double detection_time_ms, double tracking_time_ms) {
    std::lock_guard<std::mutex> lock(stats_mutex_);

    if (detection_time_ms > 0.0) {
        statistics_.total_detections++;

        // 移动平均
        if (statistics_.total_detections == 1) {
            statistics_.avg_detection_time_ms = detection_time_ms;
        } else {
            double alpha = 0.1;
            statistics_.avg_detection_time_ms = alpha * detection_time_ms +
                                                (1.0 - alpha) * statistics_.avg_detection_time_ms;
        }
    }

    if (tracking_time_ms > 0.0) {
        // 更新跟踪时间统计
        if (statistics_.avg_tracking_time_ms == 0.0) {
            statistics_.avg_tracking_time_ms = tracking_time_ms;
        } else {
            double alpha = 0.1;
            statistics_.avg_tracking_time_ms = alpha * tracking_time_ms +
                                               (1.0 - alpha) * statistics_.avg_tracking_time_ms;
        }
    }
}

// 获取模型标签
std::string AITrackingAlgorithm::getModelLabel(int model_type) const {
    switch (model_type) {
        case 1: return "Fire/Smoke";
        case 2: return "Person";
        case 3: return "UAV";
        case 4: return "License Plate";
        case 5: return "Meter";
        case 6: return "Water Meter";
        case 7: return "Crack";
        default: return "Unknown";
    }
}

// 检查边界框是否有效
bool AITrackingAlgorithm::isValidBoundingBox(const cv::Rect2d& rect) const {
    return rect.width > 10.0 && rect.height > 10.0 &&
           rect.x >= 0 && rect.y >= 0 &&
           rect.x + rect.width <= frame_width_ &&
           rect.y + rect.height <= frame_height_;
}

// 裁剪边界框
cv::Rect2d AITrackingAlgorithm::clipBoundingBox(const cv::Rect2d& rect) const {
    cv::Rect2d clipped = rect;

    clipped.x = std::max(0.0, clipped.x);
    clipped.y = std::max(0.0, clipped.y);

    if (clipped.x + clipped.width > frame_width_) {
        clipped.width = frame_width_ - clipped.x;
    }

    if (clipped.y + clipped.height > frame_height_) {
        clipped.height = frame_height_ - clipped.y;
    }

    return clipped;
}

void AITrackingAlgorithm::drawTrackingResults(cv::Mat& frame, const TrackingResult& result) {
    if (!result.success || result.objects.empty()) {
        return;
    }

    for (size_t i = 0; i < result.objects.size(); i++) {
        const cv::Rect& bbox = result.objects[i];
        int objectId = (i < result.object_ids.size()) ? result.object_ids[i] : -1;

        // 获取颜色
        cv::Scalar color = getColorForId(objectId);

        // 绘制边界框
        cv::rectangle(frame, bbox, color, 2);

        // 绘制目标ID
        if (objectId >= 0) {
            std::string label = "ID: " + std::to_string(objectId);
            int baseline = 0;
            cv::Size textSize = cv::getTextSize(label, cv::FONT_HERSHEY_SIMPLEX, 0.6, 2, &baseline);

            // 背景矩形
            cv::Point textOrg(bbox.x, bbox.y - 10);
            if (textOrg.y < textSize.height) {
                textOrg.y = bbox.y + bbox.height + textSize.height + 10;
            }

            cv::rectangle(frame,
                          cv::Point(textOrg.x, textOrg.y - textSize.height - baseline),
                          cv::Point(textOrg.x + textSize.width, textOrg.y + baseline),
                          color, cv::FILLED);

            // 绘制文本
            cv::putText(frame, label, textOrg, cv::FONT_HERSHEY_SIMPLEX, 0.6,
                        cv::Scalar(255, 255, 255), 2);
        }

        // 绘制中心点
        cv::Point center(bbox.x + bbox.width / 2, bbox.y + bbox.height / 2);
        cv::circle(frame, center, 3, color, -1);
    }

    // 在左上角显示统计信息
    std::string info = "Tracking: " + std::to_string(result.objects.size()) + " objects";
    cv::putText(frame, info, cv::Point(10, 30), cv::FONT_HERSHEY_SIMPLEX, 0.7,
                cv::Scalar(0, 255, 0), 2);
}

cv::Scalar AITrackingAlgorithm::getColorForId(int id) {
    if (id < 0) {
        return cv::Scalar(128, 128, 128); // 灰色用于无效ID
    }

    return colors_[id % colors_.size()];
}

void AITrackingAlgorithm::initializeColors() {
    // 预定义一些颜色用于绘制不同的跟踪目标
    colors_ = {
            cv::Scalar(255, 0, 0),    // 红色
            cv::Scalar(0, 255, 0),    // 绿色
            cv::Scalar(0, 0, 255),    // 蓝色
            cv::Scalar(255, 255, 0),  // 青色
            cv::Scalar(255, 0, 255),  // 品红
            cv::Scalar(0, 255, 255),  // 黄色
            cv::Scalar(128, 0, 128),  // 紫色
            cv::Scalar(255, 165, 0),  // 橙色
            cv::Scalar(0, 128, 0),    // 深绿
            cv::Scalar(128, 128, 0)   // 橄榄色
    };
}