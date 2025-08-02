#include <key_util.h>
#include <control_window.h>
#include "constant.h"
#include "util/json_util.h"
#include <QScrollBar>
#include <QLayout>
#include <QApplication>
#include <QScreen>
#include <QRect>
#include <QStyle>
#include <QResizeEvent>

ControlWindow::ControlWindow(QString remoteId, QString remotePwdMd5, WsCli *_ws_cli, QWidget *parent)
    : QMainWindow(parent), isReceivedImg(false), windowSizeAdjusted(false),
      remote_id(remoteId), remote_pwd_md5(remotePwdMd5), m_rtc_ctl(remoteId, remotePwdMd5, false), m_ws(_ws_cli)
{
    initUI();
    initCLI();
    // 初始化WebRtcCtl
    emit initRtcCtl();
}

ControlWindow::~ControlWindow()
{
    disconnect();
    if (m_rtc_ctl_thread.isRunning())
    {
        m_rtc_ctl_thread.quit();
        if(!m_rtc_ctl_thread.wait(3000))
        {
            LOG_WARN("WebRtcCtl thread did not quit gracefully, terminating");
            m_rtc_ctl_thread.terminate();
            m_rtc_ctl_thread.wait(1000);
        }
    }
}

void ControlWindow::initUI()
{
    setAttribute(Qt::WA_DeleteOnClose); // 关闭时自动触发deleteLater()

    // 设置窗口标志：禁用最大化按钮和缩放功能，但保留最小化和关闭
    setWindowFlags(Qt::Window | Qt::WindowTitleHint | Qt::WindowSystemMenuHint |
                   Qt::WindowMinimizeButtonHint | Qt::WindowCloseButtonHint);

    // 设置固定大小策略，禁止用户手动调整窗口大小
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    setWindowTitle("远程：" + remote_id);

    // 设置初始窗口大小（将在收到第一帧视频时自动调整）
    resize(800, 600);

    // 使用传统QLabel渲染
    label.setText("正在连接...");
    label.setAlignment(Qt::AlignCenter);
    label.setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    // 设置文字颜色为白色，在黑色背景上可见
    label.setStyleSheet("QLabel { background: black; border: none; margin: 0px; padding: 0px; color: white; font-size: 16px; }");

    // 将QLabel设置为滚动区域的子部件
    scrollArea.setWidget(&label);

    LOG_INFO("Initialized with QLabel video rendering, window size will auto-adjust to video");

    // 禁用滚动条作为默认设置（当视频适合屏幕时）
    scrollArea.setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea.setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // 设置Chrome风格的滚动条样式（备用，当需要时启用）
    scrollArea.setStyleSheet(
        "QScrollArea {"
        "    border: none;"
        "    background: black;" // 确保背景是黑色而不是白色
        "    margin: 0px;"
        "    padding: 0px;"
        "}"
        "QScrollArea > QWidget > QWidget {"
        "    background: black;" // 确保内部widget也是黑色背景
        "}"
        "QScrollBar:vertical {"
        "    background: rgba(0,0,0,0);"
        "    width: 8px;"
        "    border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical {"
        "    background: rgba(128,128,128,0.5);"
        "    border-radius: 4px;"
        "    min-height: 20px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "    background: rgba(128,128,128,0.8);"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "    border: none;"
        "    background: none;"
        "    height: 0px;"
        "}"
        "QScrollBar:horizontal {"
        "    background: rgba(0,0,0,0);"
        "    height: 8px;"
        "    border-radius: 4px;"
        "}"
        "QScrollBar::handle:horizontal {"
        "    background: rgba(128,128,128,0.5);"
        "    border-radius: 4px;"
        "    min-width: 20px;"
        "}"
        "QScrollBar::handle:horizontal:hover {"
        "    background: rgba(128,128,128,0.8);"
        "}"
        "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {"
        "    border: none;"
        "    background: none;"
        "    width: 0px;"
        "}");

    scrollArea.setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    scrollArea.setAlignment(Qt::AlignCenter);
    // 确保滚动区域没有内边距和边框
    scrollArea.setContentsMargins(0, 0, 0, 0);
    scrollArea.setFrameShape(QFrame::NoFrame); // 去除边框
    scrollArea.setLineWidth(0);
    scrollArea.setMidLineWidth(0);

    // 确保label也没有边框和内边距
    label.setContentsMargins(0, 0, 0, 0);
    // 注释掉原来的样式设置，因为上面已经设置了包含颜色的完整样式
    // label.setStyleSheet("QLabel { background: black; border: none; margin: 0px; padding: 0px; }");

    this->setCentralWidget(&scrollArea);

    // 确保主窗口也没有额外的边距
    this->setContentsMargins(0, 0, 0, 0);
    this->centralWidget()->setContentsMargins(0, 0, 0, 0);
}

void ControlWindow::initCLI()
{
    connect(&m_rtc_ctl, &WebRtcCtl::sendWsCliBinaryMsg, m_ws, &WsCli::sendWsCliBinaryMsg);
    connect(&m_rtc_ctl, &WebRtcCtl::sendWsCliTextMsg, m_ws, &WsCli::sendWsCliTextMsg);
    connect(m_ws, &WsCli::onWsCliRecvBinaryMsg, &m_rtc_ctl, &WebRtcCtl::onWsCliRecvBinaryMsg);
    connect(m_ws, &WsCli::onWsCliRecvTextMsg, &m_rtc_ctl, &WebRtcCtl::onWsCliRecvTextMsg);

    // 配置rtc工作逻辑
    connect(this, &ControlWindow::initRtcCtl, &m_rtc_ctl, &WebRtcCtl::init);
    connect(this, &ControlWindow::sendMsg2InputChannel, &m_rtc_ctl, &WebRtcCtl::inputChannelSendMsg);

    connect(&m_rtc_ctl, &WebRtcCtl::videoFrameDecoded, this, &ControlWindow::updateImg);

    m_rtc_ctl.moveToThread(&m_rtc_ctl_thread);
    m_rtc_ctl_thread.start();
}

void ControlWindow::keyPressEvent(QKeyEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    // 发送给远端
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_KEYBOARD)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_KEY, KeyUtil::qtKeyToWinKey(event->key()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_DOWN)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    // LOG_DEBUG(msg);

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::keyReleaseEvent(QKeyEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    // 发送给远端
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_KEYBOARD)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_KEY, KeyUtil::qtKeyToWinKey(event->key()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_UP)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    // LOG_DEBUG(msg);

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    // 发送给远端
    QPointF pos = getNormPoint(event->pos());
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_MOVE)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    // LOG_DEBUG(msg);

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::mousePressEvent(QMouseEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    // 发送给远端
    QPointF pos = getNormPoint(event->pos());
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_BUTTON, static_cast<int>(event->button()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_DOWN)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    // LOG_DEBUG(msg);

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    // 发送给远端
    QPointF pos = getNormPoint(event->pos());
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_BUTTON, static_cast<int>(event->button()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_UP)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    // LOG_DEBUG(msg);

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    // 发送给远端
    QPointF pos = getNormPoint(event->pos());
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_BUTTON, static_cast<int>(event->button()))
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_DOUBLECLICK)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    // LOG_DEBUG(msg);

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::wheelEvent(QWheelEvent *event)
{
    if (!isReceivedImg)
    {
        return;
    }
    // 发送给远端
    QPointF pos = getNormPoint(event->pos());
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_MOUSE)
                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                          .add(Constant::KEY_RECEIVER, remote_id)
                          .add(Constant::KEY_RECEIVER_PWD, remote_pwd_md5)
                          .add(Constant::KEY_X, pos.x())
                          .add(Constant::KEY_Y, pos.y())
                          .add(Constant::KEY_DWFLAGS, Constant::KEY_WHEEL)
                          .add(Constant::KEY_MOUSEDATA, event->angleDelta().y())
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    // LOG_DEBUG(msg);

    rtc::message_variant msgStr(msg.toStdString());
    emit sendMsg2InputChannel(msgStr);
}

void ControlWindow::resizeEvent(QResizeEvent *event)
{
    // 如果窗口大小已经根据视频调整过，阻止用户手动调整
    if (windowSizeAdjusted)
    {
        LOG_DEBUG("Blocking manual resize attempt, window size is fixed to video size");
        return; // 不调用父类方法，阻止调整大小
    }

    // 在窗口大小调整之前，允许正常的调整大小操作
    QMainWindow::resizeEvent(event);
}

QPointF ControlWindow::getNormPoint(const QPoint &pos)
{
    // 获取鼠标在label内的坐标
    QPointF labelPos = pos - label.pos(); // 如果label不是顶层控件，需要正确计算相对位置

    QPointF res;
    // 获取label和pixmap的尺寸
    QSize labelSize = label.size();
    QSize pixmapSize = label.pixmap()->size();

    // 计算实际显示区域（保持宽高比）
    QSize scaledSize = pixmapSize.scaled(labelSize, Qt::KeepAspectRatio);
    QPointF offset(
        (labelSize.width() - scaledSize.width()) / 2.0,
        (labelSize.height() - scaledSize.height()) / 2.0);

    // 检查点击是否在有效区域内
    if (labelPos.x() < offset.x() ||
        labelPos.y() < offset.y() ||
        labelPos.x() > (offset.x() + scaledSize.width()) ||
        labelPos.y() > (offset.y() + scaledSize.height()))
    {
        // 点击在边框区域，不处理
        return res;
    }

    // 转换为pixmap坐标
    qreal scaleFactor = qMin(
        (qreal)scaledSize.width() / pixmapSize.width(),
        (qreal)scaledSize.height() / pixmapSize.height());
    QPointF pixmapPos(
        (labelPos.x() - offset.x()) / scaleFactor,
        (labelPos.y() - offset.y()) / scaleFactor);

    // 归一化坐标
    qreal x_n = pixmapPos.x() / pixmapSize.width();
    qreal y_n = pixmapPos.y() / pixmapSize.height();

    res.setX(x_n);
    res.setY(y_n);
    return res;
}

void ControlWindow::updateImg(const QImage &img)
{
    // 验证输入图像
    if (img.isNull() || img.width() <= 0 || img.height() <= 0)
    {
        LOG_WARN("Received invalid image: null={}, size={}x{}",
                 img.isNull(), img.width(), img.height());
        return;
    }
    isReceivedImg = true;
    // 在收到第一帧有效视频时，调整窗口大小
    if (!windowSizeAdjusted)
    {
        adjustWindowSizeToVideo(img.size());
    }

    // 使用传统QLabel渲染（保持原有逻辑）

    // 检查图像数据质量（可选的质量检测）
    static int consecutiveBadFrames = 0;
    bool imageQualityGood = true;

    // 简单的图像质量检测：检查是否有足够的非零像素
    if (img.depth() >= 24)
    { // 确保是彩色图像
        const uchar *bits = img.constBits();
        int totalBytes = img.byteCount();
        int nonZeroBytes = 0;

        // 采样检查前1000字节
        int checkBytes = std::min(1000, totalBytes);
        for (int i = 0; i < checkBytes; i++)
        {
            if (bits[i] != 0)
            {
                nonZeroBytes++;
            }
        }

        // 如果非零字节太少，可能是解码问题
        if (nonZeroBytes < checkBytes / 20)
        { // 少于5%的非零字节
            imageQualityGood = false;
            consecutiveBadFrames++;
            LOG_WARN("Detected potentially corrupted frame: {}/{} non-zero bytes, consecutive bad frames: {}",
                     nonZeroBytes, checkBytes, consecutiveBadFrames);
        }
        else
        {
            consecutiveBadFrames = 0; // 重置计数
        }
    }

    // 如果连续收到太多质量差的帧，显示警告但仍然尝试显示
    if (consecutiveBadFrames > 5)
    {
        // 显示连接质量警告，但不阻止显示
        static bool warningShown = false;
        if (!warningShown)
        {
            LOG_ERROR("Video quality appears poor, may need to check network connection or request keyframe");
            warningShown = true;
            // 可以在UI上显示警告文字，保持白色文字和黑色背景
            label.setStyleSheet("QLabel { background: black; border: 2px solid red; margin: 0px; padding: 0px; color: white; font-size: 16px; }");
        }
    }
    else if (consecutiveBadFrames == 0)
    {
        // 恢复正常样式，保持白色文字和黑色背景
        label.setStyleSheet("QLabel { background: black; border: none; margin: 0px; padding: 0px; color: white; font-size: 16px; }");
    }

    // 确保图像格式一致，避免格式转换问题
    QImage displayImg = img;
    if (img.format() != QImage::Format_RGB888)
    {
        displayImg = img.convertToFormat(QImage::Format_RGB888);
        LOG_DEBUG("Converted image format from {} to RGB888", static_cast<int>(img.format()));
    }

    // 使用Qt的优化转换，并设置合适的转换标志
    QPixmap pixmap = QPixmap::fromImage(displayImg, Qt::ColorOnly);

    // 检查转换结果
    if (pixmap.isNull())
    {
        LOG_ERROR("Failed to convert QImage to QPixmap, image size: {}x{}, format: {}",
                  displayImg.width(), displayImg.height(), static_cast<int>(displayImg.format()));
        return;
    }

    // 更新标签尺寸和内容 - 优化渲染延迟
    label.setFixedSize(displayImg.size());
    label.setPixmap(pixmap);

    // 注意：不要在这里修改scrollArea的大小，因为它已经在adjustWindowSizeToVideo中根据屏幕大小智能设置了

    // 优化重绘策略，减少延迟
    label.update(); // 使用update()而不是repaint()，让Qt优化重绘时机

    // 减少统计输出频率，降低日志开销
    static int frameCount = 0;
    frameCount++;
    // if (frameCount % 300 == 0)
    // { // 每300帧输出一次，减少日志开销
    //     LOG_INFO("QLabel rendered frame #{}, size: {}x{}, quality check: {}",
    //              frameCount, displayImg.width(), displayImg.height(),
    //              imageQualityGood ? "good" : "poor");
    // }
}

void ControlWindow::adjustWindowSizeToVideo(const QSize &videoSize)
{
    if (windowSizeAdjusted)
    {
        return; // 已经调整过，避免重复调整
    }

    LOG_INFO("Adjusting window size to match video: {}x{}", videoSize.width(), videoSize.height());

    // 获取屏幕尺寸
    QScreen *screen = QApplication::primaryScreen();
    QRect screenGeometry = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);

    LOG_INFO("Screen available geometry: {}x{}", screenGeometry.width(), screenGeometry.height());
    int titleBarHeight = style()->pixelMetric(QStyle::PM_TitleBarHeight);
    int maxContentWidth = screenGeometry.width();                    // 减去边框和滚动条的宽度
    int maxContentHeight = screenGeometry.height() - titleBarHeight; // 减去边框
    // 设置label大小为原始视频大小（用于鼠标坐标计算）
    label.setFixedSize(videoSize);

    // 设置滚动区域大小
    scrollArea.setFixedSize(videoSize);

    bool needMaximize = videoSize.height() > maxContentHeight || videoSize.width() > maxContentWidth;
    // 判断是否需要滚动条：仅当视频大于屏幕时才需要
    if (needMaximize)
    {
        scrollArea.setFixedSize(maxContentWidth, maxContentHeight);
        this->showMaximized();
    }

    // 强制布局更新
    scrollArea.updateGeometry();
    this->updateGeometry();

    // 确保窗口大小正确设置
    this->adjustSize(); // 让窗口自动调整到内容大小

    if (!needMaximize)
    {
        // 居中显示窗口
        QRect windowGeometry = this->geometry();
        windowGeometry.moveCenter(screenGeometry.center());

        // 确保窗口不会超出屏幕边界
        if (windowGeometry.left() < screenGeometry.left())
        {
            windowGeometry.moveLeft(screenGeometry.left());
        }
        if (windowGeometry.top() < screenGeometry.top())
        {
            windowGeometry.moveTop(screenGeometry.top());
        }
        if (windowGeometry.right() > screenGeometry.right())
        {
            windowGeometry.moveRight(screenGeometry.right());
        }
        if (windowGeometry.bottom() > screenGeometry.bottom())
        {
            windowGeometry.moveBottom(screenGeometry.bottom());
        }

        this->setGeometry(windowGeometry);

        LOG_INFO("Window positioned at: ({}, {}), size: {}x{}",
                 windowGeometry.x(), windowGeometry.y(),
                 windowGeometry.width(), windowGeometry.height());
    }
    windowSizeAdjusted = true;
}
