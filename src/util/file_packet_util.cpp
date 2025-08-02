#include "file_packet_util.h"
#include "logger_manager.h"
#include "constant.h"
#include "util/json_util.h"
#include <thread>
#include <chrono>

FilePacketUtil::FilePacketUtil(QObject *parent)
    : QObject(parent)
{
}

FilePacketUtil::~FilePacketUtil()
{
    QMutexLocker locker(&m_reassemblyMutex);
    
    // 清理所有临时文件和文件对象
    for (auto& pair : m_reassemblyBuffers) {
        if (pair.second.tempFile) {
            pair.second.tempFile->close();
            delete pair.second.tempFile;
        }
        if (!pair.second.tempFilePath.isEmpty()) {
            QFile::remove(pair.second.tempFilePath);
        }
    }
    
    m_reassemblyBuffers.clear();
}

bool FilePacketUtil::sendFileStream(const QString &filePath, const QJsonObject &header, std::shared_ptr<rtc::DataChannel> channel)
{
    if (!channel || !channel->isOpen()) {
        LOG_ERROR("Channel not available for file streaming");
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_ERROR("Failed to open file for streaming: {} error: {}", filePath, file.errorString());
        return false;
    }

    // 准备头部数据
    QByteArray headerBytes = JsonUtil::toCompactBytes(header);
    QByteArray headerSizeBytes;
    QDataStream headerStream(&headerSizeBytes, QIODevice::WriteOnly);
    headerStream.setByteOrder(QDataStream::BigEndian);
    headerStream << static_cast<quint32>(headerBytes.size());

    // 计算总数据大小
    quint64 totalDataSize = 4 + headerBytes.size() + file.size();
    quint64 totalFragments = (totalDataSize + PAYLOAD_SIZE - 1) / PAYLOAD_SIZE;

    LOG_INFO("Starting stream send for file: {} ({}, {} fragments)", filePath,
             Convert::formatFileSize(totalDataSize), totalFragments);

    // 生成消息ID
    QUuid messageId = QUuid::createUuid();
    QByteArray messageIdBytes = messageId.toRfc4122();
    
    LOG_DEBUG("Generated message ID: {}", messageId.toString());

    // 准备数据源缓冲区，先加入头部数据
    QByteArray dataBuffer;
    dataBuffer.append(headerSizeBytes);
    dataBuffer.append(headerBytes);

    quint64 fragmentIndex = 0;
    quint64 totalSent = 0;

    // 开始流式发送分包
    while (fragmentIndex < totalFragments) {
        // 准备当前分包数据
        QByteArray fragmentPayload;
        
        // 先从缓冲区获取数据
        if (!dataBuffer.isEmpty()) {
            int toTake = qMin(static_cast<int>(PAYLOAD_SIZE), dataBuffer.size());
            fragmentPayload.append(dataBuffer.left(toTake));
            dataBuffer.remove(0, toTake);
        }
        
        // 如果载荷还不够且文件还有数据，从文件读取
        while (fragmentPayload.size() < PAYLOAD_SIZE && !file.atEnd()) {
            QByteArray fileData = file.read(PAYLOAD_SIZE - fragmentPayload.size());
            if (fileData.isEmpty()) {
                break;
            }
            fragmentPayload.append(fileData);
        }

        // 如果没有数据了就结束
        if (fragmentPayload.isEmpty()) {
            break;
        }

        // 创建完整的分包
        rtc::binary fragment(FRAGMENT_SIZE);
        
        // 写入分包头部
        std::memcpy(fragment.data(), messageIdBytes.constData(), 16);
        
        // 写入总分包数（大端序）
        LOG_DEBUG("Writing totalFragments: {} (0x{:X})", totalFragments, totalFragments);
        for (int i = 0; i < 8; ++i) {
            int shift = (7-i) * 8;
            uint64_t totalFragments64 = static_cast<uint64_t>(totalFragments);
            uint64_t shifted = totalFragments64 >> shift;
            std::byte val = static_cast<std::byte>(shifted & 0xFF);
            fragment[16 + i] = val;
            LOG_DEBUG("i={}, shift={}, shifted=0x{:X}, val={}", i, shift, shifted, static_cast<int>(val));
        }
        
        // 写入分包索引（大端序）
        LOG_DEBUG("Writing fragmentIndex: {} (0x{:X})", fragmentIndex, fragmentIndex);
        for (int i = 0; i < 8; ++i) {
            int shift = (7-i) * 8;
            uint64_t fragmentIndex64 = static_cast<uint64_t>(fragmentIndex);
            uint64_t shifted = fragmentIndex64 >> shift;
            std::byte val = static_cast<std::byte>(shifted & 0xFF);
            fragment[24 + i] = val;
            LOG_DEBUG("FragIdx i={}, shift={}, shifted=0x{:X}, val={}", i, shift, shifted, static_cast<int>(val));
        }

        // 复制载荷数据
        std::memcpy(fragment.data() + HEADER_SIZE, fragmentPayload.constData(), fragmentPayload.size());

        // 如果载荷不足分包大小，用0填充剩余部分
        if (fragmentPayload.size() < PAYLOAD_SIZE) {
            std::memset(fragment.data() + HEADER_SIZE + fragmentPayload.size(), 0, 
                       PAYLOAD_SIZE - fragmentPayload.size());
        }

        // 发送分包
        try {
            channel->send(fragment);
            totalSent += fragmentPayload.size();
            
            // 定期输出进度日志，并包含更多调试信息
            if (fragmentIndex % 100 == 0 || fragmentIndex == totalFragments - 1) {
                LOG_DEBUG("Sent fragment {}/{} ({}) - MessageID: {}", 
                         fragmentIndex + 1, totalFragments, Convert::formatFileSize(totalSent),
                         messageId.toString());
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to send fragment {}: {}", fragmentIndex, e.what());
            file.close();
            return false;
        }

        fragmentIndex++;
        
        // 小延迟避免过快发送导致网络拥塞
        if (fragmentIndex % 10 == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    file.close();

    LOG_INFO("Successfully sent file stream: {} ({}, {} fragments)", 
             filePath, Convert::formatFileSize(totalDataSize), totalFragments);
    return true;
}

void FilePacketUtil::processReceivedFragment(const rtc::binary &data, const QString &channelName)
{
    if (data.size() < HEADER_SIZE)
    {
        LOG_ERROR("Fragment too small: {}", Convert::formatFileSize(data.size()));
        return;
    }

    // 解析包头
    QByteArray messageIdArray;
    messageIdArray.resize(16);
    for (int i = 0; i < 16; ++i)
    {
        messageIdArray[i] = static_cast<char>(data[i]);
    }
    QUuid messageId = QUuid::fromRfc4122(messageIdArray);

    if (messageId.isNull())
    {
        LOG_ERROR("Invalid message ID in fragment");
        return;
    }

    // 解析总分包数（大端序，8字节）
    quint64 totalFragments = 0;
    for (int i = 0; i < 8; ++i)
    {
        totalFragments = (totalFragments << 8) | static_cast<quint8>(data[16 + i]);
    }

    // 解析分包索引（大端序，8字节）
    quint64 fragmentIndex = 0;
    for (int i = 0; i < 8; ++i)
    {
        fragmentIndex = (fragmentIndex << 8) | static_cast<quint8>(data[24 + i]);
    }

    // 验证解析结果的合理性
    if (totalFragments == 0 || totalFragments > 1000000) { // 最多100万个分片
        LOG_ERROR("Invalid totalFragments: {}", totalFragments);
        return;
    }
    
    if (fragmentIndex >= totalFragments) {
        LOG_ERROR("Invalid fragmentIndex: {} >= {}", fragmentIndex, totalFragments);
        return;
    }

    // 添加调试日志
    LOG_DEBUG("Fragment received - ID: {}, Index: {}/{}, Size: {}", 
             messageId.toString(), fragmentIndex, totalFragments, 
             Convert::formatFileSize(data.size()));

    // 提取载荷
    rtc::binary fragment(data.begin() + HEADER_SIZE, data.end());

    QString fullMessageId = channelName + "_" + messageId.toString();
    reassembleFragment(fullMessageId, fragmentIndex, totalFragments, fragment);
}

void FilePacketUtil::processFileDataPacket(const QString &tempFilePath)
{
    QFile tempFile(tempFilePath);
    if (!tempFile.open(QIODevice::ReadOnly)) {
        LOG_ERROR("Failed to open temp file for processing: {}", tempFilePath);
        return;
    }

    // 只读取头部信息（前4个字节 + 头部JSON）
    if (tempFile.size() < 4) {
        LOG_ERROR("Temp file too small to contain header size");
        tempFile.close();
        return;
    }

    QByteArray headerSizeBytes = tempFile.read(4);
    QDataStream stream(headerSizeBytes);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 headerSize;
    stream >> headerSize;

    if (headerSize > tempFile.size() - 4) {
        LOG_ERROR("Invalid header size: {}, total file: {}", headerSize, tempFile.size());
        tempFile.close();
        return;
    }

    // 读取头部JSON
    QByteArray headerBytes = tempFile.read(headerSize);
    QJsonObject header = JsonUtil::safeParseObject(headerBytes);

    if (!JsonUtil::isValidObject(header)) {
        LOG_ERROR("Failed to parse file data packet header");
        tempFile.close();
        return;
    }

    QString msgType = JsonUtil::getString(header, Constant::KEY_MSGTYPE);
    QString ctlPath = JsonUtil::getString(header, Constant::KEY_PATH_CTL);
    QString cliPath = JsonUtil::getString(header, Constant::KEY_PATH_CLI);

    // 计算实际文件数据的大小和起始位置
    qint64 fileDataStart = 4 + headerSize;
    qint64 fileDataSize = tempFile.size() - fileDataStart;

    if (msgType == Constant::TYPE_FILE_DOWNLOAD && !ctlPath.isEmpty() && !cliPath.isEmpty())
    {
        // 流式复制文件数据到目标位置
        if (streamCopyFile(tempFile, fileDataStart, ctlPath, fileDataSize)) {
            emit fileDownloadCompleted(true, ctlPath);
            LOG_INFO("Received file download: {} ({})", ctlPath, Convert::formatFileSize(fileDataSize));
        } else {
            emit fileDownloadCompleted(false, ctlPath);
        }
    }
    else if (msgType == Constant::TYPE_FILE_UPLOAD && !ctlPath.isEmpty() && !cliPath.isEmpty())
    {
        // 流式复制文件数据到目标位置
        if (streamCopyFile(tempFile, fileDataStart, cliPath, fileDataSize)) {
            emit fileReceived(true, cliPath);
            LOG_INFO("Received file upload: {} ({})", cliPath, Convert::formatFileSize(fileDataSize));
        } else {
            emit fileReceived(false, cliPath);
        }
    }
    else
    {
        LOG_WARNING("Unknown file data packet type: {} ({})", msgType, JsonUtil::toCompactString(header));
    }

    tempFile.close();
}

bool FilePacketUtil::streamCopyFile(QFile &sourceFile, qint64 sourceOffset, const QString &targetPath, qint64 dataSize)
{
    // 确保目标目录存在
    QFileInfo targetFileInfo(targetPath);
    QDir().mkpath(targetFileInfo.absolutePath());

    // 打开目标文件
    QFile targetFile(targetPath);
    if (!targetFile.open(QIODevice::WriteOnly)) {
        LOG_ERROR("Failed to create target file: {} error: {}", targetPath, targetFile.errorString());
        return false;
    }

    // 定位到源文件的数据起始位置
    if (!sourceFile.seek(sourceOffset)) {
        LOG_ERROR("Failed to seek source file to offset: {}", sourceOffset);
        targetFile.close();
        return false;
    }

    // 流式复制数据
    const qint64 BUFFER_SIZE = 64 * 1024; // 64KB 缓冲区
    QByteArray buffer;
    qint64 totalCopied = 0;
    qint64 remaining = dataSize;

    while (remaining > 0 && !sourceFile.atEnd()) {
        qint64 toRead = qMin(BUFFER_SIZE, remaining);
        buffer = sourceFile.read(toRead);
        
        if (buffer.isEmpty()) {
            break;
        }

        qint64 written = targetFile.write(buffer);
        if (written != buffer.size()) {
            LOG_ERROR("Failed to write to target file: {} written: {}, expected: {}", 
                     targetPath, written, buffer.size());
            targetFile.close();
            QFile::remove(targetPath);
            return false;
        }

        totalCopied += written;
        remaining -= written;
    }

    targetFile.flush();
    targetFile.close();

    // 验证复制的数据大小
    if (totalCopied != dataSize) {
        LOG_ERROR("File copy size mismatch: {} copied: {}, expected: {}", 
                 targetPath, totalCopied, dataSize);
        QFile::remove(targetPath);
        return false;
    }

    // 等待文件真正落盘
    QFileInfo checkFile(targetPath);
    checkFile.refresh();
    int retries = 0;
    while (!checkFile.exists() && retries < 10) {
        QThread::msleep(10);
        checkFile.refresh();
        retries++;
    }

    if (!checkFile.exists()) {
        LOG_ERROR("Target file does not exist after copy: {}", targetPath);
        return false;
    }

    return true;
}

void FilePacketUtil::reassembleFragment(const QString &messageId, quint64 fragmentIndex,
                                        quint64 totalFragments, const rtc::binary &fragment)
{
    QMutexLocker locker(&m_reassemblyMutex);

    if (totalFragments == 0 || fragmentIndex >= totalFragments)
    {
        LOG_ERROR("Invalid fragment parameters: index={}, total={}", fragmentIndex, totalFragments);
        return;
    }

    ReassemblyBuffer &buffer = m_reassemblyBuffers[messageId];

    // 初始化缓冲区
    if (buffer.receivedFragments.empty())
    {
        buffer.totalFragments = totalFragments;
        buffer.receivedFragments.resize(totalFragments, false);
        buffer.timestamp = QDateTime::currentMSecsSinceEpoch();
        
        // 创建临时文件
        QString safeMessageId = QString(messageId).replace("/", "_").replace("\\", "_");
        buffer.tempFilePath = QDir::tempPath() + "/" + safeMessageId + ".tmp";
        buffer.tempFile = new QFile(buffer.tempFilePath);
        if (!buffer.tempFile->open(QIODevice::WriteOnly)) {
            LOG_ERROR("Failed to create temp file for reassembly: {}", buffer.tempFilePath);
            delete buffer.tempFile;
            buffer.tempFile = nullptr;
            return;
        }
        
        LOG_DEBUG("Created reassembly temp file: {}", buffer.tempFilePath);
    }

    if (!buffer.tempFile) {
        LOG_ERROR("Temp file not available for fragment reassembly");
        return;
    }

    // 验证偏移量计算的合理性
    quint64 offset = fragmentIndex * PAYLOAD_SIZE;

    if (offset > MAX_REASONABLE_OFFSET) {
        LOG_ERROR("Invalid fragment offset calculated: {} (fragmentIndex: {}, PAYLOAD_SIZE: {})", 
                 offset, fragmentIndex, PAYLOAD_SIZE);
        return;
    }
    
    // 将分片载荷写入文件的对应位置（按载荷偏移量，不是分片偏移量）
    if (!buffer.tempFile->seek(offset)) {
        LOG_ERROR("Failed to seek temp file to offset: {}", offset);
        return;
    }
    
    qint64 written = buffer.tempFile->write(reinterpret_cast<const char*>(fragment.data()), fragment.size());
    buffer.tempFile->flush();
    
    if (written != static_cast<qint64>(fragment.size())) {
        LOG_ERROR("Failed to write fragment to temp file: {} (wanted: {}, written: {})", 
                 buffer.tempFilePath, Convert::formatFileSize(fragment.size()), Convert::formatFileSize(written));
        return;
    }

    buffer.receivedFragments[fragmentIndex] = true;
    
    LOG_DEBUG("Fragment {}/{} written to temp file at offset {} ({})", 
             fragmentIndex + 1, totalFragments, offset, Convert::formatFileSize(fragment.size()));

    // 检查是否完整
    bool complete = true;
    for (quint64 i = 0; i < totalFragments; ++i)
    {
        if (!buffer.receivedFragments[i])
        {
            complete = false;
            break;
        }
    }

    if (complete)
    {
        LOG_DEBUG("Fragment reassembly complete, temp file: {}", buffer.tempFilePath);

        buffer.tempFile->close();
        delete buffer.tempFile;
        buffer.tempFile = nullptr;

        // 直接处理临时文件，而不是读取到内存
        if (messageId.contains("file")) {
            processFileDataPacket(buffer.tempFilePath);
        }

        // 清理临时文件和缓冲区
        QFile::remove(buffer.tempFilePath);
        m_reassemblyBuffers.erase(messageId);
    }
}
