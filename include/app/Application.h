#ifndef APPLICATION_H
#define APPLICATION_H

#include "ffmpeg_base/MultiStreamManager.h"
#include "common/StreamConfig.h"
#include "common/Watchdog.h"
#include "mqtt/mqtt_sync_client.h"
#include "http/httplib.h"
#include <string>
#include <memory>

/**
 * @brief 应用程序类
 * 管理应用程序生命周期和流管理，完全基于配置文件
 */
class Application {
public:
    /**
     * @brief 获取应用程序实例（单例模式）
     * @return 应用程序实例的引用
     */
    static Application& getInstance();

    /**
     * @brief 禁止拷贝构造
     */
    Application(const Application&) = delete;

    /**
     * @brief 禁止赋值操作
     */
    Application& operator=(const Application&) = delete;

    /**
     * @brief 初始化应用程序
     * @param configFilePath 配置文件路径，默认为"config.ini"
     * @return 初始化是否成功
     */
    bool initialize(const std::string& configFilePath = "config.ini");

    /**
     * @brief 运行应用程序
     * @return 返回代码
     */
    int run();

    /**
     * @brief 处理信号
     * @param signal 信号值
     */
    void handleSignal(int signal);

    /**
     * @brief 清理应用程序资源
     */
    void cleanup();
private:
    /**
     * @brief 私有构造函数（单例模式）
     */
    Application();

    /**
     * @brief 列出所有流
     */
    void listStreams();

    /**
     * @brief 启动所有配置的流
     */
    void startAllStreams();

    /**
     * @brief 停止所有运行中的流
     */
    void stopAllStreams();

    /**
     * @brief 监控并管理流状态
     */
    void monitorStreams();

    /**
 * @brief 设置自定义帧处理回调
 */
    void setupCustomFrameProcessing();

private:
    // 成员变量
    bool running_;                           // 运行状态
    std::string configFilePath_;             // 配置文件路径
    std::unique_ptr<MultiStreamManager> streamManager_; // 流管理器
    int monitorIntervalSeconds_;             // 监控间隔（秒）
    bool autoRestartStreams_;                // 是否自动重启出错的流

    // 看门狗相关
    std::unique_ptr<Watchdog> watchdog_;     // 看门狗
    bool useWatchdog_;                       // 是否使用看门狗
    int watchdogIntervalSeconds_;            // 看门狗检查间隔（秒）

    int periodicReconnectInterval_; // 周期性重连间隔（秒）
    int64_t lastPeriodicReconnectTime_ = 0; // 上次周期性重连时间

    std::unique_ptr<httplib::Client> httpClient_;
};

/**
 * @brief 信号处理函数
 * @param signal 信号值
 */
void signalHandlerWrapper(int signal);

#endif // APPLICATION_H