#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>
#include <string>
#include <map>
#include <mutex>

/**
 * @brief 看门狗类，用于监控系统组件并在故障时恢复
 */
class Watchdog {
public:
    /**
     * @brief 构造函数
     * @param checkIntervalMs 检查间隔（毫秒）
     */
    Watchdog(int checkIntervalMs = 5000);

    /**
     * @brief 析构函数
     */
    ~Watchdog();

    /**
     * @brief 启动看门狗
     */
    void start();

    /**
     * @brief 停止看门狗
     */
    void stop();

    /**
     * @brief 注册监控目标
     * @param targetName 目标名称
     * @param healthCheckFunc 健康检查函数，返回true表示健康
     * @param recoveryFunc 恢复函数
     * @return 注册是否成功
     */
    bool registerTarget(const std::string& targetName,
                        std::function<bool()> healthCheckFunc,
                        std::function<void()> recoveryFunc);

    /**
     * @brief 取消注册监控目标
     * @param targetName 目标名称
     * @return 取消注册是否成功
     */
    bool unregisterTarget(const std::string& targetName);

    /**
     * @brief 为目标喂食（标记为健康）
     * @param targetName 目标名称
     */
    void feedTarget(const std::string& targetName);

    /**
     * @brief 检查看门狗是否在运行
     * @return 是否在运行
     */
    bool isRunning() const;

private:
    /**
     * @brief 监控目标结构体
     */
    struct Target {
        std::string name;
        std::function<bool()> healthCheck;
        std::function<void()> recovery;
        std::chrono::time_point<std::chrono::steady_clock> lastFeedTime;
        int failCount;
        bool needsRecovery;
        std::chrono::time_point<std::chrono::steady_clock> lastRecoveryTime;
    };

    /**
     * @brief 监控线程的主循环
     */
    void monitorLoop();

    std::atomic<bool> running_;
    int checkIntervalMs_;
    std::thread monitorThread_;
    std::vector<Target> targets_;
    std::mutex targetMutex_;
};

#endif // WATCHDOG_H