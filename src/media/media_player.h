#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include <QQueue>
#include <QTimer>
#include <QAudioOutput>
#include <QAudioFormat>
#include <QAudioDeviceInfo>
#include <QIODevice>
#include <QBuffer>
#include <rtc/rtc.hpp>

// 音频播放工作者类（不继承QThread）
class AudioPlayWorker : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlayWorker(QObject* parent = nullptr);
    ~AudioPlayWorker();
    
public slots:
    void startPlayback();
    void stopPlayback();
    void addAudioData(const rtc::binary& audioData);
    void processAudio();
    
signals:
    void playbackStarted();
    void playbackStopped();
    void audioProcessed();
    
private:
    bool initializeAudio();
    void cleanupAudio();
    void processAudioQueue();
    void writeAudioData();
    
    bool m_running;
    bool m_audioInitialized;
    QMutex m_mutex;
    QWaitCondition m_waitCondition;
    QQueue<rtc::binary> m_audioQueue;
    QTimer* m_processTimer;
    
    // Qt Audio 相关
    QAudioOutput* m_audioOutput;
    QAudioFormat m_audioFormat;
    QIODevice* m_audioDevice;
    QBuffer* m_audioBuffer;
    QByteArray m_playbackData;
    
    // 音频参数
    int m_sampleRate;
    int m_channels;
};

class MediaPlayer : public QObject
{
    Q_OBJECT

public:
    explicit MediaPlayer(QObject *parent = nullptr);
    ~MediaPlayer();

    void startPlayback();
    void stopPlayback();
    void playAudioData(const rtc::binary& audioData);
    
    bool isPlaying() const { return m_isPlaying; }

private:
    bool m_isPlaying;
    AudioPlayWorker* m_audioWorker;
    QThread* m_audioThread;

signals:
    void audioFramePlayed();
    void playbackError(const QString& error);
    
    // 内部信号，用于线程间通信
    void startAudioPlayback();
    void stopAudioPlayback();
    void addAudioDataToWorker(const rtc::binary& audioData);
};

#endif // MEDIA_PLAYER_H
