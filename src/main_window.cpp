#include "main_window.h"
#include "ui_main_window.h"
#include "control_window.h"
#include "file_transfer_window.h"
#include "constant.h"
#include "util/json_util.h"
#include <QMessageBox>
#include <QMap>
#include <QClipboard>
#include <QHostInfo>
#include <QThread>
#include <QBuffer>
#include <QGuiApplication>
#include <QScreen>
#include <QCryptographicHash>

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent), ui(new Ui::MainWindow), windowTitle("AiRan"), textToCopy("欢迎使用{}远程工具，您的识别码：{} \n验证码: {}"), isCaptureing(false)
{
    initUI();
    initCli();
}

MainWindow::~MainWindow()
{
    disconnect();
    if (m_ws_thread.isRunning())
    {
        m_ws_thread.quit();
        if (!m_ws_thread.wait(3000))
        {
            LOG_WARN("websocket thread did not quit gracefully, terminating");
            m_ws_thread.terminate();
            m_ws_thread.wait(1000);
        }
    }
    delete ui;
}

void MainWindow::initUI()
{
    ui->setupUi(this);
    setWindowTitle(windowTitle);
    setWindowFlags(windowFlags() & ~Qt::WindowMaximizeButtonHint);

    ui->local_id->setText(ConfigUtil->local_id);
    ui->local_pwd->setText(ConfigUtil->getLocalPwd());
    ui->local_id->setReadOnly(true);
    ui->local_pwd->setReadOnly(true);
}

void MainWindow::initCli()
{
    // 连接websocket相关信号
    connect(&m_ws, &WsCli::onWsCliConnected, this, &MainWindow::onWsCliConnected);
    connect(&m_ws, &WsCli::onWsCliRecvBinaryMsg, this, &MainWindow::onWsCliRecvBinaryMsg);
    connect(&m_ws, &WsCli::onWsCliRecvTextMsg, this, &MainWindow::onWsCliRecvTextMsg);
    connect(this, &MainWindow::initWsCli, &m_ws, &WsCli::init);
    connect(this, &MainWindow::reConnectWsCli, &m_ws, &WsCli::reConnect);
    connect(this, &MainWindow::sendWsCliBinaryMsg, &m_ws, &WsCli::sendWsCliBinaryMsg);
    connect(this, &MainWindow::sendWsCliTextMsg, &m_ws, &WsCli::sendWsCliTextMsg);

    // 将WebSocket客户端移动到工作线程
    m_ws.moveToThread(&m_ws_thread);
    m_ws_thread.start();
    QString wsUrl = ConfigUtil->wsUrl;
    wsUrl = wsUrl.append("?sessionId=")
                .append(ConfigUtil->local_id)
                .append("&hostname=")
                .append(QHostInfo::localHostName());

    emit initWsCli(wsUrl, 30 * 1000);
}

void MainWindow::connFileMgr(const QString &remote_id, const QString &remote_pwd_md5)
{
    if (onlineMap.contains(remote_id))
    {
        // 发送给远端
        FileTransferWindow *fw = new FileTransferWindow(remote_id, remote_pwd_md5, &m_ws);
        fw->showMaximized();
    }
    else
    {
        LOG_ERROR("设备不在线，无法连接文件传输");
        if (ConfigUtil->showUI)
        {
            QMessageBox::critical(nullptr, "错误", "设备不在线");
        }
    }
}

void MainWindow::connDesktopMgr(const QString &remote_id, const QString &remote_pwd_md5)
{
    if (onlineMap.contains(remote_id))
    {
        ControlWindow *cw = new ControlWindow(remote_id, remote_pwd_md5, &m_ws);
        cw->show();
    }
    else
    {
        LOG_ERROR("设备不在线，无法远程桌面");
        if (ConfigUtil->showUI)
        {
            QMessageBox::critical(nullptr, "错误", "设备不在线");
        }
    }
}

void MainWindow::on_btn_conn_clicked()
{
    QString remote_id = ui->remote_id->text();
    QString remote_pwd = ui->remote_pwd->text();

    if (remote_id.isEmpty()|| remote_pwd.isEmpty())
    {
        LOG_ERROR("错误,远端识别码和密码不能为空");
        if (ConfigUtil->showUI)
        {
            QMessageBox::critical(this, "错误", "远端识别码和密码不能为空");
        }
        return;
    }
    QByteArray hashResult = QCryptographicHash::hash(remote_pwd.toUtf8(), QCryptographicHash::Md5);
    QString remote_pwd_md5 = hashResult.toHex().toUpper();

    if (ui->remote_desktop->isChecked())
    {
        connDesktopMgr(remote_id, remote_pwd_md5);
    }
    else if (ui->remote_file->isChecked())
    {
        connFileMgr(remote_id, remote_pwd_md5);
    }
}

void MainWindow::on_local_pwd_change_clicked()
{
    ConfigUtil->setLocalPwd(QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper());
    ConfigUtil->saveIni();
    ui->local_pwd->setText(ConfigUtil->getLocalPwd());
}

void MainWindow::on_local_share_clicked()
{
    // 获取系统剪切板
    QClipboard *clipboard = QApplication::clipboard();

    // 将文本写入剪切板
    clipboard->setText(textToCopy.arg(windowTitle, ConfigUtil->local_id, ConfigUtil->getLocalPwd()));
}

void MainWindow::onWsCliConnected()
{
    LOG_INFO("websocket connected");
    ui->ws_connect_status->setText("服务器已连接");
}

void MainWindow::onWsCliDisconnected()
{
    LOG_ERROR("websocket disconnected");
    ui->ws_connect_status->setText("服务器断开连接，正在重连...");
    emit reConnectWsCli();
}

void MainWindow::onWsCliRecvTextMsg(const QString &message)
{
    onWsCliRecvBinaryMsg(message.toUtf8());
}

void MainWindow::onWsCliRecvBinaryMsg(const QByteArray &message)
{
    QJsonObject object = JsonUtil::safeParseObject(message);
    if (!JsonUtil::isValidObject(object))
    {
        LOG_ERROR("Failed to parse JSON in main window");
        return;
    }

    QString sender = JsonUtil::getString(object, Constant::KEY_SENDER);
    QString type = JsonUtil::getString(object, Constant::KEY_TYPE);

    if (sender.isEmpty() || type.isEmpty())
    {
        LOG_ERROR("Missing sender or type in message");
        return;
    }
    if (sender == Constant::ROLE_SERVER)
    {
        if (type == Constant::TYPE_ONLINE_ONE)
        {
            QJsonValue dataVal = object.value(Constant::KEY_DATA);
            if (!dataVal.isObject())
            {
                LOG_ERROR("Invalid data object in ONLINE_ONE message");
                return;
            }

            QJsonObject rcsUserObj = dataVal.toObject();
            QString sn = JsonUtil::getString(rcsUserObj, Constant::KEY_SN);
            if (!sn.isEmpty())
            {
                onlineMap.insert(sn, rcsUserObj);
            }
            else
            {
                LOG_ERROR("Missing SN in ONLINE_ONE user data");
            }
        }
        else if (type == Constant::TYPE_ONLINE_LIST)
        {
            QJsonValue dataVal = object.value(Constant::KEY_DATA);
            if (!dataVal.isArray())
            {
                LOG_ERROR("Invalid data array in ONLINE_LIST message");
                return;
            }

            QJsonArray rcsUserArr = dataVal.toArray();
            for (const QJsonValue &val : rcsUserArr)
            {
                if (!val.isObject())
                {
                    continue;
                }
                QJsonObject rcsUserObj = val.toObject();
                QString sn = JsonUtil::getString(rcsUserObj, Constant::KEY_SN);
                if (!sn.isEmpty())
                {
                    onlineMap.insert(sn, rcsUserObj);
                }
            }
        }
        else if (type == Constant::TYPE_OFFLINE_ONE)
        {
            QJsonValue dataVal = object.value(Constant::KEY_DATA);
            if (!dataVal.isObject())
            {
                LOG_ERROR("Invalid data object in OFFLINE_ONE message");
                return;
            }

            QJsonObject rcsUserObj = dataVal.toObject();
            QString sn = JsonUtil::getString(rcsUserObj, Constant::KEY_SN);
            if (!sn.isEmpty())
            {
                onlineMap.remove(sn);
            }
        }
        else if (type == Constant::TYPE_ERROR)
        {
            QString data = JsonUtil::getString(object, Constant::KEY_DATA);
            if (data.isEmpty())
            {
                LOG_ERROR("参数错误,缺失data");
                return;
            }

            LOG_ERROR("错误: " + data);
            if (ConfigUtil->showUI)
            {
                QMessageBox::critical(nullptr, "错误", data);
            }
        }
    }
    else if (type == Constant::TYPE_CONNECT)
    {
        QString receiverPwd = JsonUtil::getString(object, Constant::KEY_RECEIVER_PWD, "");
        if (receiverPwd.isEmpty() || receiverPwd != ConfigUtil->local_pwd_md5)
        {
            LOG_ERROR("Missing receiver password in CONNECT message");
            return;
        }
        int fps = JsonUtil::getInt(object, Constant::KEY_FPS, 15);
        bool isOnlyFile = JsonUtil::getBool(object, Constant::KEY_IS_ONLY_FILE, false);

        QThread *m_rtc_cli_thread = new QThread();
        QString senderName = QString("WebRtcCli_%1_%2").arg(sender, isOnlyFile ? "file" : "desktop");
        m_rtc_cli_thread->setObjectName(senderName);
        WebRtcCli *m_rtc_cli = new WebRtcCli(sender, fps, isOnlyFile);

        connect(&m_ws, &WsCli::onWsCliRecvBinaryMsg, m_rtc_cli, &WebRtcCli::onWsCliRecvBinaryMsg);
        connect(&m_ws, &WsCli::onWsCliRecvTextMsg, m_rtc_cli, &WebRtcCli::onWsCliRecvTextMsg);
        connect(m_rtc_cli, &WebRtcCli::sendWsCliBinaryMsg, &m_ws, &WsCli::sendWsCliBinaryMsg);
        connect(m_rtc_cli, &WebRtcCli::sendWsCliTextMsg, &m_ws, &WsCli::sendWsCliTextMsg);
        connect(m_rtc_cli, &WebRtcCli::destroyCli, [this, senderName, m_rtc_cli, m_rtc_cli_thread]()
                {
                    if (m_rtc_cli)
                    {
                        delete m_rtc_cli;
                        LOG_INFO("WebRtcCli for {} destroyed", senderName);
                    }
                    if (m_rtc_cli_thread)
                    {
                        m_rtc_cli_thread->quit();
                        if (!m_rtc_cli_thread->wait(3000))
                        {
                            LOG_WARN("WebRtcCli thread {} did not quit gracefully, terminating", senderName);
                            m_rtc_cli_thread->terminate();
                            m_rtc_cli_thread->wait(1000);
                        }
                        delete m_rtc_cli_thread;
                    }
                    LOG_INFO("WebRtcCli thread for {} destroyed", senderName); });
        m_rtc_cli->moveToThread(m_rtc_cli_thread);
        m_rtc_cli_thread->start();
        QMetaObject::invokeMethod(m_rtc_cli, "init", Qt::QueuedConnection);
    }
}
