#ifndef TRACKING_STREAM_PROCESSOR_H
#define TRACKING_STREAM_PROCESSOR_H

#include "ffmpeg_base/StreamProcessor.h"
#include "common/StreamConfig.h"
#include "AIService/rknnPool.h"
#include "opencv2/opencv.hpp"
#include "opencv2/tracking.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

// 跟踪目标结构体
struct TrackingTarget {
    int id;                          // 目标ID
    cv::Rect2d bbox;                 // 边界框
    cv::Ptr<cv::Tracker> tracker;    // OpenCV跟踪器
    std::string className;           // 目标类别
    float confidence;                // 置信度
    int lostFrames;                  // 丢失帧数
    int64_t lastSeenTime;           // 最后检测到的时间
    bool isActive;                   // 是否活跃
    cv::Scalar color;               // 显示颜色
    std::vector<cv::Point2f> trajectory; // 轨迹点
};

// 检测结果结构体
struct DetectionResult {
    cv::Rect bbox;
    std::string className;
    float confidence;
};

// 帧数据结构体
struct FrameData {
    cv::Mat frame;
    int64_t pts;
    int fps;
    bool isValid;

    FrameData() : pts(0), fps(25), isValid(false) {}
    FrameData(const cv::Mat& f, int64_t p, int f_rate)
            : frame(f.clone()), pts(p), fps(f_rate), isValid(true) {}
};

/**
 * @brief 带跟踪算法的流处理器
 * 支持AI检测+多目标跟踪+推流功能
 */
class TrackingStreamProcessor {
public:
    /**
     * @brief 构造函数
     * @param inputConfig 输入流配置
     * @param outputConfig 输出流配置
     */
    TrackingStreamProcessor(const StreamConfig& inputConfig, const StreamConfig& outputConfig);

    /**
     * @brief 析构函数
     */
    ~TrackingStreamProcessor();

    /**
     * @brief 启动处理
     */
    bool start();

    /**
     * @brief 停止处理
     */
    void stop();

    /**
     * @brief 检查是否正在运行
     */
    bool isRunning() const { return running_; }

    /**
     * @brief 检查是否有错误
     */
    bool hasError() const { return hasError_; }

    /**
     * @brief 设置AI模型
     */
    void setAIModel(std::unique_ptr<rknn_lite> model);

    /**
     * @brief 设置跟踪器类型
     */
    void setTrackerType(const std::string& trackerType);

    /**
     * @brief 设置检测间隔（帧数）
     */
    void setDetectionInterval(int interval) { detectionInterval_ = interval; }

    /**
     * @brief 设置最大跟踪目标数量
     */
    void setMaxTargets(int maxTargets) { maxTargets_ = maxTargets; }

    /**
     * @brief 获取当前跟踪目标数量
     */
    int getCurrentTargetCount() const;

    /**
     * @brief 设置跟踪结果回调
     */
    void setTrackingCallback(std::function<void(const std::vector<TrackingTarget>&)> callback);

private:
    // 配置
    StreamConfig inputConfig_;
    StreamConfig outputConfig_;

    // 状态
    std::atomic<bool> running_;
    std::atomic<bool> hasError_;

    // 线程
    std::thread inputThread_;          // 输入线程
    std::thread trackingThread_;       // 跟踪线程
    std::thread outputThread_;        // 输出线程

    // 帧队列
    std::queue<FrameData> inputFrameQueue_;     // 输入帧队列
    std::queue<FrameData> outputFrameQueue_;    // 输出帧队列
    std::mutex inputQueueMutex_;
    std::mutex outputQueueMutex_;
    std::condition_variable inputQueueCond_;
    std::condition_variable outputQueueCond_;

    // AI模型
    std::unique_ptr<rknn_lite> aiModel_;

    // 跟踪器相关
    std::string trackerType_;                    // 跟踪器类型
    std::vector<TrackingTarget> targets_;        // 跟踪目标列表
    std::mutex targetsMutex_;
    int nextTargetId_;                          // 下一个目标ID
    int detectionInterval_;                     // 检测间隔
    int frameCount_;                           // 帧计数
    int maxTargets_;                           // 最大目标数量

    // 流处理器
    std::unique_ptr<StreamProcessor> inputProcessor_;   // 输入流处理器
    std::unique_ptr<StreamProcessor> outputProcessor_;  // 输出流处理器

    // FFmpeg编码相关
    AVFormatContext* outputFormatContext_;
    AVCodecContext* encoderContext_;
    AVStream* outputStream_;
    SwsContext* swsContext_;

    // 回调函数
    std::function<void(const std::vector<TrackingTarget>&)> trackingCallback_;
    std::mutex callbackMutex_;

    // 配置参数
    static const int MAX_QUEUE_SIZE = 10;      // 队列最大大小
    static const int MAX_LOST_FRAMES = 30;     // 最大丢失帧数
    static const int MIN_DETECTION_CONFIDENCE = 50; // 最小检测置信度(%)

    // 私有方法
    void inputThreadFunc();                    // 输入线程函数
    void trackingThreadFunc();                 // 跟踪线程函数
    void outputThreadFunc();                   // 输出线程函数

    bool initializeOutput();                   // 初始化输出
    void cleanup();                           // 清理资源

    // AI检测相关
    std::vector<DetectionResult> runAIDetection(const cv::Mat& frame);
    bool isValidDetection(const DetectionResult& detection);

    // 跟踪算法相关
    cv::Ptr<cv::Tracker> createTracker();
    void updateTrackers(cv::Mat& frame);
    void addNewTargets(const cv::Mat& frame, const std::vector<DetectionResult>& detections);
    void removeInactiveTargets();
    float calculateIoU(const cv::Rect2d& rect1, const cv::Rect2d& rect2);
    bool isOverlapping(const cv::Rect2d& rect1, const cv::Rect2d& rect2, float threshold = 0.3f);

    // 可视化相关
    void drawTrackingResults(cv::Mat& frame);
    void drawTarget(cv::Mat& frame, const TrackingTarget& target);
    void drawTrajectory(cv::Mat& frame, const TrackingTarget& target);
    cv::Scalar generateColor(int id);

    // 编码相关
    bool encodeAndSendFrame(const cv::Mat& frame, int64_t pts);
    AVFrame* matToAVFrame(const cv::Mat& mat);

    // 工具方法
    void logTrackingStatus();
    void notifyTrackingCallback();
};

/**
 * @brief 跟踪流管理器
 * 管理多个跟踪流处理器
 */
class TrackingStreamManager {
public:
    TrackingStreamManager();
    ~TrackingStreamManager();

    /**
     * @brief 创建跟踪流
     */
    std::string createTrackingStream(const StreamConfig& inputConfig,
                                     const StreamConfig& outputConfig,
                                     const std::string& aiModelPath = "");

    /**
     * @brief 启动跟踪流
     */
    bool startTrackingStream(const std::string& streamId);

    /**
     * @brief 停止跟踪流
     */
    bool stopTrackingStream(const std::string& streamId);

    /**
     * @brief 获取跟踪流处理器
     */
    std::shared_ptr<TrackingStreamProcessor> getTrackingProcessor(const std::string& streamId);

    /**
     * @brief 列出所有跟踪流
     */
    std::vector<std::string> listTrackingStreams();

    /**
     * @brief 停止所有跟踪流
     */
    void stopAll();

private:
    std::map<std::string, std::shared_ptr<TrackingStreamProcessor>> trackingStreams_;
    std::mutex streamsMutex_;
    int nextStreamId_;

    std::string generateStreamId();
};

#endif // TRACKING_STREAM_PROCESSOR_H