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

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

/**
 * @brief 单个流处理类
 * 负责处理单个流的转码和推送
 */
class StreamProcessor {
public:
    // 帧处理回调函数类型定义
    using VideoFrameCallback = std::function<void(const AVFrame* frame, int64_t pts, int fps)>;
    using AudioFrameCallback = std::function<void(const AVFrame* frame, int64_t pts, int fps)>;
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
    std::thread processingThread_;

    // 帧处理回调函数
    VideoFrameCallback videoFrameCallback_;
    AudioFrameCallback audioFrameCallback_;
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
    void processLoop();
    void processVideoPacket(AVPacket* packet, StreamContext& streamCtx);
    void processAudioPacket(AVPacket* packet, StreamContext& streamCtx);
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
};

#endif // STREAM_PROCESSOR_H