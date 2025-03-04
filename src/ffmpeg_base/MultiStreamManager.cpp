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
    stopAll();
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

    // 注册到看门狗
    if (watchdog_) {
        processor->registerWithWatchdog(watchdog_);
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

    // 获取流处理器并从映射中移除
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);

        auto it = streams_.find(streamId);
        if (it == streams_.end()) {
            Logger::warning("Stream not found: " + streamId);
            return false;
        }

        processor = it->second;
        streams_.erase(it);
    }

    // 在线程池中停止处理器
    try {
        // 使用 future 来跟踪任务完成情况，并添加超时机制
        auto future = threadPool_->enqueue([processor, streamId]() {
            try {
                processor->stop();
                Logger::info("Stopped stream: " + streamId);
                return true;
            } catch (const std::exception& e) {
                Logger::error("Error stopping stream " + streamId + ": " + std::string(e.what()));
                return false;
            }
        });

        // 等待任务完成，但最多等待10秒
        std::future_status status = future.wait_for(std::chrono::seconds(10));
        if (status == std::future_status::timeout) {
            Logger::warning("Timeout while stopping stream " + streamId + ", the stream may not be fully cleaned up");
            return false;
        }

        return future.get();
    } catch (const std::exception& e) {
        Logger::error("Exception in stopStream thread for " + streamId + ": " + std::string(e.what()));
        return false;
    }
}

void MultiStreamManager::stopAll() {
    std::vector<std::shared_ptr<StreamProcessor>> processorsToStop;
    std::vector<std::string> streamIds;

    // 收集所有流处理器并清空映射
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);

        for (const auto& pair : streams_) {
            processorsToStop.push_back(pair.second);
            streamIds.push_back(pair.first);
        }

        streams_.clear();
    }

    // 使用计数器跟踪停止失败的流数量
    int failureCount = 0;

    // 停止所有流处理器
    for (size_t i = 0; i < processorsToStop.size(); i++) {
        try {
            Logger::info("Stopping stream: " + streamIds[i]);
            processorsToStop[i]->stop();
        } catch (const std::exception& e) {
            failureCount++;
            Logger::error("Error stopping stream " + streamIds[i] + ": " + std::string(e.what()));
        }
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
        // 如果传入 null 指针，记录并直接返回
        if (!watchdog) {
            Logger::warning("Attempting to set null watchdog to MultiStreamManager");
            return;
        }

        watchdog_ = watchdog;
        Logger::info("Watchdog set for MultiStreamManager");

        // 注册现有的流到看门狗
        std::lock_guard<std::mutex> lock(streamsMutex_);
        int registeredCount = 0;

        for (const auto& pair : streams_) {
            try {
                pair.second->registerWithWatchdog(watchdog_);
                registeredCount++;
            } catch (const std::exception& e) {
                Logger::warning("Failed to register stream " + pair.first + " with watchdog: " + e.what());
            } catch (...) {
                Logger::warning("Unknown exception when registering stream " + pair.first + " with watchdog");
            }
        }

        Logger::info("Registered " + std::to_string(registeredCount) + " of " +
                     std::to_string(streams_.size()) + " streams with watchdog");
    } catch (const std::exception& e) {
        Logger::error("Exception in MultiStreamManager::setWatchdog: " + std::string(e.what()));
        watchdog_ = nullptr; // 重置指针，确保一致性
    } catch (...) {
        Logger::error("Unknown exception in MultiStreamManager::setWatchdog");
        watchdog_ = nullptr;
    }
}