#ifndef H264_ENCODER_H
#define H264_ENCODER_H

#include <QImage>
#include <QMutex>
#include <QObject>
#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

class H264Encoder : public QObject {
  Q_OBJECT

public:
  explicit H264Encoder(QObject *parent = nullptr);
  ~H264Encoder();

  // 初始化编码器
  bool initialize(int width, int height, int fps = 30, int bitrate = 2000000);

  void forceKeyFrame();
  // 编码QImage为H264数据
  rtc::binary encodeFrame(const QImage &image);

  // 释放资源
  void cleanup();

  // 检查硬件加速支持
  static QStringList getAvailableHWAccels();

private:
  bool initializeCodec(const QString &hwAccel = QString());
  bool initializeHardwareAccel(const QString &hwAccel);
  bool initializeQSV(); // QSV专用初始化
  AVFrame *qimageToAVFrame(const QImage &image);
  AVFrame *transferToHardware(AVFrame *swFrame);
  rtc::binary avpacketToBinary(AVPacket *packet);

  // FFmpeg 组件
  AVCodecContext *m_codecContext;
  const AVCodec *m_codec;
  AVFrame *m_frame;
  AVFrame *m_hwFrame;
  AVPacket *m_packet;
  SwsContext *m_swsContext;
  AVBufferRef *m_hwDeviceCtx;

  // 编码参数
  int m_width;
  int m_height;
  int m_fps;
  int m_bitrate;

  // 编码状态
  int m_frameCount; // 已编码帧数

  // 线程安全
  QMutex m_mutex;

  // 硬件加速
  QString m_hwAccelName;
  enum AVPixelFormat m_hwPixelFormat;

  bool m_initialized;
  bool m_forceKeyFrame;
};

#endif // H264_ENCODER_H
