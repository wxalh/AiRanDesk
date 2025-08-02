#ifndef CONTROL_WINDOW_H
#define CONTROL_WINDOW_H

#include <QMainWindow>
#include <QWheelEvent>
#include <QScrollArea>
#include <QTimer>
#include <QLabel>
#include <config_util.h>
#include <webrtc_ctl.h>
#include <ws_cli.h>

QT_BEGIN_NAMESPACE

class ControlWindow : public QMainWindow
{
    Q_OBJECT
public:
    ControlWindow(QString remoteId, QString remotePwdMd5, WsCli *_ws_cli, QWidget *parent = nullptr);
    ~ControlWindow();
    void initUI();
    void initCLI();

    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override; // 重写以防止手动调整大小

    // 获取归一化坐标
    QPointF getNormPoint(const QPoint &pos);

private:
    bool isReceivedImg;      // 是否接收到图片
    bool windowSizeAdjusted; // 标记窗口大小是否已经根据视频调整过
    QScrollArea scrollArea;

    QLabel label;

    QString remote_id;
    QString remote_pwd_md5;
    WebRtcCtl m_rtc_ctl;
    WsCli *m_ws;
    QThread m_rtc_ctl_thread;
signals:
    void sendMsg2InputChannel(const rtc::message_variant &data);
    void initRtcCtl();
public slots:
    void updateImg(const QImage &img);
private slots:
    void adjustWindowSizeToVideo(const QSize &videoSize); // 根据视频尺寸调整窗口大小
};
#endif // CONTROL_WINDOW_H
