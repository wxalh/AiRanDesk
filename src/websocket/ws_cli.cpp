#include "ws_cli.h"

WsCli::WsCli(QObject *parent)
    : QObject{parent}
{
}

WsCli::~WsCli()
{
    disconnect();
    if(m_ws){
        delete m_ws;
    }
}

void WsCli::init(const QString &url,quint64 heart_interval_ms)
{
    m_heart_interval_ms=heart_interval_ms;
    m_url=QUrl(url);
    m_connected=false;

    m_ws=new QWebSocket();
    connect(&m_heart_timer,&QTimer::timeout,this,&WsCli::sendHeartMsg);
    connect(this,SIGNAL(startHeartTimer(int)),&m_heart_timer,SLOT(start(int)));

    connect(this,&WsCli::wsClose,m_ws,&QWebSocket::close);
    connect(this,SIGNAL(wsOpen(QUrl)),m_ws,SLOT(open(QUrl)));
    connect(this,&WsCli::wsPing,m_ws,&QWebSocket::ping);


    connect(m_ws,&QWebSocket::aboutToClose,this,&WsCli::onWsAboutToClose);
    connect(m_ws,&QWebSocket::binaryMessageReceived,this,&WsCli::onWsBinaryMessageReceived);
    connect(m_ws,&QWebSocket::connected,this,&WsCli::onWsConnected);
    connect(m_ws,&QWebSocket::disconnected,this,&WsCli::onWsDisconnected);
    connect(m_ws,SIGNAL(error(QAbstractSocket::SocketError)),this,SLOT(onWsError(QAbstractSocket::SocketError)));
    connect(m_ws,&QWebSocket::pong,this,&WsCli::onWsPong);
    connect(m_ws,&QWebSocket::preSharedKeyAuthenticationRequired,this,&WsCli::onWsPreSharedKeyAuthenticationRequired);
    connect(m_ws,&QWebSocket::proxyAuthenticationRequired,this,&WsCli::onWsProxyAuthenticationRequired);
    connect(m_ws,&QWebSocket::sslErrors,this,&WsCli::onWsSslErrors);
    connect(m_ws,&QWebSocket::textMessageReceived,this,&WsCli::onWsTextMessageReceived);
    emit this->wsOpen(m_url);
}

void WsCli::onWsAboutToClose()
{
    LOG_ERROR("aboutToClose");
    this->m_connected=false;
}

void WsCli::onWsBinaryMessageReceived(const QByteArray &message)
{
    LOG_DEBUG("size:{}",message.size());
    emit this->onWsCliRecvBinaryMsg(message);
}

void WsCli::onWsTextMessageReceived(const QString &message)
{
    LOG_DEBUG(message);
    emit this->onWsCliRecvTextMsg(message);
}

void WsCli::onWsConnected()
{
    LOG_DEBUG("connected");
    this->m_connected=true;
    emit startHeartTimer(m_heart_interval_ms);
    emit this->onWsCliConnected();
}

void WsCli::onWsDisconnected()
{
    LOG_ERROR("disconnected");
    this->m_connected=false;
    emit onWsCliDisconnected();
}

void WsCli::onWsError(QAbstractSocket::SocketError error)
{
    LOG_ERROR("error {}",static_cast<int>(error));
}

void WsCli::onWsPong(quint64 elapsedTime, const QByteArray &payload)
{
    LOG_DEBUG("pong  elapsedTime: {} payload: {}", elapsedTime,payload.toStdString());
}

void WsCli::onWsPreSharedKeyAuthenticationRequired(QSslPreSharedKeyAuthenticator *authenticator)
{
    Q_UNUSED(authenticator);
    LOG_ERROR("onWsPreSharedKeyAuthenticationRequired");
}

void WsCli::onWsProxyAuthenticationRequired(const QNetworkProxy &proxy, QAuthenticator *authenticator)
{
    Q_UNUSED(proxy);
    Q_UNUSED(authenticator);
    LOG_ERROR("onWsProxyAuthenticationRequired");
}

void WsCli::onWsSslErrors(const QList<QSslError> &errors)
{
    Q_UNUSED(errors);
    LOG_ERROR("onWsSslErrors");
}

void WsCli::reConnect()
{
    emit this->wsOpen(m_url);
}

void WsCli::sendWsCliTextMsg(const QString &msg)
{
    m_ws->sendTextMessage(msg);
    m_ws->flush();
}

void WsCli::sendWsCliBinaryMsg(const QByteArray &msg)
{
    m_ws->sendBinaryMessage(msg);
    m_ws->flush();
}

void WsCli::sendHeartMsg()
{
    m_ws->sendTextMessage("@heart");
    m_ws->flush();
}
