#ifndef FILE_PACKET_UTIL_H
#define FILE_PACKET_UTIL_H

#include <QObject>
#include <QMutex>
#include <QDateTime>
#include <QUuid>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QDataStream>
#include <QThread>
#include <QJsonObject>
#include <rtc/rtc.hpp>
#include <memory>
#include <map>

// 分包相关常量
constexpr quint64 FRAGMENT_SIZE = 8 * 1024; // 8KB
constexpr quint64 HEADER_SIZE = 32;         // 消息ID(16) + 总分包数(8) + 分包索引(8)
constexpr quint64 PAYLOAD_SIZE = FRAGMENT_SIZE - HEADER_SIZE; // 实际数据载荷大小
constexpr quint64 MAX_REASONABLE_OFFSET = 100LL * 1024 * 1024 * 1024; // 100GB

// 重组缓冲区结构
struct ReassemblyBuffer {
    quint64 totalFragments = 0;
    QString tempFilePath;  // 临时文件路径
    std::vector<bool> receivedFragments;  // 标记哪些分片已接收
    qint64 timestamp = 0; // 用于超时清理
    QFile* tempFile = nullptr;  // 临时文件对象
};

class FilePacketUtil : public QObject
{
    Q_OBJECT

public:
    explicit FilePacketUtil(QObject *parent = nullptr);
    ~FilePacketUtil();

    // 流式发送文件（避免大文件全部加载到内存）
    static bool sendFileStream(const QString &filePath, const QJsonObject &header, std::shared_ptr<rtc::DataChannel> channel);
    
    // 处理接收到的分包数据
    void processReceivedFragment(const rtc::binary &data, const QString &channelName);
    
    // 处理文件数据包（带头部信息的完整文件包）
    void processFileDataPacket(const QString &tempFilePath);

signals:
    // 文件下载完成信号
    void fileDownloadCompleted(bool status, const QString &tempPath);
    
    // 文件接收完成信号（通过分包重组）
    void fileReceived(bool status, const QString &tempPath);

private:
    // 重组分包
    void reassembleFragment(const QString &messageId, quint64 fragmentIndex, 
                           quint64 totalFragments, const rtc::binary &fragment);
    
    // 流式复制文件数据
    bool streamCopyFile(QFile &sourceFile, qint64 sourceOffset, const QString &targetPath, qint64 dataSize);

    // 重组缓冲区映射表
    std::map<QString, ReassemblyBuffer> m_reassemblyBuffers;
    
    // 重组互斥锁
    QMutex m_reassemblyMutex;
};

#endif // FILE_PACKET_UTIL_H
