#ifndef MULTI_STREAM_MANAGER_H
#define MULTI_STREAM_MANAGER_H

#include "StreamProcessor.h"
#include "common/ThreadPool.h"
#include "common/Watchdog.h"
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <future>   // 添加此头文件以支持std::future

/**
 * @brief 多路流管理器类
 * 用于管理多个流处理实例
 */
class MultiStreamManager {
public:
    /**
     * @brief 构造函数
     * @param maxConcurrentStreams 最大并发流数量
     */
    MultiStreamManager(int maxConcurrentStreams = 4);

    /**
     * @brief 析构函数
     */
    ~MultiStreamManager();

    /**
     * @brief 启动一个流
     * @param config 流配置
     * @return 流ID
     */
    std::string startStream(const StreamConfig& config);

    /**
     * @brief 停止一个流
     * @param streamId 流ID
     * @return 操作是否成功
     */
    bool stopStream(const std::string& streamId);

    /**
     * @brief 停止所有流
     */
    void stopAll();

    /**
     * @brief 获取所有流ID列表
     * @return 流ID列表
     */
    std::vector<std::string> listStreams();

    /**
     * @brief 检查流是否在运行
     * @param streamId 流ID
     * @return 是否在运行
     */
    bool isStreamRunning(const std::string& streamId);

    /**
     * @brief 检查流是否有错误
     * @param streamId 流ID
     * @return 是否有错误
     */
    bool hasStreamError(const std::string& streamId);

    /**
     * @brief 获取流配置
     * @param streamId 流ID
     * @return 流配置
     */
    StreamConfig getStreamConfig(const std::string& streamId);

    /**
     * @brief 更新流配置
     * @param streamId 流ID
     * @param config 新的流配置
     * @return 操作是否成功
     */
    bool updateStreamConfig(const std::string& streamId, const StreamConfig& config);

    /**
     * @brief 设置看门狗
     * @param watchdog 看门狗指针
     */
    void setWatchdog(Watchdog* watchdog);

    /**
     * @brief 重连流
     * @param streamId 流ID
     */
    void reconnectStream(const std::string& streamId);

    /**
     * @brief 异步重连流（使用线程池）
     * @param streamId 流ID
     */
    void asyncReconnectStream(const std::string& streamId);

private:
    int maxConcurrentStreams_;
    std::unique_ptr<ThreadPool> threadPool_;
    std::mutex streamsMutex_;
    std::map<std::string, std::shared_ptr<StreamProcessor>> streams_;
    Watchdog* watchdog_ = nullptr;  // 看门狗指针

    std::string generateStreamId();

    // 用于看门狗的互斥锁
    std::mutex watchdogMutex_;

    // 看门狗喂食线程和运行标志
    std::thread watchdogFeederThread_;
    std::atomic<bool> watchdogFeederRunning_{false};

    // 看门狗喂食循环
    void watchdogFeederLoop();
};

#endif // MULTI_STREAM_MANAGER_H