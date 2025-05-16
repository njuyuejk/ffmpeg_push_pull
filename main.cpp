#include "app/Application.h"
#include <iostream>
#include <csignal>
#include <stdexcept>
#include "logger/Logger.h"

// 全局信号处理函数
void globalSignalHandler(int signal) {
    std::cerr << "Received signal: " << signal << std::endl;

    try {
        Logger::warning("Received signal: " + std::to_string(signal) + ", preparing for safe exit");

        // 获取应用程序实例并请求停止
        Application& app = Application::getInstance();
        app.handleSignal(signal);

        // 如果是严重信号，等待一段时间后退出
        if (signal == SIGSEGV || signal == SIGABRT) {
            Logger::error("Received critical signal, forcing exit");
            std::cerr << "Critical error, forcing exit..." << std::endl;

            // 短暂延迟确保日志写入
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // 终止程序
            std::_Exit(128 + signal);
        }
    } catch (...) {
        std::cerr << "Error during signal handling" << std::endl;
        std::_Exit(128 + signal);
    }
}

/**
 * @brief 程序入口点
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return 程序返回代码
 */
int main(int argc, char* argv[]) {

//    std::cout << "===============================================\n";
//    std::cout << "         ███████╗ █████╗  █████╗ ██╗\n";
//    std::cout << "         ██╔════╝██╔══██╗██╔══██╗██║\n";
//    std::cout << "         ███████╗╚█████╔╝███████║██║\n";
//    std::cout << "         ╚════██║██╔══██╗██╔══██║██║\n";
//    std::cout << "         ███████║╚█████╔╝██║  ██║██║\n";
//    std::cout << "         ╚══════╝ ╚════╝ ╚═╝  ╚═╝╚═╝\n";
//    std::cout << "===============================================\n";
//    std::cout << "\n";

//    // 初始化日志系统
//    LogLevel logLevel = static_cast<LogLevel>(1);
//    Logger::init(true, "./logs", logLevel);

    Logger::info("Starting 58AI Program... \n"
                          "===============================================\n"
                          "         ███████╗ █████╗  █████╗ ██╗\n"
                          "         ██╔════╝██╔══██╗██╔══██╗██║\n"
                          "         ███████╗╚█████╔╝███████║██║\n"
                          "         ╚════██║██╔══██╗██╔══██║██║\n"
                          "         ███████║╚█████╔╝██║  ██║██║\n"
                          "         ╚══════╝ ╚════╝ ╚═╝  ╚═╝╚═╝\n"
                          "===============================================\n");

    // 设置信号处理器
    signal(SIGINT, globalSignalHandler);
    signal(SIGTERM, globalSignalHandler);
    signal(SIGSEGV, globalSignalHandler);  // 捕获段错误
    signal(SIGABRT, globalSignalHandler);  // 捕获终止信号

    try {
        // 获取应用程序实例
        Application& app = Application::getInstance();

        // 初始化应用程序
        if (!app.initialize("./config.json")) {
            return 1;
        }
        // 运行应用程序
        int result = app.run();

        std::cout << "program run result is: " << result << std::endl;

        // 清理资源
        app.cleanup();

        // 添加超时强制退出机制
        std::thread forceExitThread([]() {
            std::this_thread::sleep_for(std::chrono::seconds(5)); // 5秒超时
            std::cerr << "强制退出：程序清理超时" << std::endl;
            std::_Exit(1); // 强制终止进程
        });
        forceExitThread.detach(); // 分离线程使其独立运行

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;

        try {
            Logger::fatal("Program fatal error: " + std::string(e.what()));
            Logger::shutdown();
        } catch (...) {
            std::cerr << "Error logging fatal error" << std::endl;
        }

        return 1;
    } catch (...) {
        std::cerr << "Unknown fatal error" << std::endl;

        try {
            Logger::fatal("Program encountered unknown fatal error");
            Logger::shutdown();
        } catch (...) {
            std::cerr << "Error logging fatal error" << std::endl;
        }

        return 1;
    }
}