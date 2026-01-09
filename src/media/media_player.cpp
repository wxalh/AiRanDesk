#include "media_player.h"
#include "logger_manager.h"
#include <QDebug>
#include <QThread>
#include <QApplication>
#include <QTimer>

// AudioPlayWorker 实现
AudioPlayWorker::AudioPlayWorker(QObject* parent)
    : QObject(parent), m_running(false), m_audioInitialized(false), 
      m_audioOutput(nullptr), m_audioDevice(nullptr), m_audioBuffer(nullptr),
      m_sampleRate(44100), m_channels(2)
{
    m_processTimer = new QTimer(this);
    m_processTimer->setInterval(10); // 10ms间隔
    connect(m_processTimer, &QTimer::timeout, this, &AudioPlayWorker::processAudio);
}

AudioPlayWorker::~AudioPlayWorker()
{
    stopPlayback();
    cleanupAudio();
}

void AudioPlayWorker::startPlayback()
{
    if (m_running) {
        return;
    }
    
    if (!initializeAudio()) {
        LOG_ERROR("Failed to initialize audio system");
        return;
    }
    
    m_running = true;
    m_processTimer->start();
    LOG_INFO("Audio playback started");
    emit playbackStarted();
}

void AudioPlayWorker::stopPlayback()
{
    if (!m_running) {
        return;
    }
    
    m_running = false;
    m_processTimer->stop();
    
    {
        QMutexLocker locker(&m_mutex);
        m_audioQueue.clear();
        m_waitCondition.wakeAll();
    }
    
    cleanupAudio();
    LOG_INFO("Audio playback stopped");
    emit playbackStopped();
}

void AudioPlayWorker::addAudioData(const rtc::binary& audioData)
{
    if (!m_running) {
        return;
    }
    
    QMutexLocker locker(&m_mutex);
    // 减少队列大小，防止缓冲区溢出
    if (m_audioQueue.size() < 5) { 
        m_audioQueue.enqueue(audioData);
    } else {
        // 丢弃最旧的数据，保持队列大小
        if (!m_audioQueue.isEmpty()) {
            m_audioQueue.dequeue();
        }
        m_audioQueue.enqueue(audioData);
        LOG_DEBUG("Audio buffer overflow, replaced oldest frame");
    }
}

void AudioPlayWorker::processAudio()
{
    if (!m_running) {
        return;
    }
    
    processAudioQueue();
    emit audioProcessed();
}

bool AudioPlayWorker::initializeAudio()
{
    // 设置音频格式 (Qt5兼容)
    m_audioFormat.setSampleRate(m_sampleRate);
    m_audioFormat.setChannelCount(m_channels);
    m_audioFormat.setSampleSize(16);
    m_audioFormat.setCodec("audio/pcm");
    m_audioFormat.setByteOrder(QAudioFormat::LittleEndian);
    m_audioFormat.setSampleType(QAudioFormat::SignedInt);

    // 获取默认音频输出设备
    QAudioDeviceInfo defaultDevice = QAudioDeviceInfo::defaultOutputDevice();
    if (defaultDevice.isNull()) {
        LOG_ERROR("No default audio output device available");
        return false;
    }
    LOG_INFO("Using audio output device: {}", defaultDevice.deviceName());
    // 检查设备是否支持所需的音频格式
    if (!defaultDevice.isFormatSupported(m_audioFormat)) {
        LOG_WARN("Requested audio format not supported, using nearest format");
        m_audioFormat = defaultDevice.nearestFormat(m_audioFormat);
        // 更新我们的参数以匹配实际格式
        m_sampleRate = m_audioFormat.sampleRate();
        m_channels = m_audioFormat.channelCount();
        LOG_INFO("Using audio format: {}Hz, {} channels", m_sampleRate, m_channels);
    }

    // 创建音频输出
    m_audioOutput = new QAudioOutput(defaultDevice, m_audioFormat, this);
    
    // 设置缓冲区大小和延迟
    m_audioOutput->setBufferSize(4096); // 4KB缓冲区

    // 启动音频输出
    m_audioDevice = m_audioOutput->start();
    if (!m_audioDevice) {
        LOG_ERROR("Failed to start audio output");
        cleanupAudio();
        return false;
    }

    m_audioInitialized = true;
    LOG_INFO("Qt Audio output initialized: {}Hz, {} channels, buffer: {}", 
            m_audioFormat.sampleRate(), m_audioFormat.channelCount(), 
            Convert::formatFileSize(m_audioOutput->bufferSize()));
    return true;
}

void AudioPlayWorker::cleanupAudio()
{
    if (m_audioOutput) {
        m_audioOutput->stop();
        m_audioOutput->deleteLater();
        m_audioOutput = nullptr;
    }
    
    if (m_audioBuffer) {
        m_audioBuffer->close();
        m_audioBuffer->deleteLater();
        m_audioBuffer = nullptr;
    }
    
    m_audioDevice = nullptr;
    m_playbackData.clear();
    m_audioInitialized = false;
    LOG_DEBUG("Audio playback cleaned up");
}

void AudioPlayWorker::processAudioQueue()
{
    QMutexLocker locker(&m_mutex);
    
    if (m_audioQueue.isEmpty() || !m_audioInitialized || !m_audioDevice) {
        return;
    }
    
    // 检查音频输出状态
    if (m_audioOutput) {
        QAudio::State state = m_audioOutput->state();
        if (state == QAudio::StoppedState || state == QAudio::SuspendedState) {
            LOG_WARN("Audio output is not active, state: {}", static_cast<int>(state));
            // 尝试重新启动
            if (state == QAudio::StoppedState) {
                m_audioDevice = m_audioOutput->start();
            }
            return;
        }
    }
    
    // 检查输出设备的可用空间
    qint64 freeBytes = m_audioOutput->bytesFree();
    
    // 处理队列中的音频数据
    while (!m_audioQueue.isEmpty() && freeBytes > 0) {
        rtc::binary audioData = m_audioQueue.dequeue();
        
        if (audioData.size() > 0 && audioData.size() <= freeBytes) {
            qint64 bytesWritten = m_audioDevice->write(
                reinterpret_cast<const char*>(audioData.data()), 
                audioData.size()
            );
            
            if (bytesWritten > 0) {
                LOG_DEBUG("Played audio data: {} written, {} free", 
                         Convert::formatFileSize(bytesWritten), Convert::formatFileSize(freeBytes));
                freeBytes -= bytesWritten;
            } else {
                LOG_WARN("Failed to write audio data to output device");
                // 将数据放回队列开头
                m_audioQueue.prepend(audioData);
                break;
            }
        } else if (audioData.size() > freeBytes) {
            // 如果数据太大，暂时放回队列
            m_audioQueue.prepend(audioData);
            LOG_DEBUG("Audio buffer full, waiting for space ({} needed, {} free)", 
                     Convert::formatFileSize(audioData.size()), Convert::formatFileSize(freeBytes));
            break;
        }
    }
}

void AudioPlayWorker::writeAudioData()
{
    // 这个方法可以用于定期检查并写入缓冲的音频数据
    processAudioQueue();
}

// MediaPlayer 实现
MediaPlayer::MediaPlayer(QObject *parent)
    : QObject{parent}, m_isPlaying(false), m_audioWorker(nullptr), m_audioThread(nullptr)
{
    // 创建工作线程
    m_audioThread = new QThread(this);
    
    // 创建工作对象
    m_audioWorker = new AudioPlayWorker();
    
    // 将工作对象移动到工作线程
    m_audioWorker->moveToThread(m_audioThread);
    
    // 连接信号和槽
    connect(this, &MediaPlayer::startAudioPlayback, m_audioWorker, &AudioPlayWorker::startPlayback);
    connect(this, &MediaPlayer::stopAudioPlayback, m_audioWorker, &AudioPlayWorker::stopPlayback);
    connect(this, &MediaPlayer::addAudioDataToWorker, m_audioWorker, &AudioPlayWorker::addAudioData);
    
    connect(m_audioWorker, &AudioPlayWorker::playbackStarted, this, [this]() {
        LOG_INFO("MediaPlayer: Audio playback started");
    });
    
    connect(m_audioWorker, &AudioPlayWorker::playbackStopped, this, [this]() {
        LOG_INFO("MediaPlayer: Audio playback stopped");
    });
    
    // 当线程结束时清理工作对象
    connect(m_audioThread, &QThread::finished, m_audioWorker, &QObject::deleteLater);
    
    // 启动工作线程
    m_audioThread->start();
}

MediaPlayer::~MediaPlayer()
{
    stopPlayback();
    
    // 停止线程
    if (m_audioThread && m_audioThread->isRunning()) {
        m_audioThread->quit();
        if (!m_audioThread->wait(3000)) {
            LOG_WARN("Audio thread did not finish gracefully, terminating");
            m_audioThread->terminate();
            m_audioThread->wait(1000);
        }
    }
}

void MediaPlayer::startPlayback()
{
    if (!m_isPlaying) {
        m_isPlaying = true;
        emit startAudioPlayback();
        LOG_INFO("MediaPlayer started");
    }
}

void MediaPlayer::stopPlayback()
{
    if (m_isPlaying) {
        m_isPlaying = false;
        emit stopAudioPlayback();
        LOG_INFO("MediaPlayer stopped");
    }
}

void MediaPlayer::playAudioData(const rtc::binary& audioData)
{
    if (m_isPlaying) {
        emit addAudioDataToWorker(audioData);
    }
}
