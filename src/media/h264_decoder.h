#ifndef H264_DECODER_H
#define H264_DECODER_H

#include <QObject>
#include <QImage>
#include <QMutex>
#include <memory>
#include <rtc/rtc.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/hwcontext.h>
}

#ifndef AV_ERROR_MAX_STRING_SIZE
#define AV_ERROR_MAX_STRING_SIZE 64
#endif

class H264Decoder : public QObject
{
    Q_OBJECT

public:
    explicit H264Decoder(QObject *parent = nullptr);
    ~H264Decoder();

    // 初始化解码器
    bool initialize(const QString& hwAccel = QString());
    
    // 解码H264数据为QImage
    QImage decodeFrame(const rtc::binary& h264Data);
    
    // 释放资源
    void cleanup();
    
    // 错误恢复
    void flushDecoder();          // 刷新解码器缓冲区
    void resetDecoder();          // 重置解码器状态
    bool isWaitingForKeyFrame();  // 检查是否在等待关键帧
    
    // 检查硬件加速支持
    static QStringList getAvailableHWAccels();
    
private:
    bool initializeCodec(const QString& hwAccel);
    bool initializeHardwareAccel(const QString& hwAccel);
    bool validateHardwareDecoding();  // 验证硬件解码是否真正工作
    QImage avframeToQImage(AVFrame* frame);
    AVFrame* convertToNV12(AVFrame* inputFrame);  // 转换任意格式到NV12
    
    // 硬件解码回调函数
    static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);
    
    // FFmpeg 组件
    AVCodecContext* m_codecContext;
    const AVCodec* m_codec;
    AVFrame* m_frame;
    AVFrame* m_swFrame;
    AVFrame* m_convertFrame;  // 用于格式转换的临时帧
    AVPacket* m_packet;
    SwsContext* m_swsContext;
    AVBufferRef* m_hwDeviceCtx;
    
    // 线程安全
    QMutex m_mutex;
    
    // 硬件加速
    QString m_hwAccelName;
    enum AVPixelFormat m_hwPixelFormat;
    
    bool m_initialized;
    
    // 错误恢复和状态管理
    bool m_waitingForKeyFrame;    // 是否在等待关键帧
    int m_consecutiveErrors;      // 连续错误计数
};

#endif // H264_DECODER_H
