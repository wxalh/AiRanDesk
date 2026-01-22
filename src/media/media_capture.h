#ifndef MEDIA_CAPTURE_H
#define MEDIA_CAPTURE_H

#include <QAudioDeviceInfo>
#include <QAudioFormat>
#include <QAudioInput>
#include <QAudioOutput>
#include <QBuffer>
#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>
#include <rtc/rtc.hpp>

class H264Encoder;

// 视频捕获工作者类（不继承QThread）
class CaptureWorker : public QObject {
  Q_OBJECT
public:
  explicit CaptureWorker(QObject *parent = nullptr);
  ~CaptureWorker();

public slots:
  void startCapture(int width, int height, int fps);
  void stopCapture();
  void captureFrame();                       // 定时器触发的捕获函数
  void setResolution(int width, int height); // 动态设置分辨率
  void setFps(int fps);                      // 动态设置帧率

signals:
  void frameReady(const rtc::binary &h264Data, quint64 timestamp_us);
  void captureStarted();
  void captureStopped();

private:
  std::pair<rtc::binary, quint64> captureScreenH264();
  bool m_running;
  int m_width;  // 编码器分辨率
  int m_height; // 编码器分辨率
  int m_fps;
  int m_screenWidth;  // 实际屏幕分辨率
  int m_screenHeight; // 实际屏幕分辨率
  QMutex m_mutex;
  QTimer *m_captureTimer;
  qint64 m_lastFrameTime; // 上一帧发送时间

  H264Encoder *m_encoder; // H264编码器
};

// 音频捕获工作者类（不继承QThread）
class AudioCaptureWorker : public QObject {
  Q_OBJECT
public:
  explicit AudioCaptureWorker(QObject *parent = nullptr);
  ~AudioCaptureWorker();

public slots:
  void startCapture(int sampleRate = 44100, int channels = 2);
  void stopCapture();

private slots:
  void captureAudio();
  void checkAudioLevel(); // 检查音频电平

signals:
  void audioFrameReady(const rtc::binary &audioData);
  void captureStarted();
  void captureStopped();

private:
  bool initializeAudio();
  void cleanupAudio();
  void processAudioData();                  // 处理音频输入数据
  QAudioDeviceInfo findSystemAudioDevice(); // 查找系统音频设备

  bool m_running;
  int m_sampleRate;
  int m_channels;
  QMutex m_mutex;
  QTimer *m_captureTimer;
  QTimer *m_levelCheckTimer; // 用于检查音频电平

  // Qt音频相关
  QAudioInput *m_audioInput;
  QAudioFormat m_audioFormat;
  QIODevice *m_audioDevice;
  QBuffer *m_audioBuffer;
  QByteArray m_audioData;

  // 音频状态
  bool m_audioInitialized;
  bool m_hasAudioActivity; // 是否有音频活动
  double m_audioThreshold; // 音频检测阈值
};

class MediaCapture : public QObject {
  Q_OBJECT

public:
  explicit MediaCapture(QObject *parent = nullptr);
  ~MediaCapture();

  void startCapture(int width = 1920, int height = 1080, int fps = 10);
  void stopCapture();
  bool isCapturing() const { return m_isCapturing; }

  // 动态设置分辨率和帧率
  void setResolution(int width, int height);
  void setFps(int fps);

  // 启动音频捕获
  void startAudioCapture(int sampleRate = 44100, int channels = 2);
  void stopAudioCapture();
  bool isAudioCapturing() const { return m_isAudioCapturing; }

private slots:
  void onCaptureFrameReady(const rtc::binary &h264Data, quint64 timestamp_us);
  void onAudioFrameReady(const rtc::binary &audioData);

private:
  bool m_isCapturing;
  bool m_isAudioCapturing;

  // 工作者对象和线程
  CaptureWorker *m_captureWorker;
  AudioCaptureWorker *m_audioCaptureWorker;
  QThread *m_captureThread;
  QThread *m_audioCaptureThread;

  int m_width;
  int m_height;
  int m_fps;

signals:
  void videoFrameReady(const rtc::binary &h264Data, quint64 timestamp_us);
  void audioFrameReady(const rtc::binary &audioData);

  // 内部信号，用于线程间通信
  void startVideoCapture(int width, int height, int fps);
  void stopVideoCapture();
  void startAudioCaptureSignal(int sampleRate, int channels);
  void stopAudioCaptureSignal();
  void frameReceived();
  void setResolutionSignal(int width,
                           int height); // 内部信号，传递分辨率设置到工作线程
  void setFpsSignal(int fps);           // 内部信号，传递帧率设置到工作线程
};

#endif // MEDIA_CAPTURE_H
