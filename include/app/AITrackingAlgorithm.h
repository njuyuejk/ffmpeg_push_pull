#ifndef AI_TRACKING_ALGORITHM_H
#define AI_TRACKING_ALGORITHM_H

#include "ffmpeg_base/StreamProcessor.h"
#include "AIService/rknnPool.h"
#include "logger/Logger.h"
#include "opencv2/opencv.hpp"
#include "opencv2/tracking.hpp"
#include "common/opencv2avframe.h"
#include "common/ThreadPool.h"
#include <memory>
#include <vector>
#include <map>
#include <chrono>

/**
 * @brief AI检测结果结构体（前向声明）
 */
struct Detection;

/**
 * @brief 跟踪目标的信息
 */
struct TrackedObject {
    int id;                                     // 目标唯一ID
    cv::Rect2d bbox;                           // 当前边界框
    cv::Ptr<cv::Tracker> tracker;             // OpenCV跟踪器
    int model_type;                            // 检测模型类型
    std::string label;                         // 目标标签
    float confidence;                          // 置信度
    std::chrono::steady_clock::time_point last_detected; // 上次检测时间
    std::chrono::steady_clock::time_point created_time;  // 创建时间
    int frames_since_detection;                // 自上次检测以来的帧数
    int consecutive_misses;                    // 连续丢失帧数
    bool is_active;                           // 是否活跃
    bool needs_redetection;                   // 是否需要重新检测

    TrackedObject() : id(-1), model_type(0), confidence(0.0f),
                      frames_since_detection(0), consecutive_misses(0),
                      is_active(true), needs_redetection(false) {}
};

struct TrackeModelPoolEntry {
    std::vector<std::unique_ptr<rknn_lite>> rkpool; // 模型集合
    std::unique_ptr<ThreadPool> pool_; // 线程池
    std::queue<std::future<bool>> futs; // 任务函数
    bool first_ok; // 模型第一次初始化标识
    int rknnFlag; // 模型数量
};


/**
 * @brief AI检测与跟踪融合算法
 * 
 * 此类实现了ITrackingAlgorithm接口，将AI检测结果与OpenCV跟踪器结合，
 * 提供稳定的多目标跟踪功能。
 */
class AITrackingAlgorithm : public ITrackingAlgorithm {
public:
    /**
     * @brief 构造函数
     * @param models_config 模型配置映射 (model_type -> model_path)
     * @param detection_interval 检测间隔帧数
     * @param tracker_type 跟踪器类型 ("KCF", "CSRT", "MIL")
     */
    AITrackingAlgorithm(const std::map<int, std::string>& models_config,
                        int detection_interval = 5,
                        const std::string& tracker_type = "CSRT");

    /**
     * @brief 析构函数
     */
    virtual ~AITrackingAlgorithm();

    // ITrackingAlgorithm接口实现
    bool initialize(int width, int height, int fps) override;
    AVFrame* processFrame(const AVFrame* frame, TrackingResult& result) override;
    void cleanup() override;
    std::string getName() const override;

    /**
     * @brief 设置检测置信度阈值
     * @param model_type 模型类型
     * @param threshold 置信度阈值
     */
    void setConfidenceThreshold(int model_type, float threshold);

    /**
     * @brief 设置检测间隔
     * @param interval 间隔帧数
     */
    void setDetectionInterval(int interval);

    /**
     * @brief 设置目标生存时间
     * @param frames 最大丢失帧数
     */
    void setMaxMissFrames(int frames);

    /**
     * @brief 获取当前活跃目标数量
     */
    int getActiveObjectCount() const;

    /**
     * @brief 获取统计信息
     */
    struct Statistics {
        int total_detections;        // 总检测次数
        int total_tracked_objects;   // 总跟踪目标数
        int active_objects;          // 当前活跃目标数
        int lost_objects;           // 丢失目标数
        double avg_tracking_time_ms; // 平均跟踪时间
        double avg_detection_time_ms; // 平均检测时间
    };

    Statistics getStatistics() const;

private:
    // 配置参数
    std::map<int, std::string> models_config_;    // 模型配置
    std::map<int, float> confidence_thresholds_;  // 置信度阈值
    int detection_interval_;                      // 检测间隔
    int max_miss_frames_;                        // 最大丢失帧数
    std::string tracker_type_;                   // 跟踪器类型

    // 运行时状态
    int frame_width_;
    int frame_height_;
    int fps_;
    bool initialized_;

    // AI检测相关
    std::map<int, std::unique_ptr<TrackeModelPoolEntry>> ai_models_;  // AI模型实例
    int frame_count_;                            // 当前帧计数
    bool should_detect_;                         // 是否应该执行检测

    // 跟踪相关
    std::vector<std::unique_ptr<TrackedObject>> tracked_objects_; // 跟踪目标列表
    int next_object_id_;                         // 下一个分配的目标ID
    cv::Mat current_frame_;                      // 当前帧（BGR格式）

    // 统计信息
    mutable std::mutex stats_mutex_;
    Statistics statistics_;
    std::chrono::steady_clock::time_point last_stats_update_;

    // 内部方法

    /**
     * @brief 初始化AI模型
     */
    bool initializeModels();

    /**
     * @brief 执行AI检测
     * @param frame 输入图像
     * @return 检测结果
     */
    std::vector<Detection> performDetection(const cv::Mat& frame);

    /**
     * @brief 更新现有跟踪器
     * @param frame 当前帧
     */
    void updateTrackers(const cv::Mat& frame);

    /**
     * @brief 将检测结果与现有跟踪目标关联
     * @param detections 检测结果
     */
    void associateDetections(const std::vector<Detection>& detections);

    /**
     * @brief 创建新的跟踪目标
     * @param detection 检测结果
     * @param frame 当前帧
     */
    void createNewTracker(const Detection& detection, const cv::Mat& frame);

    /**
     * @brief 移除丢失的跟踪目标
     */
    void removeLostTargets();

    /**
     * @brief 计算两个边界框的IoU
     * @param rect1 边界框1
     * @param rect2 边界框2
     * @return IoU值
     */
    double calculateIoU(const cv::Rect2d& rect1, const cv::Rect2d& rect2);

    /**
     * @brief 计算两个边界框的中心距离
     * @param rect1 边界框1
     * @param rect2 边界框2
     * @return 中心距离
     */
    double calculateCenterDistance(const cv::Rect2d& rect1, const cv::Rect2d& rect2);

    /**
     * @brief 创建OpenCV跟踪器
     * @return 跟踪器指针
     */
    cv::Ptr<cv::Tracker> createTracker();

    /**
     * @brief 更新统计信息
     */
    void updateStatistics(double detection_time_ms, double tracking_time_ms);

    /**
     * @brief 清理无效的跟踪器
     */
    void cleanupInvalidTrackers();

    /**
     * @brief 获取模型类型对应的标签
     * @param model_type 模型类型
     * @return 标签字符串
     */
    std::string getModelLabel(int model_type) const;

    /**
     * @brief 检查边界框是否有效
     * @param rect 边界框
     * @return 是否有效
     */
    bool isValidBoundingBox(const cv::Rect2d& rect) const;

    /**
     * @brief 裁剪边界框到图像范围内
     * @param rect 边界框
     * @return 裁剪后的边界框
     */
    cv::Rect2d clipBoundingBox(const cv::Rect2d& rect) const;
};

#endif // AI_TRACKING_ALGORITHM_H
