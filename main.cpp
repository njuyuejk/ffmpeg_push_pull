#include "app/Application.h"
#include <iostream>

/**
 * @brief 程序入口点
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return 程序返回代码
 */
int main(int argc, char* argv[]) {
    try {
        // 获取应用程序实例
        Application& app = Application::getInstance();

        // 初始化应用程序（使用默认配置文件路径）
        if (!app.initialize("D:/project/C++/my/ffmpeg_push_pull/config.ini")) {
            return 1;
        }

        // 运行应用程序
        int result = app.run();

        // 清理资源
        app.cleanup();

        return result;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}