#ifndef FILETRANSFERTOOL_H
#define FILETRANSFERTOOL_H

#include <QWidget>
#include <QLabel>
#include <QStyle>
#include <QDir>
#include <QIcon>
#include <QTableWidgetItem>
#include <config_util.h>
#include <webrtc_ctl.h>
#include <ws_cli.h>
#include "convert.h"

namespace Ui {
class FileTransferWindow;
}

class FileTransferWindow : public QWidget
{
    Q_OBJECT
public:
    // 构造函数：传入远程ID、远程密码MD5、WebSocket客户端实例
    explicit FileTransferWindow(QString remoteId,QString remotePwdMd5,WsCli *_ws_cli,QWidget *parent = nullptr);
    ~FileTransferWindow();
    // 初始化UI界面
    void initUI();
    // 初始化WebRTC控制器
    void initCLI();
    // 处理键盘事件
    void keyPressEvent(QKeyEvent *event) override;
signals:
    // 信号：发送消息到输入数据通道
    void inputChannelSendMsg(const rtc::message_variant& data);
    // 信号：发送消息到文件数据通道  
    void fileChannelSendMsg(const rtc::message_variant& data);
    // 信号：发送消息到文件文本数据通道  
    void fileTextChannelSendMsg(const rtc::message_variant& data);
    // 信号：初始化webrtc
    void initRtcCtl();
    // 信号：上传文件到CLI
    void uploadFile2CLI(const QString &ctlPath, const QString &cliPath);
public slots:
    // 处理上传按钮点击事件
    void onUploadButtonClicked();
    // 处理下载按钮点击事件
    void onDownloadButtonClicked();
    // 接收到被控端文件列表
    void recvGetFileList(const QJsonObject &object);
    //接收到被控端文件
    void recvDownloadFile(bool status, const QString &filePath);
    void recvUploadFileRes(bool status, const QString &filePath);
private slots:
    // 处理本地路径选择变化
    void on_localPathCombo_textActivated(const QString &path);
    // 处理本地文件列表双击事件
    void on_localTable_cellDoubleClicked(int row, int column);

    void on_remotePathCombo_textActivated(const QString &path);
    // 处理远程路径选择变化
    void on_remoteTable_cellDoubleClicked(int row, int column);
private:
    // 设置本地文件列表
    void populateLocalFiles();
    // 设置远程文件列表
    void populateRemoteFiles();
    // 设置文件列表
    void setupFileTables();
    // 设置传输记录表格
    void setupLogTable();
    // 更新本地路径显示
    void updateLocalPathCombo();
    // 更新远程路径显示
    void updateRemotePathCombo();
private:
    Ui::FileTransferWindow *ui;
    bool connected;
    QLabel label;
    QString remote_id;
    QString remote_pwd_md5;
    WebRtcCtl m_rtc_ctl;
    WsCli *m_ws;
    QThread m_rtc_ctl_thread;

    QDir currentLocalDir;
    QString currentRemotePath;  // 当前远程路径
    QIcon dirIcon;
    QIcon fileIcon;

    QJsonArray remoteFiles;
};

#endif // FILETRANSFERTOOL_H
