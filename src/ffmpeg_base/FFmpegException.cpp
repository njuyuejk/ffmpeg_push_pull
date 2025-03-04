#include "ffmpeg_base/FFmpegException.h"

FFmpegException::FFmpegException(const std::string& message, int errorCode)
        : message_(message), errorCode_(errorCode) {

    char errorStr[AV_ERROR_MAX_STRING_SIZE] = {0};
    if (errorCode != 0) {
        // 获取FFmpeg错误码的字符串描述
        av_strerror(errorCode, errorStr, AV_ERROR_MAX_STRING_SIZE);
        fullMessage_ = message + " (FFmpeg error: " + errorStr + ", code: " +
                       std::to_string(errorCode) + ")";
    } else {
        fullMessage_ = message;
    }
}

const char* FFmpegException::what() const noexcept {
    return fullMessage_.c_str();
}

int FFmpegException::getErrorCode() const {
    return errorCode_;
}