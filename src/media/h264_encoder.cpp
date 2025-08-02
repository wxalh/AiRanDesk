#include "h264_encoder.h"
#include "logger_manager.h"
#include <QDebug>

// 引用硬件设备上下文管理器（在h264_decoder.cpp中定义）
// 这样编码器和解码器可以共享硬件设备上下文，避免冲突
extern class HardwareContextManager& getHardwareContextManager();

// 由于C++的单例模式，我们需要在这里声明一个获取函数
class HardwareContextManager* getSharedHardwareManager();

class HardwareContextManager
{
public:
    static HardwareContextManager& instance() {
        static HardwareContextManager instance;
        return instance;
    }
    
    AVBufferRef* getDeviceContext(const QString& hwAccel) {
        QMutexLocker locker(&m_mutex);
        
        if (m_contexts.contains(hwAccel)) {
            AVBufferRef* ctx = m_contexts[hwAccel];
            if (ctx) {
                return av_buffer_ref(ctx);
            } else {
                m_contexts.remove(hwAccel);
            }
        }
        
        AVHWDeviceType deviceType = av_hwdevice_find_type_by_name(hwAccel.toUtf8().data());
        if (deviceType == AV_HWDEVICE_TYPE_NONE) {
            LOG_ERROR("Hardware device type not found: {}", hwAccel);
            return nullptr;
        }
        
        AVBufferRef* newCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&newCtx, deviceType, nullptr, nullptr, 0);
        if (ret < 0) {
            if (hwAccel == "qsv") {
                ret = av_hwdevice_ctx_create(&newCtx, deviceType, "auto", nullptr, 0);
            }
            
            if (ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_WARN("Failed to create shared hardware device context {}: {}", hwAccel, errbuf);
                return nullptr;
            }
        }
        
        m_contexts[hwAccel] = newCtx;
        LOG_DEBUG("Created shared hardware device context for: {}", hwAccel);
        return av_buffer_ref(newCtx);
    }
    
private:
    QMap<QString, AVBufferRef*> m_contexts;
    QMutex m_mutex;
};

H264Encoder::H264Encoder(QObject *parent)
    : QObject(parent)
    , m_codecContext(nullptr)
    , m_codec(nullptr)
    , m_frame(nullptr)
    , m_hwFrame(nullptr)
    , m_packet(nullptr)
    , m_swsContext(nullptr)
    , m_hwDeviceCtx(nullptr)
    , m_width(0)
    , m_height(0)
    , m_fps(30)
    , m_bitrate(2000000)
    , m_frameCount(0)  // 初始化帧计数
    , m_hwPixelFormat(AV_PIX_FMT_NONE)
    , m_initialized(false)
{
}

H264Encoder::~H264Encoder()
{
    cleanup();
}

QStringList H264Encoder::getAvailableHWAccels()
{
    QStringList hwAccels;
    
    // 检查常见的硬件加速器，按优先级排序
    const char* accelNames[] = {
        "qsv",        // Intel Quick Sync (优先检测)
        "nvenc",      // NVIDIA
        "amf",        // AMD
        "videotoolbox", // macOS
        nullptr
    };
    
    for (int i = 0; accelNames[i]; ++i) {
        QString codecName = QString("h264_%1").arg(accelNames[i]);
        const AVCodec* codec = avcodec_find_encoder_by_name(codecName.toUtf8().data());
        if (codec) {
            LOG_INFO("Found hardware encoder: {}", codecName);
            hwAccels << accelNames[i];
        } else {
            LOG_DEBUG("Hardware encoder not found: {}", codecName);
        }
    }
    
    return hwAccels;
}

bool H264Encoder::initialize(int width, int height, int fps, int bitrate)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_initialized) {
        cleanup();
    }
    
    m_width = width;
    m_height = height;
    m_fps = fps;
    m_bitrate = bitrate;
    
    // 优先尝试硬件加速
    QStringList hwAccels = getAvailableHWAccels();
    
    bool success = false;
    if (!hwAccels.isEmpty()) {
        for (const QString& hwAccel : hwAccels) {
            LOG_INFO("Trying hardware acceleration: {}", hwAccel);
            if (initializeCodec(hwAccel)) {
                LOG_INFO("Successfully initialized H264 encoder with {} acceleration", hwAccel);
                success = true;
                break;
            }
        }
    }
    
    // 如果硬件加速失败，使用软件编码
    if (!success) {
        LOG_INFO("Hardware acceleration not available, using software encoding");
        success = initializeCodec();
    }
    

    if (success) {
        m_initialized = true;
        QString accelType = m_hwAccelName.isEmpty() ? "software" : m_hwAccelName;
        LOG_INFO("🎯 H264 encoder successfully initialized with {} acceleration", accelType);

        // 性能优化提示
        if (m_hwAccelName.isEmpty()) {
            LOG_INFO("💡 Using optimized software encoding - consider upgrading GPU drivers for hardware acceleration");
        } else {
            LOG_INFO("🚀 Hardware acceleration active - optimal performance enabled");
        }
    } else {
        LOG_ERROR("❌ Failed to initialize H264 encoder with any method");
        cleanup();
    }
    
    return success;
}

bool H264Encoder::initializeCodec(const QString& hwAccel)
{
    // 查找编码器
    QString codecName = hwAccel.isEmpty() ? "libx264" : QString("h264_%1").arg(hwAccel);
    m_codec = avcodec_find_encoder_by_name(codecName.toUtf8().data());
    
    if (!m_codec) {
        LOG_ERROR("Codec {} not found", codecName);
        return false;
    }
    
    LOG_INFO("Found codec: {}", codecName);
    
    // 创建编码器上下文
    m_codecContext = avcodec_alloc_context3(m_codec);
    if (!m_codecContext) {
        LOG_ERROR("Could not allocate video codec context");
        return false;
    }
    
    // 设置编码参数
    m_codecContext->bit_rate = m_bitrate;
    m_codecContext->width = m_width;
    m_codecContext->height = m_height;
    m_codecContext->time_base = AVRational{1, m_fps};
    m_codecContext->framerate = AVRational{m_fps, 1};
    m_codecContext->gop_size = 30; // 强制更频繁的I帧（每30帧一个关键帧）
    m_codecContext->max_b_frames = 0; // 禁用B帧，简化编码
    m_codecContext->keyint_min = 10; // 最小关键帧间隔
    
    // 网络自适应优化：针对高延迟网络的编码参数
    m_codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY; // 低延迟模式
    m_codecContext->flags2 |= AV_CODEC_FLAG2_FAST; // 快速编码
    m_codecContext->slices = 4; // 使用多片编码，增强错误恢复能力
    
    // 设置编码预设和调优
    if (hwAccel.isEmpty()) {
        // 软件编码优化 - 重置硬件相关设置
        m_codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
        m_hwPixelFormat = AV_PIX_FMT_NONE;  // 确保不使用硬件传输
        m_hwDeviceCtx = nullptr;             // 确保不使用硬件设备
        LOG_INFO("Setting software encoding parameters: {}x{}, {}fps, {}bps", m_width, m_height, m_fps, m_bitrate);
        av_opt_set(m_codecContext->priv_data, "preset", "fast", 0);
        av_opt_set(m_codecContext->priv_data, "tune", "zerolatency", 0);
        // 网络自适应：增强错误恢复能力
        av_opt_set(m_codecContext->priv_data, "x264opts", "no-mbtree:sliced-threads:rc-lookahead=10", 0);
        // 注意：使用码率控制时不要设置crf
        // av_opt_set(m_codecContext->priv_data, "crf", "23", 0);
    } else {
        // 硬件加速初始化
        LOG_INFO("Setting hardware encoding parameters: {}x{}, {}fps, {}bps", m_width, m_height, m_fps, m_bitrate);
        if (!initializeHardwareAccel(hwAccel)) {
            return false;
        }
    }
    
    // 打开编码器
    int ret = avcodec_open2(m_codecContext, m_codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Could not open codec: {}", errbuf);
        return false;
    }
    
    // 分配帧 - QSV编码器的帧将在编码时动态分配
    m_frame = av_frame_alloc();
    if (!m_frame) {
        LOG_ERROR("Could not allocate video frame");
        return false;
    }
    
    // 对于QSV硬件编码，不在这里分配buffer，而是在编码时使用硬件帧
    if (hwAccel != "qsv") {
        m_frame->format = m_codecContext->pix_fmt;
        m_frame->width = m_codecContext->width;
        m_frame->height = m_codecContext->height;
        
        ret = av_frame_get_buffer(m_frame, 32);
        if (ret < 0) {
            LOG_ERROR("Could not allocate video frame data");
            return false;
        }
    }
    
    // 分配数据包
    m_packet = av_packet_alloc();
    if (!m_packet) {
        LOG_ERROR("Could not allocate packet");
        return false;
    }
    
    // 初始化图像格式转换器
    AVPixelFormat targetFormat = (hwAccel == "qsv") ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    
    // 注意：这里暂不创建SwsContext，在qimageToAVFrame中动态创建
    // 因为QSV的分辨率可能被对齐，需要在转换时确定正确的参数
    
    m_hwAccelName = hwAccel;
    return true;
}

bool H264Encoder::initializeHardwareAccel(const QString& hwAccel)
{
    if (hwAccel == "qsv") {
        // QSV特殊处理：使用简化初始化
        return initializeQSV();
    }
    
    // 其他硬件加速器的通用处理
    if (hwAccel == "nvenc") {
        // NVENC不需要硬件帧上下文，直接设置像素格式
        m_codecContext->pix_fmt = AV_PIX_FMT_YUV420P;  // NVENC可以接受软件格式
        m_hwPixelFormat = AV_PIX_FMT_NONE;  // 不使用硬件传输
        
        // NVENC特定选项
        av_opt_set(m_codecContext->priv_data, "preset", "fast", 0);
        av_opt_set(m_codecContext->priv_data, "profile", "high", 0);
        av_opt_set(m_codecContext->priv_data, "rc", "cbr", 0);  // 固定码率
        
        LOG_INFO("NVENC encoder configured with software input");
        return true;
        
    } else if (hwAccel == "amf") {
        // AMF也可以使用软件输入
        m_codecContext->pix_fmt = AV_PIX_FMT_YUV420P;  // AMF可以接受软件格式
        m_hwPixelFormat = AV_PIX_FMT_NONE;  // 不使用硬件传输
        
        // AMF特定选项
        av_opt_set(m_codecContext->priv_data, "usage", "lowlatency", 0);
        av_opt_set(m_codecContext->priv_data, "profile", "high", 0);
        av_opt_set(m_codecContext->priv_data, "rc", "cbr", 0);
        
        LOG_INFO("AMF encoder configured with software input");
        return true;
        
    } else if (hwAccel == "videotoolbox") {
        m_hwPixelFormat = AV_PIX_FMT_VIDEOTOOLBOX;
        m_codecContext->pix_fmt = AV_PIX_FMT_VIDEOTOOLBOX;
    } else {
        m_codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
        return true; // 其他硬件加速器可能不需要特殊设置
    }
    
    // 只有VideoToolbox需要硬件设备上下文
    if (hwAccel == "videotoolbox") {
        // 使用共享的硬件设备上下文管理器
        m_hwDeviceCtx = HardwareContextManager::instance().getDeviceContext("videotoolbox");
        if (!m_hwDeviceCtx) {
            LOG_ERROR("Failed to get shared VideoToolbox device context");
            return false;
        }
        
        m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        
        // 创建硬件帧上下文
        AVBufferRef* hwFramesRef = av_hwframe_ctx_alloc(m_hwDeviceCtx);
        if (!hwFramesRef) {
            LOG_ERROR("Failed to allocate hardware frames context");
            return false;
        }
        
        AVHWFramesContext* hwFramesCtx = (AVHWFramesContext*)hwFramesRef->data;
        hwFramesCtx->format = m_hwPixelFormat;
        hwFramesCtx->sw_format = AV_PIX_FMT_YUV420P;
        hwFramesCtx->width = m_width;
        hwFramesCtx->height = m_height;
        hwFramesCtx->initial_pool_size = 20;
        
        int ret = av_hwframe_ctx_init(hwFramesRef);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Failed to initialize hardware frames context: {}", errbuf);
            av_buffer_unref(&hwFramesRef);
            return false;
        }
        
        m_codecContext->hw_frames_ctx = hwFramesRef;
    }
    
    return true;
}

bool H264Encoder::initializeQSV()
{
    LOG_INFO("Initializing Intel QSV encoder with shared hardware context");
    
    // QSV要求分辨率必须是16的倍数，调整分辨率
    int alignedWidth = (m_width + 15) & ~15;   // 向上对齐到16的倍数
    int alignedHeight = (m_height + 15) & ~15; // 向上对齐到16的倍数
    
    if (alignedWidth != m_width || alignedHeight != m_height) {
        LOG_INFO("Aligning QSV resolution from {}x{} to {}x{}", m_width, m_height, alignedWidth, alignedHeight);
        m_codecContext->width = alignedWidth;
        m_codecContext->height = alignedHeight;
    }
    
    // 使用共享的硬件设备上下文管理器
    m_hwDeviceCtx = HardwareContextManager::instance().getDeviceContext("qsv");
    if (!m_hwDeviceCtx) {
        LOG_ERROR("Failed to get shared QSV device context");
        return false;
    }
    
    LOG_INFO("Successfully obtained shared QSV device context");
    
    // QSV特定设置必须在创建硬件帧上下文之前设置
    m_codecContext->pix_fmt = AV_PIX_FMT_QSV;  // 编码器使用QSV格式
    m_hwPixelFormat = AV_PIX_FMT_QSV;
    
    // 设置硬件设备上下文
    m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    
    // 创建硬件帧上下文
    AVBufferRef* hwFramesRef = av_hwframe_ctx_alloc(m_hwDeviceCtx);
    if (!hwFramesRef) {
        LOG_ERROR("Failed to allocate QSV frames context");
        return false;
    }
    
    AVHWFramesContext* hwFramesCtx = (AVHWFramesContext*)hwFramesRef->data;
    hwFramesCtx->format = AV_PIX_FMT_QSV;
    hwFramesCtx->sw_format = AV_PIX_FMT_NV12;
    hwFramesCtx->width = alignedWidth;   // 使用对齐后的尺寸
    hwFramesCtx->height = alignedHeight; // 使用对齐后的尺寸
    hwFramesCtx->initial_pool_size = 20;
    
    int ret = av_hwframe_ctx_init(hwFramesRef);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to initialize QSV frames context: {}", errbuf);
        av_buffer_unref(&hwFramesRef);
        return false;
    }
    
    m_codecContext->hw_frames_ctx = hwFramesRef;
    
    // QSV特定的编码器选项
    av_opt_set(m_codecContext->priv_data, "preset", "medium", 0);
    av_opt_set(m_codecContext->priv_data, "profile", "high", 0);
    
    // QSV特定参数调整
    m_codecContext->max_b_frames = 0;  // QSV可能不支持B帧，设置为0
    
    LOG_INFO("QSV encoder initialized successfully with shared context");
    return true;
}

rtc::binary H264Encoder::encodeFrame(const QImage& image)
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_initialized) {
        LOG_ERROR("Encoder not initialized");
        return rtc::binary();
    }
    
    // 确保图像格式为RGB888
    QImage rgbImage = image;
    if (rgbImage.format() != QImage::Format_RGB888) {
        rgbImage = rgbImage.convertToFormat(QImage::Format_RGB888);
    }
    
    // 缩放图像到编码器尺寸
    if (rgbImage.width() != m_width || rgbImage.height() != m_height) {
        rgbImage = rgbImage.scaled(m_width, m_height, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    
    // 转换为AVFrame
    AVFrame* inputFrame = qimageToAVFrame(rgbImage);
    if (!inputFrame) {
        LOG_ERROR("Failed to convert QImage to AVFrame");
        return rtc::binary();
    }
    
    AVFrame* encodingFrame = inputFrame;
    
    // 如果使用硬件加速，需要将软件帧传输到硬件
    if (m_hwPixelFormat != AV_PIX_FMT_NONE && m_hwDeviceCtx) {
        encodingFrame = transferToHardware(inputFrame);
        av_frame_free(&inputFrame);
        if (!encodingFrame) {
            LOG_ERROR("Failed to transfer frame to hardware");
            return rtc::binary();
        }
    }
    
    // 强制第一帧为关键帧，提高兼容性
    if (m_frameCount == 0) {
        encodingFrame->pict_type = AV_PICTURE_TYPE_I;  // 强制I帧
        encodingFrame->key_frame = 1;
        LOG_INFO("Forcing first frame as key frame");
    }
    
    // 编码帧
    int ret = avcodec_send_frame(m_codecContext, encodingFrame);
    
    // 增加帧计数
    m_frameCount++;
    
    av_frame_free(&encodingFrame);
    
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error sending frame to encoder: {}", errbuf);
        return rtc::binary();
    }
    
    rtc::binary result;
    
    // 接收编码后的数据包
    while (ret >= 0) {
        ret = avcodec_receive_packet(m_codecContext, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Error receiving packet from encoder: {}", errbuf);
            break;
        }
        
        // 确保数据包不为空
        if (m_packet->size > 0) {
            // 转换数据包为二进制数据
            rtc::binary packetData = avpacketToBinary(m_packet);
            result.insert(result.end(), packetData.begin(), packetData.end());
        } else {
            LOG_WARN("Received empty packet from encoder");
        }
        
        av_packet_unref(m_packet);
    }
    
    if (result.empty()) {
        LOG_DEBUG("No encoded data produced (encoder buffering)");
    }
    
    return result;
}

AVFrame* H264Encoder::qimageToAVFrame(const QImage& image)
{
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("Failed to allocate AVFrame");
        return nullptr;
    }
    
    // 对于QSV编码，创建软件帧，稍后传输到硬件
    AVPixelFormat targetFormat = AV_PIX_FMT_NV12;  // QSV推荐使用NV12
    if (m_hwAccelName != "qsv") {
        targetFormat = AV_PIX_FMT_YUV420P;  // 其他编码器使用YUV420P
    }
    
    frame->format = targetFormat;
    frame->width = m_width;    // 使用原始尺寸，不使用对齐后的尺寸
    frame->height = m_height;  // 使用原始尺寸，不使用对齐后的尺寸
    
    // 确保帧时间基准设置正确
    frame->pts = AV_NOPTS_VALUE;
    
    // 为帧分配缓冲区，使用32字节对齐
    int ret = av_frame_get_buffer(frame, 32);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Could not allocate video frame data: {}", errbuf);
        av_frame_free(&frame);
        return nullptr;
    }
    
    // RGB数据指针
    const uint8_t* srcData[1] = { image.constBits() };
    int srcLinesize[1] = { static_cast<int>(image.bytesPerLine()) };
    
    // 检查SwsContext是否有效，或者需要重新创建
    AVPixelFormat currentTargetFormat = (m_hwAccelName == "qsv") ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    if (!m_swsContext) {
        // 重新创建SwsContext以确保格式正确
        if (m_swsContext) {
            sws_freeContext(m_swsContext);
        }
        
        m_swsContext = sws_getContext(
            m_width, m_height, AV_PIX_FMT_RGB24,
            m_width, m_height, currentTargetFormat,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!m_swsContext) {
            LOG_ERROR("SwsContext creation failed for format conversion");
            av_frame_free(&frame);
            return nullptr;
        }
    }
    
    // 转换RGB到目标格式
    int swsRet = sws_scale(m_swsContext,
              srcData, srcLinesize, 0, m_height,
              frame->data, frame->linesize);
    
    if (swsRet != m_height) {
        LOG_ERROR("sws_scale failed: expected {} lines, got {}", m_height, swsRet);
        av_frame_free(&frame);
        return nullptr;
    }

    return frame;
}

rtc::binary H264Encoder::avpacketToBinary(AVPacket* packet)
{
    rtc::binary data;
    data.resize(packet->size);
    
    for (int i = 0; i < packet->size; ++i) {
        data[i] = static_cast<std::byte>(packet->data[i]);
    }
    
    // 调试：检查输出数据的起始码
    if (data.size() >= 4) {
        LOG_DEBUG("H264 packet: size={}, first 4 bytes: {:02x} {:02x} {:02x} {:02x}",
                 packet->size,
                 static_cast<uint8_t>(data[0]), static_cast<uint8_t>(data[1]),
                 static_cast<uint8_t>(data[2]), static_cast<uint8_t>(data[3]));
    }
    
    return data;
}

AVFrame* H264Encoder::transferToHardware(AVFrame* swFrame)
{
    if (!m_hwDeviceCtx || m_hwPixelFormat == AV_PIX_FMT_NONE) {
        // 不需要硬件传输，直接返回原帧
        return av_frame_clone(swFrame);
    }
    
    // 检查硬件帧上下文是否有效
    if (!m_codecContext || !m_codecContext->hw_frames_ctx) {
        LOG_WARN("Hardware frames context not available, falling back to software frame");
        return av_frame_clone(swFrame);
    }
    
    // 创建硬件帧
    AVFrame* hwFrame = av_frame_alloc();
    if (!hwFrame) {
        LOG_ERROR("Failed to allocate hardware frame");
        return nullptr;
    }
    
    // 为硬件帧分配缓冲区 - 必须先分配才能设置属性
    int ret = av_hwframe_get_buffer(m_codecContext->hw_frames_ctx, hwFrame, 0);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to allocate hardware frame buffer: {}", errbuf);
        av_frame_free(&hwFrame);
        return nullptr;
    }
    
    // 设置硬件帧属性（在分配缓冲区后）
    hwFrame->width = m_codecContext->width;   // 使用编码器的对齐尺寸
    hwFrame->height = m_codecContext->height; // 使用编码器的对齐尺寸
    
    // 如果软件帧尺寸与硬件帧尺寸不同，需要先缩放
    AVFrame* scaledFrame = swFrame;
    if (swFrame->width != hwFrame->width || swFrame->height != hwFrame->height) {
        scaledFrame = av_frame_alloc();
        if (!scaledFrame) {
            LOG_ERROR("Failed to allocate scaled frame");
            av_frame_free(&hwFrame);
            return nullptr;
        }
        
        scaledFrame->format = swFrame->format;
        scaledFrame->width = hwFrame->width;
        scaledFrame->height = hwFrame->height;
        
        ret = av_frame_get_buffer(scaledFrame, 32);
        if (ret < 0) {
            LOG_ERROR("Failed to allocate scaled frame buffer");
            av_frame_free(&hwFrame);
            av_frame_free(&scaledFrame);
            return nullptr;
        }
        
        // 创建临时的sws上下文进行缩放
        struct SwsContext* tempSwsCtx = sws_getContext(
            swFrame->width, swFrame->height, static_cast<AVPixelFormat>(swFrame->format),
            scaledFrame->width, scaledFrame->height, static_cast<AVPixelFormat>(scaledFrame->format),
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!tempSwsCtx) {
            LOG_ERROR("Failed to create temporary sws context for scaling");
            av_frame_free(&hwFrame);
            av_frame_free(&scaledFrame);
            return nullptr;
        }
        
        sws_scale(tempSwsCtx,
                  swFrame->data, swFrame->linesize, 0, swFrame->height,
                  scaledFrame->data, scaledFrame->linesize);
        
        sws_freeContext(tempSwsCtx);
    }
    
    // 传输数据到硬件帧
    ret = av_hwframe_transfer_data(hwFrame, scaledFrame, 0);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to transfer data to hardware frame: {}", errbuf);
        av_frame_free(&hwFrame);
        if (scaledFrame != swFrame) {
            av_frame_free(&scaledFrame);
        }
        return nullptr;
    }
    
    // 清理临时帧
    if (scaledFrame != swFrame) {
        av_frame_free(&scaledFrame);
    }
    
    return hwFrame;
}

void H264Encoder::cleanup()
{
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    
    if (m_hwFrame) {
        av_frame_free(&m_hwFrame);
        m_hwFrame = nullptr;
    }
    
    if (m_swsContext) {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }
    
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }
    
    // 释放硬件设备上下文的引用（共享管理器会处理实际的释放）
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }
    
    m_codec = nullptr;
    m_hwPixelFormat = AV_PIX_FMT_NONE;  // 重置硬件像素格式
    m_hwAccelName.clear();
    m_initialized = false;
    
    LOG_DEBUG("H264Encoder cleanup completed");
}
