#include "common/Watchdog.h"
#include "logger/Logger.h"
#include <algorithm>

Watchdog::Watchdog(int checkIntervalMs)
        : running_(false), checkIntervalMs_(checkIntervalMs) {
}

Watchdog::~Watchdog() {
    try {
        stop();
    } catch (const std::exception& e) {
        // 在析构函数中不要让异常传播
        Logger::error("Exception in Watchdog destructor: " + std::string(e.what()));
    } catch (...) {
        Logger::error("Unknown exception in Watchdog destructor");
    }
}

void Watchdog::start() {
    std::lock_guard<std::mutex> lock(targetMutex_);

    if (running_) return;

    try {
        // 先创建线程，然后设置运行标志
        monitorThread_ = std::thread(&Watchdog::monitorLoop, this);
        running_ = true;
        Logger::info("Watchdog started with check interval: " + std::to_string(checkIntervalMs_) + "ms");
    } catch (const std::exception& e) {
        Logger::error("Failed to start watchdog: " + std::string(e.what()));
        // 不重新抛出异常，而是记录错误
    }
}

void Watchdog::stop() {
    bool wasRunning = false;

    {
        std::lock_guard<std::mutex> lock(targetMutex_);
        wasRunning = running_;
        running_ = false;
    }

    if (wasRunning && monitorThread_.joinable()) {
        try {
            monitorThread_.join();
            Logger::info("Watchdog stopped");
        } catch (const std::exception& e) {
            Logger::error("Exception while stopping watchdog: " + std::string(e.what()));
        }
    }
}

bool Watchdog::registerTarget(const std::string& targetName,
                              std::function<bool()> healthCheckFunc) {
    if (targetName.empty() || !healthCheckFunc) {
        Logger::warning("Invalid parameters for watchdog target registration");
        return false;
    }

    std::lock_guard<std::mutex> lock(targetMutex_);

    // 检查是否已存在同名目标
    auto it = std::find_if(targets_.begin(), targets_.end(),
                           [&targetName](const Target& target) {
                               return target.name == targetName;
                           });

    if (it != targets_.end()) {
        Logger::warning("Target already registered with Watchdog: " + targetName);
        return false;
    }

    // 创建新目标
    Target target;
    target.name = targetName;
    target.healthCheck = healthCheckFunc;
    target.lastFeedTime = std::chrono::steady_clock::now();
    target.failCount = 0;

    targets_.push_back(target);
    Logger::info("Target registered with Watchdog: " + targetName);
    return true;
}

void Watchdog::feedTarget(const std::string& targetName) {
    if (targetName.empty()) return;

    std::lock_guard<std::mutex> lock(targetMutex_);

    auto it = std::find_if(targets_.begin(), targets_.end(),
                           [&targetName](const Target& target) {
                               return target.name == targetName;
                           });

    if (it != targets_.end()) {
        it->lastFeedTime = std::chrono::steady_clock::now();
        if (it->failCount > 0) {
            it->failCount = 0;
            Logger::debug("Target '" + targetName + "' fed, reset failure count to 0");
        }
    }
}

bool Watchdog::isRunning() const {
    return running_;
}

// 获取目标状态
bool Watchdog::isTargetHealthy(const std::string& targetName) const {
    std::lock_guard<std::mutex> lock(targetMutex_);

    auto it = std::find_if(targets_.begin(), targets_.end(),
                           [&targetName](const Target& target) {
                               return target.name == targetName;
                           });

    // 如果目标不存在或者失败计数大于0，则认为是不健康的
    if (it == targets_.end()) {
        return false; // 目标不存在视为不健康
    }

    return it->failCount == 0;
}

// 获取目标失败计数
int Watchdog::getTargetFailCount(const std::string& targetName) const {
    std::lock_guard<std::mutex> lock(targetMutex_);

    auto it = std::find_if(targets_.begin(), targets_.end(),
                           [&targetName](const Target& target) {
                               return target.name == targetName;
                           });

    if (it == targets_.end()) {
        return -1; // 目标不存在
    }

    return it->failCount;
}

// 取消注册目标的方法 - 这对于防止悬空引用很重要
bool Watchdog::unregisterTarget(const std::string& targetName) {
    if (targetName.empty()) return false;

    std::lock_guard<std::mutex> lock(targetMutex_);

    auto it = std::find_if(targets_.begin(), targets_.end(),
                           [&targetName](const Target& target) {
                               return target.name == targetName;
                           });

    if (it != targets_.end()) {
        // 复制函数对象到局部变量，并设置为空，以确保即使在其他线程中也不会被调用
        it->healthCheck = []() { return true; };

        // 从列表中移除
        targets_.erase(it);
        Logger::info("Target unregistered from Watchdog: " + targetName);
        return true;
    }

    return false;
}

void Watchdog::monitorLoop() {
    const int MAX_CONSECUTIVE_FAILURES = 3; // 记录最大连续失败次数，用于日志

    while (true) {
        // 创建健康检查任务列表
        struct HealthCheckTask {
            std::string targetName;
            std::function<bool()> healthCheck;
        };

        std::vector<HealthCheckTask> healthCheckTasks;

        // 使用unique_lock，因为我们需要手动解锁
        std::unique_lock<std::mutex> lock(targetMutex_);

        if (!running_) break;

        // 首先收集所有需要检查的目标
        for (auto& target : targets_) {
            // 检查是否需要健康检查
            auto timeSinceLastFeed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - target.lastFeedTime).count();

            // 如果需要健康检查
            if (timeSinceLastFeed > checkIntervalMs_ / 1000 * 2) {
                HealthCheckTask task;
                task.targetName = target.name;
                task.healthCheck = target.healthCheck;
                healthCheckTasks.push_back(task);
            }
        }

        // 释放锁，这样在执行回调时不会阻塞其他线程
        lock.unlock();

        // 执行健康检查任务
        for (const auto& task : healthCheckTasks) {
            try {
                bool health = task.healthCheck();

                // 恢复锁，更新健康状态
                lock.lock();

                // 查找目标，可能在执行检查过程中已被移除
                auto it = std::find_if(targets_.begin(), targets_.end(),
                                       [&task](const Target& t) { return t.name == task.targetName; });

                if (it != targets_.end()) {
                    if (!health) {
                        it->failCount++;
                        Logger::warning("Watchdog health check failed for " + task.targetName +
                                        " (failure count: " + std::to_string(it->failCount) +
                                        "/" + std::to_string(MAX_CONSECUTIVE_FAILURES) + ")");
                    } else {
                        // 重置失败计数
                        if (it->failCount > 0) {
                            Logger::info("Watchdog health check recovered for " + task.targetName);
                            it->failCount = 0;
                        }

                        // 更新喂食时间
                        it->lastFeedTime = std::chrono::steady_clock::now();
                    }
                }

                lock.unlock();
            } catch (const std::exception& e) {
                Logger::error("Exception in watchdog health check for target " + task.targetName + ": " + e.what());
                lock.lock();

                auto it = std::find_if(targets_.begin(), targets_.end(),
                                       [&task](const Target& t) { return t.name == task.targetName; });

                if (it != targets_.end()) {
                    it->failCount++;
                }

                lock.unlock();
            } catch (...) {
                Logger::error("Unknown exception in watchdog health check for target: " + task.targetName);
            }
        }

        // 检查是否应该退出
        lock.lock();
        if (!running_) {
            lock.unlock();
            break;
        }
        lock.unlock();

        // 睡眠一段时间
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(checkIntervalMs_));
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }

    Logger::debug("Watchdog monitor loop exited");
}