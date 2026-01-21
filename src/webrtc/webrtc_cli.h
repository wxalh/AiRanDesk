#ifndef WEBRTC_CLI_H
#define WEBRTC_CLI_H

#include <QThread>
#include <QEventLoop>
#include <QJsonObject>
#include <QMetaEnum>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMutex>
#include <QJsonArray>
#include <QJsonDocument>
#include <QImage>
#include <QSettings>
#include <QDir>
#include <QRect>
#include <QHostInfo>
#include <QScreen>
#include <QMessageBox>
#include <QMetaObject>
#include <QGuiApplication>
#include <QTimer>
#include <QDateTime>
#include <QUuid>
#include <memory>
#include <rtc/rtc.hpp>
#include <config_util.h>
#include <input_util.h>
#include "constant.h"
#include "util/json_util.h"

// 前向声明
class MediaCapture;
class FilePacketUtil;

/**
 * @brief The WebRtcCli class 被控端的webrtc对象（main_window需要用到的）
 * 主动发起webrtc连接，创建轨道和数据通道
 */
class WebRtcCli : public QObject
{
    Q_OBJECT
public:
    WebRtcCli(const QString &remoteId, int fps, bool isOnlyFile,
        int controlMaxWidth = 1920, int controlMaxHeight = 1080, QObject *parent = nullptr);
    ~WebRtcCli();

    // 解析来自WebSocket的消息
    void parseWsMsg(const QJsonObject &object);

private:
    // WebRTC核心功能
    void initPeerConnection();
    void createTracksAndChannels();
    void setupCallbacks();
    void destroy();

    // 媒体处理
    void startMediaCapture();
    void stopMediaCapture();

    // 回调设置
    void setupFileChannelCallbacks();
    void setupFileTextChannelCallbacks();
    void setupInputChannelCallbacks();

    // 成员变量
    QString m_remoteId;
    bool m_isOnlyFile; // 是否仅文件传输
    QDir m_currentDir;

    // WebRTC相关
    std::shared_ptr<rtc::PeerConnection> m_peerConnection;
    std::shared_ptr<rtc::DataChannel> m_fileChannel;
    std::shared_ptr<rtc::DataChannel> m_fileTextChannel;
    std::shared_ptr<rtc::DataChannel> m_inputChannel;
    std::shared_ptr<rtc::Track> m_videoTrack;
    std::shared_ptr<rtc::Track> m_audioTrack;
    // 连接状态
    bool m_connected;
    bool m_channelsReady;
    bool m_destroying; // 是否正在销毁

    int m_fps; // 帧率
    // 媒体相关
    MediaCapture *m_mediaCapture;
    qint64 m_baseTimestamp;

    // 文件分包工具类
    FilePacketUtil *m_filePacketUtil;

    // ICE服务器配置
    std::string m_host;
    uint16_t m_port;
    std::string m_username;
    std::string m_password;
    int m_screen_width;
    int m_screen_height;
    
    // 编码分辨率（根据控制端屏幕分辨率智能计算）
    int m_encode_width;
    int m_encode_height;
signals:
    // WebSocket消息发送
    void sendWsCliBinaryMsg(const QByteArray &message);
    void sendWsCliTextMsg(const QString &message);

    // 数据通道事件
    void destroyCli();
    
    // 媒体控制事件
    void requestKeyFrameFromCapture(); // 请求媒体捕获生成关键帧
public slots:
    // 初始化WebRTC连接并创建所有通道
    void init();
    // WebSocket消息处理
    void onWsCliRecvBinaryMsg(const QByteArray &message);
    void onWsCliRecvTextMsg(const QString &message);

    void onVideoFrameReady(const rtc::binary &h264Data);
    void onAudioFrameReady(const rtc::binary &audioData);

    void handleFileReceived(bool status, const QString &tempPath);

private:
    // 消息解析
    void parseFileMsg(const QJsonObject &object);
    void parseInputMsg(const QJsonObject &object);

    // 信令处理
    void setRemoteDescription(const QString &data, const QString &type);
    void addIceCandidate(const QString &candidate, const QString &mid);

    // 文件传输
    void populateLocalFiles();

    void sendFile(const QString &cliPath, const QString &ctlPath);
    void sendSingleFile(const QString &cliPath, const QString &ctlPath);
    void sendDirectory(const QString &cliPath, const QString &ctlPath);
    void sendFileErrorResponse(const QString &filePath, const QString &error);
    void sendUploadResponse(const QString &fileName, bool success, const QString &message);
    void saveUploadedFile(const QString &filePath, const QByteArray &data);

    // 输入处理
    void handleMouseEvent(const QJsonObject &object);
    void handleKeyboardEvent(const QJsonObject &object);

    // 通道消息发送
    void sendFileChannelMessage(const QJsonObject &message);
    void sendFileTextChannelMessage(const QJsonObject &message);
    void sendInputChannelMessage(const QJsonObject &message);
    
    // 智能分辨率计算
    void calculateOptimalResolution(int controlMaxWidth, int controlMaxHeight);

    // 分包数据处理（使用QByteArray存储分包）
    QMap<QString, QVector<QByteArray>> m_uploadFragments;
};

#endif // WEBRTC_CLI_H
