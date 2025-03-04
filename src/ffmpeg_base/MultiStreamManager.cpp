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
    threadPool_->enqueue([processor, streamId]() {
        try {
            processor->stop();
            Logger::info("Stopped stream: " + streamId);
        } catch (const std::exception& e) {
            Logger::error("Error stopping stream " + streamId + ": " + std::string(e.what()));
        }
    });

    return true;
}

void MultiStreamManager::stopAll() {
    std::vector<std::shared_ptr<StreamProcessor>> processorsToStop;

    // 收集所有流处理器并清空映射
    {
        std::lock_guard<std::mutex> lock(streamsMutex_);

        for (const auto& pair : streams_) {
            processorsToStop.push_back(pair.second);
        }

        streams_.clear();
    }

    // 停止所有流处理器
    for (auto& processor : processorsToStop) {
        try {
            processor->stop();
        } catch (const std::exception& e) {
            Logger::error("Error stopping stream: " + std::string(e.what()));
        }
    }

    Logger::info("Stopped all streams");
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