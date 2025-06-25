#ifndef STREAM_PROCESSOR_H
#define STREAM_PROCESSOR_H

#include "common/StreamConfig.h"
#include "common/Watchdog.h"
#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <string>
#include <condition_variable>
#include <queue>
#include <future>
#include "AIService/rknnPool.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

/**
 * @brief 帧数据结构，用于在拉流和推流线程之间传递
 */
struct FrameData {
    AVFrame* frame;
    int streamIndex;
    int64_t pts;
    int64_t dts;
    AVRational timeBase;
    bool isVideo;

    FrameData() : frame(nullptr), streamIndex(-1), pts(0), dts(0), isVideo(true) {}
    ~FrameData() {
        if (frame) {
            av_frame_free(&frame);
        }
    }
};

/**
 * @brief 跟踪算法处理结果
 */
struct TrackingResult {
    bool success;                    // 处理是否成功
    std::vector<cv::Rect> objects;   // 检测到的目标框
    std::vector<int> object_ids;     // 目标ID（用于多目标跟踪）
    std::string error_message;       // 错误信息

    TrackingResult() : success(false) {}
};

/**
 * @brief 跟踪帧数据结构，用于在解码和跟踪线程之间传递
 */
struct TrackingFrameData {
    AVFrame* original_frame;         // 原始帧（用于回调）
    AVFrame* processing_frame;       // 处理帧（NV12格式，用于跟踪算法）
    int streamIndex;
    int64_t pts;
    int64_t dts;
    AVRational timeBase;
    int fps;

    TrackingFrameData() : original_frame(nullptr), processing_frame(nullptr),
                          streamIndex(-1), pts(0), dts(0), fps(25) {}
    ~TrackingFrameData() {
        if (original_frame) {
            av_frame_free(&original_frame);
        }
        if (processing_frame) {
            av_frame_free(&processing_frame);
        }
    }
};

/**
 * @brief 抽象跟踪算法接口
 */
class ITrackingAlgorithm {
public:
    virtual ~ITrackingAlgorithm() = default;

    /**
     * @brief 初始化跟踪算法
     * @param width 视频宽度
     * @param height 视频高度
     * @param fps 帧率
     * @return 是否初始化成功
     */
    virtual bool initialize(int width, int height, int fps) = 0;

    /**
     * @brief 处理单帧图像
     * @param frame 输入帧（NV12格式）
     * @param result 处理结果
     * @return 处理后的帧（如果为nullptr则使用原帧）
     */
    virtual AVFrame* processFrame(const AVFrame* frame, TrackingResult& result) = 0;

    /**
     * @brief 清理资源
     */
    virtual void cleanup() = 0;

    /**
     * @brief 获取算法名称
     */
    virtual std::string getName() const = 0;
};

/**
 * @brief 单个流处理类
 * 负责处理单个流的转码和推送
 */
class StreamProcessor {
public:
    // 帧处理回调函数类型定义
    using VideoFrameCallback = std::function<void(const AVFrame* frame, int64_t pts, int fps)>;
    using AudioFrameCallback = std::function<void(const AVFrame* frame, int64_t pts, int fps)>;

    using TrackingResultCallback = std::function<void(const TrackingResult& result, int64_t pts)>;

public:
    /**
     * @brief 构造函数
     * @param config 流配置
     */
    StreamProcessor(const StreamConfig& config);

    /**
     * @brief 析构函数
     */
    ~StreamProcessor();

    /**
     * @brief 启动流处理
     * @throw FFmpegException 启动失败时抛出异常
     */
    void start();

    /**
     * @brief 停止流处理
     */
    void stop();

    /**
     * @brief 查询流是否在运行
     * @return 是否正在运行
     */
    bool isRunning() const;

    /**
     * @brief 查询是否发生错误
     * @return 是否有错误
     */
    bool hasError() const;

    /**
     * @brief 获取流配置
     * @return 流配置
     */
    const StreamConfig& getConfig() const;

    /**
     * @brief 更新流配置（需要先停止流）
     * @param config 新配置
     * @return 更新是否成功
     */
    bool updateConfig(const StreamConfig& config);

    /**
     * @brief 设置视频帧回调函数
     * @param callback 回调函数
     */
    void setVideoFrameCallback(VideoFrameCallback callback);

    /**
     * @brief 设置音频帧回调函数
     * @param callback 回调函数
     */
    void setAudioFrameCallback(AudioFrameCallback callback);

    /**
     * @brief 设置跟踪结果回调函数
     * @param callback 回调函数
     */
    void setTrackingResultCallback(TrackingResultCallback callback);

    /**
     * @brief 设置跟踪算法
     * @param algorithm 跟踪算法实例
     */
    void setTrackingAlgorithm(std::shared_ptr<ITrackingAlgorithm> algorithm);

    /**
     * @brief 启用或禁用跟踪处理
     * @param enable 是否启用
     */
    void enableTracking(bool enable);

    /**
     * @brief 检查跟踪是否启用
     * @return 是否启用跟踪
     */
    bool isTrackingEnabled() const;

    /**
     * @brief 获取跟踪处理统计信息
     */
    struct TrackingStats {
        uint64_t total_frames;          // 总处理帧数
        uint64_t successful_frames;     // 成功处理帧数
        uint64_t failed_frames;         // 失败帧数
        double avg_processing_time_ms;  // 平均处理时间（毫秒）
        uint64_t queue_size;           // 当前队列大小
    };

    TrackingStats getTrackingStats() const;

private:
    // 流处理上下文结构体
    struct StreamContext {
        int streamIndex = -1;
        AVStream* inputStream = nullptr;
        AVStream* outputStream = nullptr;

        AVCodecContext* decoderContext = nullptr;
        AVCodecContext* encoderContext = nullptr;

        AVBufferRef* hwDeviceContext = nullptr;

        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        AVFrame* hwFrame = nullptr;  // 用于硬件解码/编码

        SwsContext* swsContext = nullptr;

        // 时间戳管理
        bool first_frame_processed = true;     // 是否为第一帧
        int64_t pts_offset = 0;                // PTS偏移量
        int64_t last_output_pts = AV_NOPTS_VALUE;  // 上一个输出PTS
    };

    // 成员变量
    StreamConfig config_;
    std::atomic<bool> isRunning_;
    std::atomic<bool> hasError_;
//    std::thread processingThread_;    // 拉流解码线程
//    std::thread pushThread_;          // 推流编码线程
    std::unique_ptr<std::thread> processingThread_;    // 拉流解码线程
    std::unique_ptr<std::thread> pushThread_;          // 推流编码线程
    std::unique_ptr<std::thread> trackingThread_;      // 跟踪处理线程

    // 帧队列相关
    std::queue<std::unique_ptr<FrameData>> frameQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCondVar_;
    static const size_t MAX_QUEUE_SIZE = 50;  // 最大队列大小
    std::atomic<bool> decodingFinished_;      // 解码是否完成

    // 跟踪处理队列相关
    std::queue<std::unique_ptr<TrackingFrameData>> trackingQueue_;
    std::mutex trackingQueueMutex_;
    std::condition_variable trackingQueueCondVar_;
    static const size_t MAX_TRACKING_QUEUE_SIZE = 10;  // 跟踪队列大小（较小以减少延迟）
    std::atomic<bool> trackingFinished_;      // 跟踪处理是否完成

    // 跟踪算法相关
    std::shared_ptr<ITrackingAlgorithm> trackingAlgorithm_;
    std::atomic<bool> trackingEnabled_;
    std::mutex trackingMutex_;

    // 跟踪统计
    mutable std::mutex trackingStatsMutex_;
    TrackingStats trackingStats_;
    std::chrono::high_resolution_clock::time_point lastStatsUpdate_;

    // 帧处理回调函数
    VideoFrameCallback videoFrameCallback_;
    AudioFrameCallback audioFrameCallback_;
    TrackingResultCallback trackingResultCallback_;
    std::mutex callbackMutex_; // 保护回调函数的互斥锁

    // FFmpeg上下文
    AVFormatContext* inputFormatContext_ = nullptr;
    AVFormatContext* outputFormatContext_ = nullptr;

    std::vector<StreamContext> videoStreams_;
    std::vector<StreamContext> audioStreams_;

    // 重连相关
    std::atomic<int> reconnectAttempts_;
    std::atomic<int> maxReconnectAttempts_;
    std::atomic<int64_t> lastFrameTime_;
    std::atomic<int64_t> noDataTimeout_;
    std::atomic<bool> isReconnecting_;

    // 添加这些到类成员变量
    std::mutex ffmpegMutex_;  // 保护FFmpeg上下文访问
    bool isContextValid_ = false;  // 用于跟踪上下文是否有效的标志

    // 私有方法
    void initialize();
    void openInput();
    void openOutput();
    AVBufferRef* createHardwareDevice();

    /**
     * @brief 根据硬件加速类型获取适合的编码器
     * @param hwType 硬件加速类型
     * @param codecId 编解码器ID
     * @return 编码器指针，如果未找到返回nullptr
     */
    const AVCodec* getEncoderByHardwareType(AVHWDeviceType hwType, AVCodecID codecId);

    /**
     * @brief 检查编解码器是否支持指定的像素格式
     * @param codec 编解码器
     * @param pixFmt 像素格式
     * @return 是否支持
     */
    bool isCodecSupported(const AVCodec* codec, AVPixelFormat pixFmt);

    void setupStreams();
    void setupVideoStream(AVStream* inputStream);
    void setupAudioStream(AVStream* inputStream);

    // 拉流解码循环
    void processLoop();

    // 推流编码循环
    void pushLoop();

    // 跟踪处理循环
    void trackingLoop();

    void processVideoPacket(AVPacket* packet, StreamContext& streamCtx);
    void processAudioPacket(AVPacket* packet, StreamContext& streamCtx);

    // 编码和推送帧
    void encodeAndPushFrame(std::unique_ptr<FrameData> frameData);
    void encodeVideoFrame(StreamContext& streamCtx, AVFrame* frame);
    void encodeAudioFrame(StreamContext& streamCtx, AVFrame* frame);

    // 跟踪处理相关方法
    void enqueueTrackingFrame(AVFrame* originalFrame, AVFrame* processingFrame,
                              int streamIndex, int64_t pts, int64_t dts,
                              AVRational timeBase, int fps);
    void processTrackingFrame(std::unique_ptr<TrackingFrameData> frameData);
    AVFrame* convertFrameForTracking(const AVFrame* srcFrame);
    void updateTrackingStats(bool success, double processingTimeMs);

    void cleanup();
    bool reconnect();
    void resetErrorState();
    bool isStreamStalled() const;

    void setupInputStreams();
    void setupInputVideoStream(AVStream* inputStream);
    void setupInputAudioStream(AVStream* inputStream);

    /**
     * @brief 处理视频帧
     * @param frame 解码后的视频帧
     * @param pts 时间戳
     */
    void handleVideoFrame(const AVFrame* frame, int64_t pts, int fps);

    /**
     * @brief 处理音频帧
     * @param frame 解码后的音频帧
     * @param pts 时间戳
     */
    void handleAudioFrame(const AVFrame* frame, int64_t pts, int fps);

    /**
     * @brief 将解码后的帧加入队列
     * @param frame 解码后的帧
     * @param streamCtx 流上下文
     * @param isVideo 是否为视频帧
     */
    void enqueueFrame(AVFrame* frame, StreamContext& streamCtx, bool isVideo);
};

#endif // STREAM_PROCESSOR_H