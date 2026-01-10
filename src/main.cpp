#include <control_window.h>
#include <main_window.h>
#include <ws_cli.h>
#include <QSharedMemory>
#include <QApplication>
#include <QTranslator>
#include <QAbstractSocket>
#include "logger_manager.h"

/**
 * @brief registerCustomTypes 注册自定义对象，为了Qt信号槽可以作为形参使用
 */
void registerCustomTypes()
{
    qRegisterMetaType<QAbstractSocket::SocketState>("QAbstractSocket::SocketState");
    qRegisterMetaType<rtc::PeerConnection::GatheringState>("rtc::PeerConnection::GatheringState");
    qRegisterMetaType<rtc::PeerConnection::State>("rtc::PeerConnection::State");
    qRegisterMetaType<rtc::message_variant>("rtc::message_variant");
    qRegisterMetaType<rtc::binary>("rtc::binary");
}
/**
 * @brief initLog   初始化日志组件
 * @return
 */
void initLog()
{
    // 初始化spdlog日志系统
    LoggerManager::instance().initialize();
    LOG_INFO("The log service was successfully initialized with spdlog.");
}
/**
 * @brief isRunning 禁止多开
 * @return
 */
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <Windows.h>
#else
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#endif

bool isRunning()
{
#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    // Windows 使用互斥锁
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"Global\\AiRan");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(hMutex);
        return true;
    }
    return false;
#else
    // Linux/macOS 使用文件锁
    static int lockFile = -1;
    if (lockFile == -1)
    {
        QString lockPath = QDir::temp().absoluteFilePath("AiRan.lock");
        lockFile = open(lockPath.toLocal8Bit().constData(), O_RDWR | O_CREAT, 0644);
    }

    if (lockFile != -1 && flock(lockFile, LOCK_EX | LOCK_NB) == -1)
    {
        return true; // 已锁定表示程序已在运行
    }
    return false;
#endif
}

/**
 * @brief main  入口函数
 * @param argc
 * @param argv
 * @return
 */
int main(int argc, char *argv[])
{
    #if (QT_VERSION >= QT_VERSION_CHECK(5, 1, 2))
        QApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
        QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
    #endif

    #if (QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
        QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);
    #endif
    
    QApplication::setOrganizationName("wxalh.com");
    QApplication::setApplicationName("airan");
    QApplication a(argc, argv);
    if (isRunning())
    {
        return 0;
    }
    /**
     * @brief qtTranslator  安装中文翻译包
     */
    QTranslator qtTranslator;
    if (qtTranslator.load("locale/qtbase_zh_CN.qm"))
    { // 文件放在可执行文件同级目录
        a.installTranslator(&qtTranslator);
    }
    registerCustomTypes();
    initLog();

    MainWindow w;
    if (ConfigUtil->showUI)
    {
        w.show();
    }
    
    // 确保应用程序正常退出
    int result = a.exec();
    
    // 在应用程序退出前做一些清理
    LOG_DEBUG("Application is about to exit");
    
    return result;
}
