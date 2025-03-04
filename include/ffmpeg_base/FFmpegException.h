#ifndef FFMPEG_EXCEPTION_H
#define FFMPEG_EXCEPTION_H

#include <exception>
#include <string>

extern "C" {
#include <libavutil/error.h>
}

/**
 * @brief FFmpeg错误异常类
 * 封装FFmpeg的错误码和错误消息
 */
class FFmpegException : public std::exception {
public:
    /**
     * @brief 构造函数
     * @param message 错误消息
     * @param errorCode FFmpeg错误码
     */
    FFmpegException(const std::string& message, int errorCode = 0);

    /**
     * @brief 获取错误消息
     * @return 错误消息字符串
     */
    const char* what() const noexcept override;

    /**
     * @brief 获取FFmpeg错误码
     * @return 错误码
     */
    int getErrorCode() const;

private:
    std::string message_;
    std::string fullMessage_;
    int errorCode_;
};

#endif // FFMPEG_EXCEPTION_H