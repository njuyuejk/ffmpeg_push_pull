#include "ffmpeg_base/MultiStreamManager.h"
#include "logger/Logger.h"
#include <algorithm>
#include <random>
#include <sstream>
#include "ffmpeg_base/FFmpegException.h"

// 生成随机字符串作为ID的辅助函数
namespace {
    std::string generateRandomId(const std::string& prefix = "stream_") {
        static const char alphanum[] =
                "0123456789"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz";

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(alphanum) - 2);

        std::string id = prefix;
        for (int i = 0; i < 8; ++i) {
            id += alphanum[dis(gen)];
        }

        return id;
    }
}

MultiStreamManager::MultiStreamManager(int maxConcurrentStreams)
        : maxConcurrentStreams_(maxConcurrentStreams),
          threadPool_(std::make_unique<ThreadPool>(maxConcurrentStreams)) {

    Logger::info("Created MultiStreamManager with max " +
                 std::to_string(maxConcurrentStreams) + " concurrent streams");
}

MultiStreamManager::~MultiStreamManager() {
    try {
        // 首先停止看门狗喂食线程
        if (watchdogFeederRunning_) {
            watchdogFeederRunning_ = false;
            if (watchdogFeederThread_.joinable()) {
                watchdogFeederThread_.join();
            }
        }

        // 从看门狗注销所有目标
        if (watchdog_) {
            watchdog_->unregisterTarget("application");

            std::vector<std::string> streamIds;
            {
                std::lock_guard<std::mutex> lock(streamsMutex_);
                for (const auto& pair : streams_) {
                    streamIds.push_back(pair.first);
                }
            }

            for (const auto& streamId : streamIds) {
                try {
                    watchdog_->unregisterTarget("stream_" + streamId);
                } catch (...) {
                    // 忽略异常，仅在日志中记录
                    Logger::warning("Failed to unregister stream " + streamId + " from watchdog");
                }
            }

            // 清除看门狗指针
            watchdog_ = nullptr;
        }

        // 停止所有流
        stopAll();
    } catch (const std::exception& e) {
        Logger::error("Exception in MultiStreamManager destructor: " + std::string(e.what()));
    } catch (...) {
        Logger::error("Unknown exception in MultiStreamManager destructor");
    }
}

std::string MultiStreamManager::startStream(const StreamConfig& config) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    // 检查是否已达到最大流数量
    if (streams_.size() >= static_cast<size_t>(maxConcurrentStreams_)) {
        Logger::warning("Cannot start new stream, maximum concurrent streams reached: " +
                        std::to_string(maxConcurrentStreams_));
        throw FFmpegException("Maximum concurrent streams reached");
    }

    // 生成唯一ID（如果配置中没有提供）
    std::string streamId = config.id;
    if (streamId.empty()) {
        streamId = generateStreamId();
    } else {
        // 检查ID是否已存在
        if (streams_.find(streamId) != streams_.end()) {
            Logger::warning("Stream ID already exists: " + streamId);
            throw FFmpegException("Stream ID already exists: " + streamId);
        }
    }

    // 创建流处理器
    StreamConfig streamConfig = config;
    streamConfig.id = streamId;
    auto processor = std::make_shared<StreamProcessor>(streamConfig);

    // 注册到看门狗 - 使用安全的回调方式
    if (watchdog_) {
        // 创建一个弱引用用于看门狗回调
        std::weak_ptr<StreamProcessor> weakPtr = processor;

        watchdog_->registerTarget("stream_" + streamId,
                // 健康检查函数
                                  [weakPtr]() -> bool {
                                      // 尝试获取强引用
                                      if (auto sp = weakPtr.lock()) {
                                          return sp->isRunning() && !sp->hasError();
                                      }
                                      // 对象已被释放，返回true让看门狗停止监控
                                      return true;
                                  }
        );
    }

    // 将流处理器添加到映射中
    streams_[streamId] = processor;

    // 使用线程池启动处理器
    try {
        // 由于启动操作可能需要一定时间，我们使用线程池进行异步处理
        // 但我们需要等待启动完成再返回结果
        auto startFuture = threadPool_->enqueue([processor]() {
            processor->start();
            return true;
        });

        // 等待启动完成，最多10秒
        auto status = startFuture.wait_for(std::chrono::seconds(10));
        if (status != std::future_status::ready) {
            Logger::warning("Timeout waiting for stream to start: " + streamId);
            // 即使超时，我们也保留处理器，它可能仍在启动中
        }

        Logger::info("Started stream: " + streamId);
    } catch (const FFmpegException& e) {
        // 启动失败，从映射中移除
        streams_.erase(streamId);
        Logger::error("Failed to start stream " + streamId + ": " + std::string(e.what()));
        throw;
    }

    return streamId;
}

bool MultiStreamManager::stopStream(const std::string& streamId) {
    std::shared_ptr<StreamProcessor> processor;

    // 首先查找流，但不要立即从映射中删除
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find(streamId);
        if (it == streams_.end()) {
            Logger::warning("Stream not found to stop: " + streamId);
            return false;
        }
        processor = it->second;
    }

    // 首先从看门狗注销
    if (watchdog_) {
        try {
            watchdog_->unregisterTarget("stream_" + streamId);
            Logger::debug("Unregistered stream " + streamId + " from watchdog");
        } catch (const std::exception& e) {
            Logger::warning("Error unregistering from watchdog: " + std::string(e.what()));
        }
    }

    bool stopSuccess = false;

    // 停止处理器
    try {
        // 定义合理的超时时间
        const int STOP_TIMEOUT_SEC = 10;

        // 使用线程池停止处理器，带有超时
        auto stopFuture = threadPool_->enqueue([processor, streamId]() {
            try {
                processor->stop();
                Logger::info("Stopped stream: " + streamId);
                return true;
            } catch (const std::exception& e) {
                Logger::error("Error stopping stream " + streamId + ": " + e.what());
                return false;
            }
        });

        // 带超时等待
        auto status = stopFuture.wait_for(std::chrono::seconds(STOP_TIMEOUT_SEC));
        if (status == std::future_status::ready) {
            stopSuccess = stopFuture.get();
        } else {
            Logger::warning("Timeout waiting for stream " + streamId + " to stop");
            stopSuccess = false;
        }
    } catch (const std::exception& e) {
        Logger::error("Exception stopping stream " + streamId + ": " + std::string(e.what()));
        stopSuccess = false;
    }

    // 停止后从映射中删除
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streams_.erase(streamId);
    }

    return stopSuccess;
}

void MultiStreamManager::stopAll() {
    std::vector<std::shared_ptr<StreamProcessor>> processorsToStop;
    std::vector<std::string> streamIds;

    // 收集所有处理器
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        for (const auto& pair : streams_) {
            processorsToStop.push_back(pair.second);
            streamIds.push_back(pair.first);
        }
    }

    // 首先从看门狗注销所有流
    if (watchdog_) {
        for (const auto& streamId : streamIds) {
            try {
                watchdog_->unregisterTarget("stream_" + streamId);
                Logger::debug("Unregistered stream " + streamId + " from watchdog during stopAll");
            } catch (const std::exception& e) {
                Logger::warning("Error unregistering stream from watchdog: " + std::string(e.what()));
            }
        }
    }

    // 使用线程池并行停止所有处理器
    std::vector<std::future<bool>> stopFutures;
    stopFutures.reserve(processorsToStop.size());

    for (size_t i = 0; i < processorsToStop.size(); i++) {
        auto processor = processorsToStop[i];
        auto streamId = streamIds[i];

        stopFutures.push_back(threadPool_->enqueue([processor, streamId]() -> bool {
            try {
                Logger::info("Stopping stream: " + streamId);
                processor->stop();
                return true;
            } catch (const std::exception& e) {
                Logger::error("Error stopping stream " + streamId + ": " + std::string(e.what()));
                return false;
            }
        }));
    }

    // 等待所有停止操作完成（带超时）
    int successCount = 0;
    int failureCount = 0;

    auto stopStartTime = std::chrono::steady_clock::now();
    auto stopEndTime = stopStartTime + std::chrono::seconds(15);  // 15秒全局超时

    for (size_t i = 0; i < stopFutures.size(); i++) {
        auto currentTime = std::chrono::steady_clock::now();
        if (currentTime >= stopEndTime) {
            // 已达到全局超时
            Logger::warning("Global timeout reached while stopping streams");
            failureCount += stopFutures.size() - i;
            break;
        }

        // 计算此future的剩余超时时间
        auto remainingTime = stopEndTime - currentTime;

        // 使用剩余超时时间等待此future
        auto status = stopFutures[i].wait_for(remainingTime);
        if (status != std::future_status::ready) {
            Logger::warning("Timeout stopping stream " + streamIds[i]);
            failureCount++;
            continue;
        }

        // 获取结果
        try {
            bool result = stopFutures[i].get();
            if (result) {
                successCount++;
            } else {
                failureCount++;
            }
        } catch (const std::exception& e) {
            Logger::error("Exception getting stop result: " + std::string(e.what()));
            failureCount++;
        }
    }

    // 现在清空流映射
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streams_.clear();
    }

    if (failureCount > 0) {
        Logger::warning("Failed to cleanly stop " + std::to_string(failureCount) + " out of " +
                        std::to_string(processorsToStop.size()) + " streams");
    } else {
        Logger::info("Stopped all streams successfully");
    }
}

std::vector<std::string> MultiStreamManager::listStreams() {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    std::vector<std::string> result;
    result.reserve(streams_.size());

    for (const auto& pair : streams_) {
        result.push_back(pair.first);
    }

    return result;
}

bool MultiStreamManager::isStreamRunning(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        return false;
    }

    return it->second->isRunning();
}

bool MultiStreamManager::hasStreamError(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        return false;
    }

    return it->second->hasError();
}

StreamConfig MultiStreamManager::getStreamConfig(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        // 返回默认配置
        StreamConfig config = StreamConfig::createDefault();
        config.id = streamId;
        return config;
    }

    return it->second->getConfig();
}

bool MultiStreamManager::updateStreamConfig(const std::string& streamId, const StreamConfig& config) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(streamId);
    if (it == streams_.end()) {
        Logger::warning("Stream not found for config update: " + streamId);
        return false;
    }

    // 检查流是否正在运行
    if (it->second->isRunning()) {
        Logger::warning("Cannot update config while stream is running: " + streamId);
        return false;
    }

    // 更新配置
    StreamConfig updatedConfig = config;
    updatedConfig.id = streamId; // 确保ID不变

    return it->second->updateConfig(updatedConfig);
}

std::string MultiStreamManager::generateStreamId() {
    static int counter = 0;
    std::string id;

    do {
        id = "stream_" + std::to_string(counter++);
    } while (streams_.find(id) != streams_.end());

    return id;
}

// 添加 setWatchdog 方法
void MultiStreamManager::setWatchdog(Watchdog* watchdog) {
    try {
        std::lock_guard<std::mutex> lock(watchdogMutex_);

        // 如果传入null指针，记录并直接返回
        if (!watchdog) {
            Logger::warning("Attempting to set null watchdog to MultiStreamManager");
            return;
        }

        // 保存看门狗指针
        watchdog_ = watchdog;

        // 注册应用程序看门狗目标 - 简化版本，只监控状态
        watchdog_->registerTarget("application", []() -> bool {
            return true; // 简单的健康检查，总是返回健康
        });

        // 为所有现有流注册看门狗目标 - 简化版本，只监控状态
        {
            std::lock_guard<std::mutex> streamLock(streamsMutex_);

            for (const auto& pair : streams_) {
                const std::string& streamId = pair.first;
                std::weak_ptr<StreamProcessor> weakProcessor = pair.second;

                watchdog_->registerTarget("stream_" + streamId,
                                          [weakProcessor]() -> bool {
                                              if (auto processor = weakProcessor.lock()) {
                                                  return processor->isRunning() && !processor->hasError();
                                              }
                                              return true; // 对象已被释放，返回true表示不需要监控
                                          }
                );
            }
        }

        // 启动看门狗喂食线程
        if (!watchdogFeederRunning_) {
            watchdogFeederRunning_ = true;
            watchdogFeederThread_ = std::thread(&MultiStreamManager::watchdogFeederLoop, this);
            Logger::info("Started watchdog feeder thread");
        }

        Logger::info("Watchdog set for MultiStreamManager");
    } catch (const std::exception& e) {
        Logger::error("Exception in MultiStreamManager::setWatchdog: " + std::string(e.what()));
        watchdog_ = nullptr;
    } catch (...) {
        Logger::error("Unknown exception in MultiStreamManager::setWatchdog");
        watchdog_ = nullptr;
    }
}

void MultiStreamManager::watchdogFeederLoop() {
    const int FEED_INTERVAL_MS = 3000; // 每3秒喂食一次
    const int MAX_FAILURE_COUNT_BEFORE_RECONNECT = 3; // 连续3次失败后尝试重连

    // 记录失败计数的映射
    std::map<std::string, int> streamFailureCounts;

    Logger::info("Watchdog feeder thread started");

    while (watchdogFeederRunning_) {
        try {
            // 获取当前所有流的快照
            std::vector<std::string> streamIds;
            std::vector<std::weak_ptr<StreamProcessor>> weakProcessors;

            {
                std::lock_guard<std::mutex> lock(streamsMutex_);
                for (const auto& pair : streams_) {
                    streamIds.push_back(pair.first);
                    weakProcessors.push_back(pair.second);
                }
            }

            // 检查每个流的状态并喂食看门狗
            for (size_t i = 0; i < streamIds.size(); i++) {
                const std::string& streamId = streamIds[i];

                // 尝试获取强引用
                if (auto processor = weakProcessors[i].lock()) {
                    // 只有当流正在运行且没有错误时才喂食
                    if (processor->isRunning() && !processor->hasError()) {
                        if (watchdog_) {
                            watchdog_->feedTarget("stream_" + streamId);
                        }

                        // 重置此流的失败计数
                        streamFailureCounts[streamId] = 0;
                    } else {
                        // 增加失败计数
                        if (streamFailureCounts.find(streamId) == streamFailureCounts.end()) {
                            streamFailureCounts[streamId] = 0;
                        }
                        streamFailureCounts[streamId]++;

                        // 如果连续失败次数达到阈值，尝试重连
                        if (streamFailureCounts[streamId] >= MAX_FAILURE_COUNT_BEFORE_RECONNECT) {
                            Logger::warning("Stream " + streamId + " failed " +
                                            std::to_string(streamFailureCounts[streamId]) +
                                            " times, attempting auto-reconnect");

                            // 使用异步方式重连流
                            asyncReconnectStream(streamId);

                            // 重置失败计数
                            streamFailureCounts[streamId] = 0;
                        } else {
                            // 流有问题但尚未达到重连阈值，记录问题
                            Logger::debug("Stream " + streamId + " has issues: running=" +
                                          (processor->isRunning() ? "yes" : "no") +
                                          ", error=" + (processor->hasError() ? "yes" : "no") +
                                          ", failure count=" + std::to_string(streamFailureCounts[streamId]));
                        }
                    }
                }
            }

            // 喂食应用程序看门狗
            if (watchdog_) {
                watchdog_->feedTarget("application");
            }

        } catch (const std::exception& e) {
            Logger::error("Exception in watchdog feeder: " + std::string(e.what()));
        } catch (...) {
            Logger::error("Unknown exception in watchdog feeder");
        }

        // 睡眠一段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(FEED_INTERVAL_MS));
    }

    Logger::info("Watchdog feeder thread exited");
}

void MultiStreamManager::reconnectStream(const std::string& streamId) {
    std::shared_ptr<StreamProcessor> processor;

    // 获取处理器
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find(streamId);
        if (it == streams_.end()) {
            Logger::warning("Stream not found for reconnect: " + streamId);
            return;
        }
        processor = it->second;
    }

    StreamConfig config = processor->getConfig();

    // 停止处理器
    bool stopSuccess = stopStream(streamId);
    if (!stopSuccess) {
        Logger::warning("Failed to stop stream for reconnect: " + streamId);
    }

    // 短暂暂停后重连
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 使用相同配置启动新的处理器
    try {
        startStream(config);
        Logger::info("Successfully reconnected stream: " + streamId);
    } catch (const FFmpegException& e) {
        Logger::error("Failed to reconnect stream " + streamId + ": " + std::string(e.what()));
    }
}

void MultiStreamManager::asyncReconnectStream(const std::string& streamId) {
    // 提交重连任务到线程池
    threadPool_->enqueue([this, streamId]() {
        this->reconnectStream(streamId);
    });
}

std::shared_ptr<StreamProcessor> MultiStreamManager::getStreamProcessor(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(streamsMutex_);

    auto it = streams_.find(streamId);
    if (it != streams_.end()) {
        return it->second;
    }

    return nullptr;
}