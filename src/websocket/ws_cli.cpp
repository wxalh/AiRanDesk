#include "ws_cli.h"

WsCli::WsCli(QObject *parent)
    : QObject{parent}, m_reconnect_phase(0), m_reconnect_count(0)
{
}

WsCli::~WsCli()
{
    disconnect();
    m_heart_timer.stop();
    m_reconnect_timer.stop();
    if(m_ws){
        delete m_ws;
    }
}

void WsCli::init(const QString &url,quint64 heart_interval_ms)
{
    m_heart_interval_ms=heart_interval_ms;
    m_url=QUrl(url);
    m_connected=false;
    m_reconnect_phase = 0;
    m_reconnect_count = 0;

    m_ws=new QWebSocket();
    connect(&m_heart_timer,&QTimer::timeout,this,&WsCli::sendHeartMsg);
    connect(&m_reconnect_timer,&QTimer::timeout,this,&WsCli::attemptReconnect);
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
    LOG_INFO("WebSocket connected successfully");
    this->m_connected=true;
    
    // 重置重连状态
    m_reconnect_phase = 0;
    m_reconnect_count = 0;
    m_reconnect_timer.stop();
    
    emit startHeartTimer(m_heart_interval_ms);
    emit this->onWsCliConnected();
}

void WsCli::onWsDisconnected()
{
    LOG_WARN("WebSocket disconnected, starting intelligent reconnect");
    this->m_connected=false;
    m_heart_timer.stop();
    
    emit onWsCliDisconnected();
    
    // 启动智能重连
    scheduleReconnect();
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
    if (m_connected && m_ws) {
        m_ws->sendTextMessage("@heart");
        m_ws->flush();
    }
}

void WsCli::scheduleReconnect()
{
    if (m_connected) {
        return;  // 已连接，无需重连
    }
    
    int delay = 1000;  // 默认1秒
    
    switch (m_reconnect_phase) {
        case 0:  // 1秒重试阶段
            delay = 1000;
            break;
        case 1:  // 10秒重试阶段
            delay = 10000;
            break;
        case 2:  // 30秒重试阶段
            delay = 30000;
            break;
        case 3:  // 60秒重试阶段（永久）
            delay = 60000;
            break;
    }
    
    LOG_INFO("Scheduling reconnect in {}ms (phase: {}, attempt: {})", 
             delay, m_reconnect_phase, m_reconnect_count + 1);
    
    m_reconnect_timer.setSingleShot(true);
    m_reconnect_timer.start(delay);
}

void WsCli::attemptReconnect()
{
    if (m_connected) {
        return;  // 已连接，停止重连
    }
    
    m_reconnect_count++;
    
    LOG_INFO("Attempting reconnect (phase: {}, attempt: {})", m_reconnect_phase, m_reconnect_count);
    
    // 尝试重连
    if (m_ws) {
        m_ws->open(m_url);
    }
    
    // 检查是否需要进入下一个重连阶段
    if (m_reconnect_count >= MAX_RETRY_PER_PHASE && m_reconnect_phase < 3) {
        m_reconnect_phase++;
        m_reconnect_count = 0;
        LOG_INFO("Moving to reconnect phase {}", m_reconnect_phase);
    } else if (m_reconnect_phase == 3) {
        // 第四阶段（60秒）永久重试，重置计数但不改变阶段
        if (m_reconnect_count >= MAX_RETRY_PER_PHASE) {
            m_reconnect_count = 0;
        }
    }
    
    // 继续调度下一次重连（如果仍未连接）
    QTimer::singleShot(1000, this, [this]() {
        if (!m_connected) {
            scheduleReconnect();
        }
    });
}
