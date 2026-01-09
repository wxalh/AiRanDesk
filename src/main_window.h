#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QWidget>
#include "ws_cli.h"
#include "webrtc_cli.h"

namespace Ui {
class MainWindow;
}
/**
 * @brief The MainWindow class  程序主窗口
 */
class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    //设置ui组件
    void initUI();
    //初始化websocket连接
    void initCli();
    //连接到文件传输
    void connFileMgr(const QString &remote_id,const QString &remote_pwd_md5);
    //连接到远程桌面窗口
    void connDesktopMgr(const QString &remote_id,const QString &remote_pwd_md5);
signals:
    void closeWsCli();
    void initWsCli(const QString &url,quint64 heart_interval_ms);
    void sendWsCliTextMsg(const QString &message);
    void sendWsCliBinaryMsg(const QByteArray &message);
    void initRtcCli();
private slots:
    //当连接按钮被触发
    void on_btn_conn_clicked();
    //当密码更新按钮被触发
    void on_local_pwd_change_clicked();
    //当分享按钮被触发
    void on_local_share_clicked();
    //websocket连接成功
    void onWsCliConnected();
    //websocket断开连接
    void onWsCliDisconnected();
    //websocket重连状态更新
    void onWsCliReconnectStatus(const QString &status, int phase, int attempt, int nextDelaySeconds);
    //websocket接收到文本消息
    void onWsCliRecvTextMsg(const QString &message);
    //websocket接收到二进制消息
    void onWsCliRecvBinaryMsg(const QByteArray &message);
private:
    Ui::MainWindow *ui;
    QString windowTitle;
    QString textToCopy;
    WsCli m_ws;
    QThread m_ws_thread;
    QMap<QString,QJsonObject> onlineMap;
    bool isCaptureing;
};

#endif // MAIN_WINDOW_H
