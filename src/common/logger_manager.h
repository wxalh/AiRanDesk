#ifndef LOGGER_MANAGER_H
#define LOGGER_MANAGER_H

#include <memory>
#include <string>
#include <QString>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/daily_file_sink.h>
#include "convert.h"

// 为QString添加fmt格式化器支持
// 不直接依赖外部 fmt 头；使用 spdlog 的fmt适配层
#include <spdlog/fmt/ostr.h>

template<>
struct fmt::formatter<QString> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.begin();
    }

    template<typename FormatContext>
    auto format(const QString& q, FormatContext& ctx) const {
        return format_to(ctx.out(), "{}", q.toStdString());
    }
};

#define CLASS_AND_FUNCTION() \
    (QString(__FUNCTION__))

// 辅助函数：QString转std::string，其它类型原样返回
template<typename T>
inline auto log_arg_cast(T&& arg) -> decltype(std::forward<T>(arg)) {
    return std::forward<T>(arg);
}
inline std::string log_arg_cast(const QString& arg) {
    return arg.toStdString();
}
inline std::string log_arg_cast(QString& arg) {
    return arg.toStdString();
}
inline std::string log_arg_cast(const QByteArray& arg) {
    return QString::fromLocal8Bit(arg).toStdString();
}
inline std::string log_arg_cast(QByteArray& arg) {
    return QString::fromLocal8Bit(arg).toStdString();
}

// 递归转换所有参数为tuple
template<typename... Args, std::size_t... I>
auto log_cast_tuple_impl(const std::tuple<Args...>& t, std::index_sequence<I...>) {
    return std::make_tuple(log_arg_cast(std::get<I>(t))...);
}
template<typename... Args>
auto log_cast_tuple(Args&&... args) {
    auto t = std::forward_as_tuple(std::forward<Args>(args)...);
    return log_cast_tuple_impl(t, std::index_sequence_for<Args...>{});
}

// 展开tuple并传递给spdlog
template<typename Tuple, typename F, std::size_t... I>
void log_apply(Tuple&& t, F&& f, std::index_sequence<I...>) {
    f(std::get<I>(std::forward<Tuple>(t))...);
}
template<typename Tuple, typename F>
void log_apply(Tuple&& t, F&& f) {
    constexpr auto size = std::tuple_size<std::decay_t<Tuple>>::value;
    log_apply(std::forward<Tuple>(t), std::forward<F>(f), std::make_index_sequence<size>{});
}

class LoggerManager
{
public:
    static LoggerManager& instance();

    void initialize(const QString& logFilePath = "");
    std::shared_ptr<spdlog::logger> getLogger(const QString& name = "default");

    // 便捷的日志宏
    template<typename... Args>
    void debug(const QString& fmt, Args&&... args) {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, [&](auto&&... unpacked) {
            instance().getLogger()->debug(fmt.toStdString(), std::forward<decltype(unpacked)>(unpacked)...);
        });
    }

    template<typename... Args>
    void info(const QString& fmt, Args&&... args) {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, [&](auto&&... unpacked) {
            instance().getLogger()->info(fmt.toStdString(), std::forward<decltype(unpacked)>(unpacked)...);
        });
    }

    template<typename... Args>
    void warn(const QString& fmt, Args&&... args) {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, [&](auto&&... unpacked) {
            instance().getLogger()->warn(fmt.toStdString(), std::forward<decltype(unpacked)>(unpacked)...);
        });
    }

    template<typename... Args>
    void error(const QString& fmt, Args&&... args) {
        auto tuple = log_cast_tuple(std::forward<Args>(args)...);
        log_apply(tuple, [&](auto&&... unpacked) {
            instance().getLogger()->error(fmt.toStdString(), std::forward<decltype(unpacked)>(unpacked)...);
        });
    }
private:
    LoggerManager() = default;
    ~LoggerManager() = default;
    LoggerManager(const LoggerManager&) = delete;
    LoggerManager& operator=(const LoggerManager&) = delete;
    
    // 私有帮助方法
    void setLogLevel(std::shared_ptr<spdlog::logger> logger) const;
    void setLogLevel(std::shared_ptr<spdlog::sinks::sink> sink) const;
    
    std::shared_ptr<spdlog::logger> m_logger;
    bool m_initialized = false;
    std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> console_sink;
    std::shared_ptr<spdlog::sinks::daily_file_sink_mt> file_sink;
};

#define LOG_GENERIC(LOGGER_PTR, LEVEL, FMT, ...) \
    do { \
        auto tuple = log_cast_tuple(__VA_ARGS__); \
        log_apply(tuple, [&](auto&&... unpacked) { \
            (LOGGER_PTR)->LEVEL(FMT, std::forward<decltype(unpacked)>(unpacked)...); \
        }); \
    } while(0)

// 简化的日志宏定义
#define LOG_TRACE(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), debug, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), debug, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), info, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), warn, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), warn, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) LOG_GENERIC(LoggerManager::instance().getLogger(CLASS_AND_FUNCTION()), error, fmt, ##__VA_ARGS__)
#endif // LOGGER_MANAGER_H
