#include "media_capture.h"
#include "h264_encoder.h"
#include "logger_manager.h"
#include <QPixmap>
#include <QBuffer>
#include <QGuiApplication>
#include <QScreen>
#include <QAudioInput>
#include <QAudioOutput>
#include <QIODevice>
#include <QDateTime>
#include <algorithm>
#include <cmath>

// Qt 5 兼容性
#include <QAudioDeviceInfo>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// 视频捕获工作者实现
CaptureWorker::CaptureWorker(QObject *parent)
    : QObject(parent), m_running(false), m_width(1920), m_height(1080), m_fps(10), m_lastFrameTime(0), m_forceKeyFrame(false), m_encoder(nullptr), m_captureTimer(nullptr)
{
    m_encoder = new H264Encoder(this);
    m_captureTimer = new QTimer(this);
    connect(m_captureTimer, &QTimer::timeout, this, &CaptureWorker::captureFrame);
}

CaptureWorker::~CaptureWorker()
{
    stopCapture();
}

void CaptureWorker::startCapture(int width, int height, int fps)
{
    QMutexLocker locker(&m_mutex);
    m_width = width;
    m_height = height;
    m_fps = fps;

    // 初始化H264编码器（启用硬件加速）
    if (m_encoder)
    {
        // 设置高质量编码参数
        int bitrate = width * height * fps * 0.1; // 自适应码率

        // 尝试启用硬件加速编码
        QStringList availableAccels = H264Encoder::getAvailableHWAccels();
        bool encoderInitialized = false;

        if (!availableAccels.isEmpty())
        {
            LOG_INFO("Available hardware encoders: {}", availableAccels.join(", "));

            // 优先级：Intel QSV > NVIDIA > AMD
            QStringList preferredOrder = {"qsv", "nvenc", "amf"};

            for (const QString &preferred : preferredOrder)
            {
                if (availableAccels.contains(preferred))
                {
                    LOG_INFO("Attempting to initialize H264 encoder with {} acceleration", preferred);
                    if (m_encoder->initialize(width, height, fps, bitrate))
                    {
                        LOG_INFO("Successfully initialized H264 encoder with {} hardware acceleration", preferred);
                        encoderInitialized = true;
                        break;
                    }
                    else
                    {
                        LOG_WARN("Failed to initialize H264 encoder with {} acceleration", preferred);
                    }
                }
            }
        }

        // 如果硬件加速失败，使用软件编码
        if (!encoderInitialized)
        {
            LOG_INFO("Falling back to software H264 encoding");
            if (!m_encoder->initialize(width, height, fps, bitrate))
            {
                LOG_ERROR("Failed to initialize H264 encoder even with software encoding");
            }
            else
            {
                LOG_INFO("Successfully initialized H264 encoder with software encoding");
            }
        }
    }

    m_running = true;

    // 计算定时器间隔
    int interval = 1000 / fps; // ms
    m_captureTimer->start(interval);

    emit captureStarted();
    LOG_INFO("CaptureWorker started: {}x{} @ {}fps", width, height, fps);
}

void CaptureWorker::stopCapture()
{
    QMutexLocker locker(&m_mutex);
    m_running = false;

    if (m_captureTimer)
    {
        m_captureTimer->stop();
    }

    emit captureStopped();
    LOG_INFO("CaptureWorker stopped");
}

void CaptureWorker::captureFrame()
{
    if (!m_running)
        return;

    // 检查是否需要强制生成关键帧
    bool forceKey = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_forceKeyFrame) {
            m_forceKeyFrame = false;  // 重置标志
            forceKey = true;
        }
    }

    // 截图并编码为H264
    rtc::binary h264Data = captureScreenH264();
    if (!h264Data.empty())
    {
        QMutexLocker locker(&m_mutex);
        m_lastFrameTime = QDateTime::currentMSecsSinceEpoch();
        locker.unlock();

        if (forceKey) {
            LOG_INFO("🔑 Generated key frame in response to request");
        }

        emit frameReady(h264Data);
        LOG_DEBUG("Captured and sent video frame: {}", Convert::formatFileSize(h264Data.size()));
    }
}

rtc::binary CaptureWorker::captureScreenH264()
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen || !m_encoder)
    {
        return rtc::binary();
    }

    // 截取屏幕
    QPixmap pixmap = screen->grabWindow(0);
    if (pixmap.isNull())
    {
        return rtc::binary();
    }

    // 转换为QImage
    QImage image = pixmap.toImage();

    // 如果需要调整大小，保持高质量缩放
    if (image.width() != m_width || image.height() != m_height)
    {
        image = image.scaled(m_width, m_height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    // 使用H264编码器编码
    return m_encoder->encodeFrame(image);
}

void CaptureWorker::forceKeyFrame()
{
    QMutexLocker locker(&m_mutex);
    m_forceKeyFrame = true;
    LOG_INFO("🔑 Key frame requested for next capture");
}

// 音频捕获工作者实现
AudioCaptureWorker::AudioCaptureWorker(QObject *parent)
    : QObject(parent), m_running(false), m_sampleRate(44100), m_channels(2),
      m_captureTimer(nullptr), m_levelCheckTimer(nullptr),
      m_audioInput(nullptr), m_audioDevice(nullptr), m_audioBuffer(nullptr),
      m_audioInitialized(false), m_hasAudioActivity(false), m_audioThreshold(0.01)
{
    m_captureTimer = new QTimer(this);
    m_levelCheckTimer = new QTimer(this);
    connect(m_captureTimer, &QTimer::timeout, this, &AudioCaptureWorker::captureAudio);
    connect(m_levelCheckTimer, &QTimer::timeout, this, &AudioCaptureWorker::checkAudioLevel);
}

AudioCaptureWorker::~AudioCaptureWorker()
{
    stopCapture();
    cleanupAudio();
}

void AudioCaptureWorker::startCapture(int sampleRate, int channels)
{
    QMutexLocker locker(&m_mutex);
    m_sampleRate = sampleRate;
    m_channels = channels;

    if (!initializeAudio())
    {
        LOG_ERROR("Failed to initialize audio capture");
        return;
    }

    m_running = true;
    m_hasAudioActivity = false;

    // 启动音频输入
    if (m_audioInput && m_audioBuffer)
    {
        m_audioDevice = m_audioInput->start();
        if (m_audioDevice)
        {
            connect(m_audioDevice, &QIODevice::readyRead, this, &AudioCaptureWorker::processAudioData);
            LOG_INFO("Audio input started successfully");
        }
        else
        {
            LOG_ERROR("Failed to start audio input");
        }
    }

    // 启动音频电平检测定时器（每100ms检查一次）
    m_levelCheckTimer->start(100);

    emit captureStarted();
    LOG_INFO("AudioCaptureWorker started: {} Hz, {} channels (capturing system audio output)", sampleRate, channels);
}

void AudioCaptureWorker::stopCapture()
{
    QMutexLocker locker(&m_mutex);
    m_running = false;

    if (m_levelCheckTimer)
    {
        m_levelCheckTimer->stop();
    }

    if (m_captureTimer)
    {
        m_captureTimer->stop();
    }

    cleanupAudio();
    emit captureStopped();
    LOG_INFO("AudioCaptureWorker stopped");
}

void AudioCaptureWorker::captureAudio()
{
    // 这个方法现在由processAudioData替代，保留以防直接调用
    processAudioData();
}

void AudioCaptureWorker::processAudioData()
{
    if (!m_running || !m_audioDevice)
        return;

    QByteArray data = m_audioDevice->readAll();
    if (data.isEmpty())
        return;

    // 检查音频电平
    double level = 0.0;
    const int16_t *samples = reinterpret_cast<const int16_t *>(data.constData());
    int sampleCount = data.size() / sizeof(int16_t);

    for (int i = 0; i < sampleCount; ++i)
    {
        level += abs(samples[i]);
    }
    level /= (sampleCount * 32768.0); // 归一化到0-1

    // 只有超过阈值才发送音频数据
    if (level > m_audioThreshold)
    {
        rtc::binary audioData;
        audioData.resize(data.size());
        std::memcpy(audioData.data(), data.constData(), data.size());

        emit audioFrameReady(audioData);
        LOG_DEBUG("Captured and sent audio frame: {}, level: {:.3f}", Convert::formatFileSize(data.size()), level);

        m_hasAudioActivity = true;
    }
    else if (m_hasAudioActivity)
    {
        // 音频活动停止
        m_hasAudioActivity = false;
        LOG_DEBUG("Audio activity stopped");
    }
}

void AudioCaptureWorker::checkAudioLevel()
{
    if (!m_running)
        return;

    // 现在音频电平检查在processAudioData中实时进行
    // 这个方法可以用于定期清理或状态检查

    if (m_audioInput)
    {
        QAudio::State state = m_audioInput->state();
        if (state == QAudio::StoppedState || state == QAudio::SuspendedState)
        {
            // 降低日志级别，避免过多警告输出
            static int warnCount = 0;
            if (warnCount++ % 100 == 0) { // 每100次只打印一次
                LOG_DEBUG("Audio input state is not active: {} (count: {})", static_cast<int>(state), warnCount);
            }
        }
    }
}

QAudioDeviceInfo AudioCaptureWorker::findSystemAudioDevice()
{
    QList<QAudioDeviceInfo> devices = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);

    LOG_INFO("Searching for system audio output capture device...");
    LOG_INFO("Available audio output devices:");

    // 查找各种可能的系统音频输出设备名称（优先级从高到低）
    QStringList systemAudioNames = {
        // 最常见的立体声混音设备
        "立体声混音", "Stereo Mix", "Stereo Mixer",
        // Windows 10/11 的新名称
        "What U Hear", "Wave Out Mix", "Line Out Mix",
        // 部分声卡驱动的命名
        "混音", "Mix", "Loopback", "Digital Output",
        // Realtek 声卡常见名称
        "Realtek Stereo Mix", "Realtek Digital Output",
        // 其他可能的系统音频设备
        "System Audio", "Desktop Audio", "Monitor", "Output Mix",
        // 扬声器设备（某些情况下可以作为录音设备）
        "Speaker", "Speakers", "扬声器", "音响", "耳机", "Headphones"};

    for (const QAudioDeviceInfo &device : devices)
    {
        QString deviceName = device.deviceName();
        LOG_INFO("  - {}", deviceName);

        for (const QString &name : systemAudioNames)
        {
            if (deviceName.contains(name, Qt::CaseInsensitive))
            {
                LOG_INFO("Found potential system audio device: {}", deviceName);
                return device;
            }
        }
    }

    LOG_WARN("No system audio capture device found!");
    LOG_WARN("To capture system audio on Windows:");
    LOG_WARN("1. Right-click sound icon in system tray");
    LOG_WARN("2. Select 'Open Sound settings' or 'Recording devices'");
    LOG_WARN("3. In Recording tab, right-click and show disabled devices");
    LOG_WARN("4. Enable 'Stereo Mix' or similar device");

    return QAudioDeviceInfo(); // 返回空设备
}

bool AudioCaptureWorker::initializeAudio()
{
    // 设置音频格式 (Qt5兼容)
    m_audioFormat.setSampleRate(m_sampleRate);
    m_audioFormat.setChannelCount(m_channels);
    m_audioFormat.setSampleSize(16);
    m_audioFormat.setCodec("audio/pcm");
    m_audioFormat.setByteOrder(QAudioFormat::LittleEndian);
    m_audioFormat.setSampleType(QAudioFormat::SignedInt);

    // 查找系统音频输出设备（立体声混音）
    QAudioDeviceInfo targetDevice = findSystemAudioDevice();

    // 如果没找到立体声混音，提供详细的解决方案和临时措施
    if (targetDevice.isNull())
    {
        LOG_ERROR("Cannot find system audio output capture device (Stereo Mix)!");
        LOG_ERROR("========== 系统音频捕获需要启用立体声混音 ==========");
        LOG_ERROR("Windows 10/11 启用立体声混音步骤：");
        LOG_ERROR("1. 右键点击任务栏音量图标");
        LOG_ERROR("2. 选择'打开声音设置' -> '声音控制面板'");
        LOG_ERROR("3. 切换到'录制'选项卡");
        LOG_ERROR("4. 在空白处右键，选择'显示禁用的设备'");
        LOG_ERROR("5. 找到'立体声混音'，右键启用并设为默认设备");
        LOG_ERROR("6. 如果没有立体声混音，请更新音频驱动程序");
        LOG_ERROR("================================================");
        
        // 尝试使用默认麦克风作为临时方案（会有警告）
        QAudioDeviceInfo defaultDevice = QAudioDeviceInfo::defaultInputDevice();
        if (!defaultDevice.isNull()) {
            LOG_WARN("临时使用默认输入设备: {}", defaultDevice.deviceName());
            LOG_WARN("注意：这将捕获麦克风而不是系统音频！");
            targetDevice = defaultDevice; // 使用默认设备
        } else {
            LOG_ERROR("没有任何可用的音频输入设备！");
            return false; // 完全失败
        }
    }

    // 检查设备是否支持所需的音频格式
    if (!targetDevice.isFormatSupported(m_audioFormat))
    {
        LOG_WARN("Requested audio format not supported, using nearest format");
        m_audioFormat = targetDevice.nearestFormat(m_audioFormat);
        LOG_INFO("Using audio format: {}Hz, {} channels",
                 m_audioFormat.sampleRate(), m_audioFormat.channelCount());
    }

    // 创建音频输入
    m_audioInput = new QAudioInput(targetDevice, m_audioFormat, this);

    // 创建音频缓冲区
    m_audioBuffer = new QBuffer(&m_audioData, this);
    m_audioBuffer->open(QIODevice::WriteOnly);

    m_audioInitialized = true;
    LOG_INFO("Audio capture initialized for device: {} ({}Hz, {} channels)",
             targetDevice.deviceName(),
             m_audioFormat.sampleRate(), m_audioFormat.channelCount());
    return true;
}

void AudioCaptureWorker::cleanupAudio()
{
    if (m_audioInput)
    {
        m_audioInput->stop();
        m_audioInput->deleteLater();
        m_audioInput = nullptr;
    }

    if (m_audioBuffer)
    {
        m_audioBuffer->close();
        m_audioBuffer->deleteLater();
        m_audioBuffer = nullptr;
    }

    m_audioDevice = nullptr;
    m_audioData.clear();
    m_audioInitialized = false;
    LOG_DEBUG("Audio capture cleaned up");
}

// MediaCapture实现
MediaCapture::MediaCapture(QObject *parent)
    : QObject(parent), m_isCapturing(false), m_isAudioCapturing(false), m_captureWorker(nullptr), m_audioCaptureWorker(nullptr), m_captureThread(nullptr), m_audioCaptureThread(nullptr), m_width(1920), m_height(1080), m_fps(10)
{
}

MediaCapture::~MediaCapture()
{
    stopCapture();
    stopAudioCapture();
}

void MediaCapture::startCapture(int width, int height, int fps)
{
    if (m_isCapturing)
    {
        stopCapture();
    }

    // 设置参数
    m_width = width;
    m_height = height;
    m_fps = std::max(1, std::min(fps, 60)); // 限制帧率在1-60之间

    // 创建工作线程
    m_captureThread = new QThread(this);

    // 创建工作对象
    m_captureWorker = new CaptureWorker();

    // 将工作对象移动到工作线程
    m_captureWorker->moveToThread(m_captureThread);

    // 连接信号和槽
    connect(this, &MediaCapture::startVideoCapture, m_captureWorker, &CaptureWorker::startCapture);
    connect(this, &MediaCapture::stopVideoCapture, m_captureWorker, &CaptureWorker::stopCapture);
    connect(this, &MediaCapture::requestKeyFrameSignal, m_captureWorker, &CaptureWorker::forceKeyFrame);
    connect(m_captureWorker, &CaptureWorker::frameReady, this, &MediaCapture::onCaptureFrameReady);

    // 当线程结束时清理工作对象
    connect(m_captureThread, &QThread::finished, m_captureWorker, &QObject::deleteLater);

    // 启动工作线程
    m_captureThread->start();

    m_isCapturing = true;

    // 启动捕获
    emit startVideoCapture(m_width, m_height, m_fps);
}

void MediaCapture::stopCapture()
{
    if (!m_isCapturing)
        return;

    m_isCapturing = false;

    if (m_captureWorker && m_captureThread)
    {
        // 断开信号连接防止回调到已销毁的对象
        disconnect(this, &MediaCapture::stopVideoCapture, m_captureWorker, &CaptureWorker::stopCapture);
        disconnect(m_captureWorker, &CaptureWorker::frameReady, this, &MediaCapture::onCaptureFrameReady);

        // 停止捕获
        QMetaObject::invokeMethod(m_captureWorker, "stopCapture", Qt::QueuedConnection);

        // 等待线程结束
        m_captureThread->quit();
        if (!m_captureThread->wait(3000))
        {
            LOG_WARN("Video capture thread did not quit gracefully, terminating");
            m_captureThread->terminate();
            m_captureThread->wait(1000);
        }

        m_captureThread = nullptr;
        m_captureWorker = nullptr; // 已经通过finished信号自动删除
    }
}

void MediaCapture::startAudioCapture(int sampleRate, int channels)
{
    if (m_isAudioCapturing)
    {
        stopAudioCapture();
    }

    // 创建工作线程
    m_audioCaptureThread = new QThread(this);

    // 创建工作对象
    m_audioCaptureWorker = new AudioCaptureWorker();

    // 将工作对象移动到工作线程
    m_audioCaptureWorker->moveToThread(m_audioCaptureThread);

    // 连接信号和槽
    connect(this, &MediaCapture::startAudioCaptureSignal, m_audioCaptureWorker, &AudioCaptureWorker::startCapture);
    connect(this, &MediaCapture::stopAudioCaptureSignal, m_audioCaptureWorker, &AudioCaptureWorker::stopCapture);
    connect(m_audioCaptureWorker, &AudioCaptureWorker::audioFrameReady, this, &MediaCapture::onAudioFrameReady);

    // 当线程结束时清理工作对象
    connect(m_audioCaptureThread, &QThread::finished, m_audioCaptureWorker, &QObject::deleteLater);

    // 启动工作线程
    m_audioCaptureThread->start();

    m_isAudioCapturing = true;

    // 启动音频捕获
    emit startAudioCaptureSignal(sampleRate, channels);
}

void MediaCapture::stopAudioCapture()
{
    if (!m_isAudioCapturing)
        return;

    m_isAudioCapturing = false;

    if (m_audioCaptureWorker && m_audioCaptureThread)
    {
        // 断开信号连接防止回调到已销毁的对象
        disconnect(this, &MediaCapture::stopAudioCaptureSignal, m_audioCaptureWorker, &AudioCaptureWorker::stopCapture);
        disconnect(m_audioCaptureWorker, &AudioCaptureWorker::audioFrameReady, this, &MediaCapture::onAudioFrameReady);

        // 停止音频捕获
        QMetaObject::invokeMethod(m_audioCaptureWorker, "stopCapture", Qt::QueuedConnection);

        // 等待线程结束
        m_audioCaptureThread->quit();
        if (!m_audioCaptureThread->wait(3000))
        {
            LOG_WARN("Audio capture thread did not quit gracefully, terminating");
            m_audioCaptureThread->terminate();
            m_audioCaptureThread->wait(1000);
        }

        m_audioCaptureThread = nullptr;
        m_audioCaptureWorker = nullptr; // 已经通过finished信号自动删除
    }
}

void MediaCapture::onCaptureFrameReady(const rtc::binary &h264Data)
{
    if (!m_isCapturing)
    {
        LOG_WARN("Received frame but not capturing, ignoring");
        return;
    }

    LOG_DEBUG("MediaCapture received H264 frame: {}", Convert::formatFileSize(h264Data.size()));

    // 直接发送H264数据，无需压缩
    emit videoFrameReady(h264Data);
}

void MediaCapture::onAudioFrameReady(const rtc::binary &audioData)
{
    if (!m_isAudioCapturing)
        return;

    // 直接转发音频数据
    emit audioFrameReady(audioData);
}

void MediaCapture::requestKeyFrame()
{
    if (m_isCapturing && m_captureWorker) {
        LOG_INFO("🔑 MediaCapture: Requesting key frame from capture worker");
        emit requestKeyFrameSignal();
    } else {
        LOG_WARN("Cannot request key frame - capture not active");
    }
}
