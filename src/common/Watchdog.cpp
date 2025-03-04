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
                              std::function<bool()> healthCheckFunc,
                              std::function<void()> recoveryFunc) {
    if (targetName.empty() || !healthCheckFunc || !recoveryFunc) {
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
    target.recovery = recoveryFunc;
    target.lastFeedTime = std::chrono::steady_clock::now();
    target.failCount = 0;
    target.needsRecovery = false;
    target.lastRecoveryTime = std::chrono::steady_clock::now() - std::chrono::minutes(10); // 设置上次恢复时间为10分钟前

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
        it->needsRecovery = false;
    }
}

bool Watchdog::isRunning() const {
    return running_;
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
        targets_.erase(it);
        Logger::info("Target unregistered from Watchdog: " + targetName);
        return true;
    }

    return false;
}

void Watchdog::monitorLoop() {
    const int MAX_CONSECUTIVE_FAILURES = 3;     // 连续失败次数阈值
    const int MIN_RECOVERY_INTERVAL_SEC = 60;   // 最小恢复间隔（秒）

    while (true) {
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            if (!running_) break;  // 检查是否应该退出循环

            for (auto& target : targets_) {
                if (!running_) break;  // 再次检查，以便快速响应停止请求

                try {
                    // 计算自上次喂食以来的时间（秒）
                    auto timeSinceLastFeed = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - target.lastFeedTime).count();

                    // 计算自上次恢复以来的时间（秒）
                    auto timeSinceLastRecovery = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - target.lastRecoveryTime).count();

                    // 检查目标是否需要恢复且上次恢复时间已过足够长
                    if (target.needsRecovery && timeSinceLastRecovery >= MIN_RECOVERY_INTERVAL_SEC) {
                        try {
                            Logger::warning("Watchdog attempting recovery for target: " + target.name);
                            target.recovery();
                            target.needsRecovery = false;
                            target.failCount = 0;
                            target.lastFeedTime = std::chrono::steady_clock::now();
                            target.lastRecoveryTime = std::chrono::steady_clock::now();
                            Logger::info("Watchdog recovery completed for target: " + target.name);
                        } catch (const std::exception& e) {
                            Logger::error("Watchdog recovery failed for target " + target.name + ": " + e.what());
                            // 仍然更新恢复时间，防止频繁重试
                            target.lastRecoveryTime = std::chrono::steady_clock::now();
                        } catch (...) {
                            Logger::error("Unknown exception in watchdog recovery for target: " + target.name);
                            target.lastRecoveryTime = std::chrono::steady_clock::now();
                        }
                        continue;
                    }

                    // 如果喂食超时，执行健康检查
                    if (timeSinceLastFeed > checkIntervalMs_ / 1000 * 2) {
                        // 执行健康检查
                        bool health = false;
                        try {
                            health = target.healthCheck();
                        } catch (const std::exception& e) {
                            Logger::warning("Watchdog health check exception for " + target.name + ": " + e.what());
                            health = false;
                        } catch (...) {
                            Logger::warning("Unknown exception in watchdog health check for " + target.name);
                            health = false;
                        }

                        if (!health) {
                            target.failCount++;
                            Logger::warning("Watchdog health check failed for " + target.name +
                                            " (failure count: " + std::to_string(target.failCount) +
                                            "/" + std::to_string(MAX_CONSECUTIVE_FAILURES) + ")");

                            // 判断是否需要恢复
                            if (target.failCount >= MAX_CONSECUTIVE_FAILURES) {
                                Logger::error("Watchdog: Target " + target.name + " requires recovery");
                                target.needsRecovery = true;
                            }
                        } else {
                            // 重置失败计数
                            if (target.failCount > 0) {
                                Logger::info("Watchdog health check recovered for " + target.name);
                                target.failCount = 0;
                            }

                            // 更新喂食时间
                            target.lastFeedTime = std::chrono::steady_clock::now();
                        }
                    }
                } catch (const std::exception& e) {
                    Logger::error("Exception in watchdog monitor loop for target " + target.name + ": " + e.what());
                } catch (...) {
                    Logger::error("Unknown exception in watchdog monitor loop");
                }
            }
        } // 释放锁

        // 检查是否应该退出
        {
            std::lock_guard<std::mutex> lock(targetMutex_);
            if (!running_) break;
        }

        // 睡眠一段时间
        try {
            std::this_thread::sleep_for(std::chrono::milliseconds(checkIntervalMs_));
        } catch (const std::exception& e) {
            Logger::error("Exception in watchdog sleep: " + std::string(e.what()));
            // 短暂休眠，避免CPU占用过高
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        } catch (...) {
            Logger::error("Unknown exception in watchdog sleep");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }

    Logger::debug("Watchdog monitor loop exited");
}