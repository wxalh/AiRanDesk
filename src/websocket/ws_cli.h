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
    QWebSocket *m_ws;
    QUrl m_url;
    bool m_connected;
    bool autoConnect;
signals:
    void startHeartTimer(int msec);
    void wsClose(QWebSocketProtocol::CloseCode closeCode = QWebSocketProtocol::CloseCodeNormal, const QString &reason = QString());
    void wsOpen(const QUrl &url);
    void wsPing(const QByteArray &payload = QByteArray());
    void onWsCliDisconnected();
    void onWsCliConnected();
    void onWsCliRecvTextMsg(const QString &message);
    void onWsCliRecvBinaryMsg(const QByteArray &message);
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
    void sendWsCliTextMsg(const QString &msg);
    void sendWsCliBinaryMsg(const QByteArray &msg);
    void sendHeartMsg();
};

#endif // WS_CLI_H
