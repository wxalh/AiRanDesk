#ifndef WS_CLI_H
#define WS_CLI_H

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <QAuthenticator>
#include <QNetworkProxy>
#include <QSslPreSharedKeyAuthenticator>
/* spdlog头文件引用 */
#include "logger_manager.h"

class WsCli : public QObject
{
    Q_OBJECT
public:
    explicit WsCli(QObject *parent = nullptr);
    ~WsCli();
private:
    quint64 m_heart_interval_ms;
    QTimer m_heart_timer;
    QTimer m_reconnect_timer;
    QWebSocket *m_ws;
    QUrl m_url;
    bool m_connected;
    bool autoConnect;
    
    // 重连机制相关
    int m_reconnect_phase;  // 0: 1秒重试, 1: 10秒重试, 2: 30秒重试, 3: 60秒重试
    int m_reconnect_count;  // 当前阶段重试次数
    static const int MAX_RETRY_PER_PHASE = 10;  // 每个阶段最大重试次数
    
    void scheduleReconnect();
signals:
    void startReconnectTimer(int msec);
    void stopReconnectTimer();
    // 心跳定时器信号
    void startHeartTimer(int msec);
    void stopHeartTimer();
    void wsClose(QWebSocketProtocol::CloseCode closeCode = QWebSocketProtocol::CloseCodeNormal, const QString &reason = QString());
    void wsOpen(const QUrl &url);
    void wsPing(const QByteArray &payload = QByteArray());
    void onWsCliDisconnected();
    void onWsCliConnected();
    void onWsCliRecvTextMsg(const QString &message);
    void onWsCliRecvBinaryMsg(const QByteArray &message);
    // 重连状态更新信号
    void onReconnectStatusUpdate(const QString &status, int phase, int attempt, int nextDelaySeconds);
public slots:
    void init(const QString &url,quint64 heart_interval_ms);
    void onWsConnected();
    void onWsDisconnected();
    void onWsBinaryMessageReceived(const QByteArray &message);
    void onWsTextMessageReceived(const QString &message);
    void onWsError(QAbstractSocket::SocketError error);
    void onWsAboutToClose();
    void onWsPong(quint64 elapsedTime, const QByteArray &payload);
    void onWsPreSharedKeyAuthenticationRequired(QSslPreSharedKeyAuthenticator *authenticator);
    void onWsProxyAuthenticationRequired(const QNetworkProxy &proxy, QAuthenticator *authenticator);
    void onWsSslErrors(const QList<QSslError> &errors);

    void reConnect();
    void attemptReconnect();  // 智能重连尝试
    void sendWsCliTextMsg(const QString &msg);
    void sendWsCliBinaryMsg(const QByteArray &msg);
    void sendHeartMsg();
};

#endif // WS_CLI_H
