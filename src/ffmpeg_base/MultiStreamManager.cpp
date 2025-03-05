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

    // 其他代码不变

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

    // 启动处理器
    try {
        processor->start();
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
    bool found = false;

    // 首先从映射中获取流
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        auto it = streams_.find(streamId);
        if (it != streams_.end()) {
            processor = it->second;
            found = true;
        }
    }

    if (!found) {
        Logger::warning("Stream not found: " + streamId);
        return false;
    }

    // 在做任何事情之前从看门狗注销
    {
        std::lock_guard<std::mutex> lock(watchdogMutex_);
        if (watchdog_) {
            try {
                // 使用try-catch块，确保注销失败不会影响后续操作
                watchdog_->unregisterTarget("stream_" + streamId);
                Logger::debug("Unregistered stream " + streamId + " from watchdog");
            } catch (const std::exception& e) {
                Logger::warning("Failed to unregister stream " + streamId + " from watchdog: " + e.what());
            }
        }
    }

    // 停止流处理
    bool stopResult = false;
    try {
        processor->stop();
        Logger::info("Stopped stream: " + streamId);
        stopResult = true;
    } catch (const std::exception& e) {
        Logger::error("Error stopping stream " + streamId + ": " + e.what());
        stopResult = false;
    }

    // 最后从映射中移除
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);
        streams_.erase(streamId);
    }

    return stopResult;
}

void MultiStreamManager::stopAll() {
    std::vector<std::shared_ptr<StreamProcessor>> processorsToStop;
    std::vector<std::string> streamIds;

    // 收集所有流处理器
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

    // 停止所有流处理器
    int failureCount = 0;
    for (size_t i = 0; i < processorsToStop.size(); i++) {
        try {
            Logger::info("Stopping stream: " + streamIds[i]);
            processorsToStop[i]->stop();
        } catch (const std::exception& e) {
            failureCount++;
            Logger::error("Error stopping stream " + streamIds[i] + ": " + std::string(e.what()));
        }
    }

    // 清空映射
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
                    } else {
                        // 流有问题但我们不再由看门狗自动处理 - 仅记录问题
                        Logger::debug("Stream " + streamId + " has issues: running=" +
                                      (processor->isRunning() ? "yes" : "no") +
                                      ", error=" + (processor->hasError() ? "yes" : "no"));
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