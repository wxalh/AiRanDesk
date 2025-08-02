#include "logger_manager.h"
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include "config_util.h"

LoggerManager& LoggerManager::instance()
{
    static LoggerManager instance;
    return instance;
}

spdlog::level::level_enum LoggerManager::getLogLevel() const
{
    if(ConfigUtil->logLevel == "debug") {
        return spdlog::level::debug;
    } else if(ConfigUtil->logLevel == "info") {
        return spdlog::level::info;
    } else if(ConfigUtil->logLevel == "warn") {
        return spdlog::level::warn;
    } else if(ConfigUtil->logLevel == "error") {
        return spdlog::level::err;
    } else {
        return spdlog::level::off;
    }
}

void LoggerManager::setLogLevel(std::shared_ptr<spdlog::logger> logger) const
{
    logger->set_level(getLogLevel());
}

void LoggerManager::setLogLevel(std::shared_ptr<spdlog::sinks::sink> sink) const
{
    sink->set_level(getLogLevel());
}

void LoggerManager::initialize(const QString& logFilePath)
{
    if (m_initialized) {
        return;
    }
    
    try {
        // 设置日志文件路径
        QString logPath = logFilePath;
        if (logPath.isEmpty()) {
            QString appDataPath = QCoreApplication::applicationDirPath() +"/logs";
            QDir().mkpath(appDataPath);
            logPath = appDataPath + "/app.log";
        }
        
        // 创建多个输出器：控制台 + 滚动文件
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        setLogLevel(console_sink);
        console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$]\t[%n]\t- %v");
        
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            logPath.toStdString(), 1024 * 1024 * 10, 10); // 10MB per file, 10 files max
        setLogLevel(file_sink);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l]\t[%n]\t- %v");

        // 创建logger
        std::vector<spdlog::sink_ptr> sinks {console_sink, file_sink};
        m_logger = std::make_shared<spdlog::logger>("default", sinks.begin(), sinks.end());
        
        // 设置日志级别
        setLogLevel(m_logger);
        m_logger->flush_on(spdlog::level::info);
        
        // 注册到spdlog
        spdlog::register_logger(m_logger);
        spdlog::set_default_logger(m_logger);
        
        m_initialized = true;
        m_logger->info("Logger initialized, log file: {}", logPath.toStdString());
        
    } catch (const spdlog::spdlog_ex& ex) {
        // 如果初始化失败，创建一个简单的控制台logger
        m_logger = spdlog::stdout_color_mt("AiRan_fallback");
        m_logger->error("Log initialization failed: {}", ex.what());
        m_initialized = true;
    }
}

std::shared_ptr<spdlog::logger> LoggerManager::getLogger(const QString& name)
{
    if (!m_initialized) {
        initialize();
    }
    QString funcName=name;
    int pos = funcName.indexOf("::<lambda");
    funcName = (pos != -1) ? funcName.left(pos) : funcName; // 找到则截取，否则返回原串

    if (funcName == "default" || funcName.isEmpty()) {
        return m_logger;
    }
    
    // 返回指定名称的logger，如果不存在则创建
    auto logger = spdlog::get(funcName.toStdString());
    if (!logger) {
        // 创建新的logger，使用与默认logger相同的输出器和设置
        try {
            // 获取默认logger的输出器
            auto sinks = m_logger->sinks();
            
            // 创建新的logger
            logger = std::make_shared<spdlog::logger>(funcName.toStdString(), sinks.begin(), sinks.end());
            
            // 设置与默认logger相同的日志级别
            setLogLevel(logger);
            
            // 设置刷新级别
            logger->flush_on(spdlog::level::info);
            
            // 注册新的logger
            spdlog::register_logger(logger);
            
        } catch (const spdlog::spdlog_ex& ex) {
            // 如果创建失败，返回默认logger
            m_logger->error("Failed to create logger '{}': {}", funcName.toStdString(), ex.what());
            logger = m_logger;
        }
    }
    return logger;
}
