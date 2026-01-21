#ifndef CONTROL_WINDOW_H
#define CONTROL_WINDOW_H

#include <QMainWindow>
#include <QWheelEvent>
#include <QScrollArea>
#include <QTimer>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QFrame>
#include <QApplication>
#include <QClipboard>
#include <QButtonGroup>
#include <config_util.h>
#include <webrtc_ctl.h>
#include <ws_cli.h>

QT_BEGIN_NAMESPACE

class ControlWindow : public QMainWindow
{
    Q_OBJECT
public:
    ControlWindow(QString remoteId, QString remotePwdMd5, WsCli *_ws_cli,
                  bool adaptiveResolution = false, QWidget *parent = nullptr);
    ~ControlWindow();
    void initUI();
    void initCLI();

    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override; // 重写以防止手动调整大小

    // 获取归一化坐标
    QPointF getNormPoint(const QPoint &pos);

    // 浮动工具栏相关方法
    void createFloatingToolbar();
    void updateToolbarPosition();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

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
    
    // 自适应分辨率设置
    bool m_adaptiveResolution;

    // 浮动工具栏
    QFrame *m_floatingToolbar;
    QPushButton *m_screenshotBtn;
    QPushButton *m_fileTransferBtn;
    QPushButton *m_ocrBtn;
    
    // 工具栏拖拽相关
    bool m_draggingToolbar;
    QPoint m_dragStartPosition;
    QPoint m_toolbarOffset;
    QSize m_windowSize; // 用于存储窗口大小
signals:
    void sendMsg2InputChannel(const rtc::message_variant &data);
    void initRtcCtl();
public slots:
    void updateImg(const QImage &img);
    
    // 工具栏按钮槽函数
    void onScreenshotClicked();
    void onFileTransferClicked();
    void onOcrClicked();
    
private slots:
    void adjustWindowSizeToVideo(const QSize &videoSize); // 根据视频尺寸调整窗口大小
};
#endif // CONTROL_WINDOW_H
