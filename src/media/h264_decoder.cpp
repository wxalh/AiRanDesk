#include "h264_decoder.h"
#include "logger_manager.h"
#include <QDebug>
#include <QMap>
#include <QMutex>

// 硬件设备上下文管理器 - 单例模式，避免重复创建硬件上下文
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
                // 增加引用计数并返回
                return av_buffer_ref(ctx);
            } else {
                // 清理无效的上下文
                m_contexts.remove(hwAccel);
            }
        }
        
        // 创建新的硬件设备上下文
        AVHWDeviceType deviceType = av_hwdevice_find_type_by_name(hwAccel.toUtf8().data());
        if (deviceType == AV_HWDEVICE_TYPE_NONE) {
            LOG_ERROR("Hardware device type not found: {}", hwAccel);
            return nullptr;
        }
        
        AVBufferRef* newCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&newCtx, deviceType, nullptr, nullptr, 0);
        if (ret < 0) {
            // 对于QSV，尝试使用"auto"参数
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
        
        // 缓存上下文
        m_contexts[hwAccel] = newCtx;
        LOG_DEBUG("Created shared hardware device context for: {}", hwAccel);
        
        // 返回新的引用
        return av_buffer_ref(newCtx);
    }
    
    void cleanup() {
        QMutexLocker locker(&m_mutex);
        for (auto it = m_contexts.begin(); it != m_contexts.end(); ++it) {
            if (it.value()) {
                av_buffer_unref(&it.value());
            }
        }
        m_contexts.clear();
        LOG_DEBUG("Cleared all shared hardware device contexts");
    }
    
private:
    QMap<QString, AVBufferRef*> m_contexts;
    QMutex m_mutex;
    
    HardwareContextManager() = default;
    ~HardwareContextManager() {
        cleanup();
    }
};

H264Decoder::H264Decoder(QObject *parent)
    : QObject(parent)
    , m_codecContext(nullptr)
    , m_codec(nullptr)
    , m_frame(nullptr)
    , m_swFrame(nullptr)
    , m_convertFrame(nullptr)
    , m_packet(nullptr)
    , m_swsContext(nullptr)
    , m_hwDeviceCtx(nullptr)
    , m_hwPixelFormat(AV_PIX_FMT_NONE)
    , m_initialized(false)
    , m_waitingForKeyFrame(true)
    , m_consecutiveErrors(0)
    , m_lastGoodFrameTimestamp(0)
{
}

H264Decoder::~H264Decoder()
{
    cleanup();
}

QStringList H264Decoder::getAvailableHWAccels()
{
    QStringList hwAccels;

    // 检查硬件设备类型支持，而不是特定的解码器
    const char* deviceTypes[] = {
        "qsv",        // Intel Quick Sync (优先检测)
        "cuda",       // NVIDIA CUDA
        "dxva2",      // Windows DirectX
        "d3d11va",    // Windows Direct3D 11
        "videotoolbox", // macOS
        "v4l2m2m",      // Linux V4L2
        "omx",          // OpenMAX
        "rkmpp",       // Rockchip MPP
        "mpp",         // MPP
        "mppenc",     // MPP Encoder
        nullptr
    };
    
    for (int i = 0; deviceTypes[i]; ++i) {
        AVHWDeviceType type = av_hwdevice_find_type_by_name(deviceTypes[i]);
        if (type != AV_HWDEVICE_TYPE_NONE) {
            // 尝试创建设备上下文来验证支持
            AVBufferRef* testCtx = nullptr;
            int ret = av_hwdevice_ctx_create(&testCtx, type, nullptr, nullptr, 0);
            if (ret >= 0) {
                LOG_INFO("Found supported hardware device: {}", deviceTypes[i]);
                hwAccels << deviceTypes[i];
                av_buffer_unref(&testCtx);
            } else {
                // 对于QSV，尝试"auto"参数
                if (strcmp(deviceTypes[i], "qsv") == 0) {
                    ret = av_hwdevice_ctx_create(&testCtx, type, "auto", nullptr, 0);
                    if (ret >= 0) {
                        LOG_INFO("Found supported hardware device: {} (with auto)", deviceTypes[i]);
                        hwAccels << deviceTypes[i];
                        av_buffer_unref(&testCtx);
                    } else {
                        char errbuf[AV_ERROR_MAX_STRING_SIZE];
                        av_strerror(ret, errbuf, sizeof(errbuf));
                        LOG_DEBUG("Hardware device not supported: {} - {}", deviceTypes[i], errbuf);
                    }
                } else {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_DEBUG("Hardware device not supported: {} - {}", deviceTypes[i], errbuf);
                }
            }
        } else {
            LOG_DEBUG("Hardware device type not found: {}", QString::fromUtf8(deviceTypes[i]));
        }
    }

    return hwAccels;
}

bool H264Decoder::initialize(const QString& hwAccel)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_initialized) {
        cleanup();
    }
    
    // 智能自适应硬件加速选择策略
    bool success = false;
    if (!hwAccel.isEmpty()) {
        LOG_INFO("Attempting to initialize H264 decoder with {} acceleration", hwAccel);
        success = initializeCodec(hwAccel);
    } else {
        QStringList hwAccels = getAvailableHWAccels();
        LOG_INFO("Available hardware decoders: {}", hwAccels.join(", "));
        if (!hwAccels.isEmpty()) {
            // 多层次自适应策略：按硬件类型和兼容性排序
            QStringList adaptiveOrder;
            
            // 第一优先级：专用硬件加速器（最高效）
            if (hwAccels.contains("cuda")) adaptiveOrder << "cuda";          // NVIDIA专用
            
            // 第二优先级：通用DirectX硬件加速（兼容性好）
            if (hwAccels.contains("d3d11va")) adaptiveOrder << "d3d11va";    // 现代DirectX
            if (hwAccels.contains("dxva2")) adaptiveOrder << "dxva2";        // 传统DirectX
            
            // 第三优先级：厂商特定加速器（可能有兼容性问题）
            if (hwAccels.contains("qsv")) adaptiveOrder << "qsv";            // Intel QSV
            
            LOG_INFO("Adaptive hardware acceleration order: {}", adaptiveOrder.join(" -> "));
            
            // 逐一尝试硬件加速器
            for (const QString& hwType : adaptiveOrder) {
                LOG_INFO("Attempting hardware acceleration: {}", hwType);
                if (initializeCodec(hwType)) {
                    LOG_INFO("✓ Successfully initialized H264 decoder with {} hardware acceleration", hwType);
                    success = true;
                    break;
                } else {
                    LOG_WARN("✗ Failed to initialize {} hardware acceleration, trying next", hwType);
                }
            }
        }
    }
    
    // 自适应回退策略：如果所有硬件加速都失败，使用高效的软件解码
    if (!success) {
        LOG_WARN("All hardware acceleration failed, falling back to optimized software decoding");
        success = initializeCodec(QString());
    }
    
    if (success) {
        m_initialized = true;
        QString accelType = m_hwAccelName.isEmpty() ? "software" : m_hwAccelName;
        LOG_INFO("🎯 H264 decoder successfully initialized with {} acceleration", accelType);
        
        // 性能优化提示
        if (m_hwAccelName.isEmpty()) {
            LOG_INFO("💡 Using optimized software decoding - consider upgrading GPU drivers for hardware acceleration");
        } else {
            LOG_INFO("🚀 Hardware acceleration active - optimal performance enabled");
        }
    } else {
        LOG_ERROR("❌ Failed to initialize H264 decoder with any method");
        cleanup();
    }
    
    return success;
}

bool H264Decoder::initializeCodec(const QString& hwAccel)
{
    // 查找解码器 - 对于硬件解码，使用标准h264解码器而不是特定的硬件解码器
    QString codecName = "h264";  // 始终使用标准h264解码器
    m_codec = avcodec_find_decoder_by_name(codecName.toUtf8().data());
    
    if (!m_codec) {
        LOG_ERROR("Codec {} not found", codecName);
        return false;
    }
    
    LOG_DEBUG("Found codec: {}", codecName);
    
    // 创建解码器上下文
    m_codecContext = avcodec_alloc_context3(m_codec);
    if (!m_codecContext) {
        LOG_ERROR("Could not allocate video codec context");
        return false;
    }
    
    // 智能硬件加速初始化
    bool hardwareInitialized = false;
    if (!hwAccel.isEmpty()) {
        LOG_DEBUG("Setting hardware decoding parameters for: {}", hwAccel);
        if (initializeHardwareAccel(hwAccel)) {
            // 设置get_format回调函数，这是硬件解码的关键
            m_codecContext->get_format = get_hw_format;
            m_codecContext->opaque = this;  // 传递this指针给回调函数
            hardwareInitialized = true;
            LOG_DEBUG("Hardware acceleration setup completed for: {}", hwAccel);
        } else {
            LOG_WARN("Hardware acceleration setup failed for: {}", hwAccel);
        }
    }
    
    // 打开解码器（硬件或软件）
    int ret = avcodec_open2(m_codecContext, m_codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        
        if (hardwareInitialized) {
            LOG_WARN("Hardware decoder failed to open ({}): {}", hwAccel, errbuf);
            LOG_INFO("Attempting graceful fallback to software decoding");
            
            // 清理硬件资源
            avcodec_free_context(&m_codecContext);
            if (m_hwDeviceCtx) {
                av_buffer_unref(&m_hwDeviceCtx);
                m_hwDeviceCtx = nullptr;
            }
            m_hwPixelFormat = AV_PIX_FMT_NONE;
            
            // 重新创建软件解码器上下文
            m_codecContext = avcodec_alloc_context3(m_codec);
            if (!m_codecContext) {
                LOG_ERROR("Could not allocate software video codec context");
                return false;
            }
            
            ret = avcodec_open2(m_codecContext, m_codec, nullptr);
            if (ret < 0) {
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Software decoder also failed: {}", errbuf);
                return false;
            }
            
            LOG_INFO("✓ Graceful fallback to software decoding successful");
            m_hwAccelName.clear();  // 清除硬件加速标识
        } else {
            LOG_ERROR("Software decoder failed to open: {}", errbuf);
            return false;
        }
    } else {
        if (hardwareInitialized) {
            LOG_DEBUG("✓ Hardware decoder opened successfully: {}", hwAccel);
        } else {
            LOG_DEBUG("✓ Software decoder opened successfully");
        }
    }
    
    // 分配帧
    m_frame = av_frame_alloc();
    if (!m_frame) {
        LOG_ERROR("Could not allocate video frame");
        return false;
    }
    
    // 如果使用硬件加速，分配软件帧用于转换
    if (!hwAccel.isEmpty()) {
        m_swFrame = av_frame_alloc();
        if (!m_swFrame) {
            LOG_ERROR("Could not allocate software frame");
            return false;
        }
    }
    
    // 分配数据包
    m_packet = av_packet_alloc();
    if (!m_packet) {
        LOG_ERROR("Could not allocate packet");
        return false;
    }
    
    m_hwAccelName = hwAccel;
    LOG_INFO("Decoder initialization completed for: {}", hwAccel.isEmpty() ? "software" : hwAccel);
    return true;
}

bool H264Decoder::initializeHardwareAccel(const QString& hwAccel)
{
    // 根据不同的硬件加速器设置像素格式
    if (hwAccel == "cuda") {
        m_hwPixelFormat = AV_PIX_FMT_CUDA;
    } else if (hwAccel == "qsv") {
        // QSV通过DirectX接口工作，优先使用D3D11格式
        // 注意：这里设置一个期望的格式，实际格式由get_hw_format回调决定
        m_hwPixelFormat = AV_PIX_FMT_D3D11;
        LOG_INFO("QSV will use DirectX interfaces (D3D11/DXVA2) for hardware acceleration");
    } else if (hwAccel == "dxva2") {
        m_hwPixelFormat = AV_PIX_FMT_DXVA2_VLD;
    } else if (hwAccel == "d3d11va") {
        m_hwPixelFormat = AV_PIX_FMT_D3D11;
    } else if (hwAccel == "videotoolbox") {
        m_hwPixelFormat = AV_PIX_FMT_VIDEOTOOLBOX;
    } else if (hwAccel == "rkmpp") {
        // Rockchip MPP 通常通过 DRM PRIME 输出硬件帧
        // 实际可用格式由 get_hw_format 从 pix_fmts 决策。
        m_hwPixelFormat = AV_PIX_FMT_DRM_PRIME;
        LOG_INFO("RKMPP hardware acceleration expected format: {}", av_get_pix_fmt_name(m_hwPixelFormat));
    } else {
        LOG_WARN("Unknown hardware accelerator: {}", hwAccel);
        return false;
    }

    LOG_INFO("Setting initial hardware pixel format: {} for {}", av_get_pix_fmt_name(m_hwPixelFormat), hwAccel);

    // 使用共享的硬件设备上下文管理器
    LOG_INFO("Getting shared hardware device context for: {}", hwAccel);
    m_hwDeviceCtx = HardwareContextManager::instance().getDeviceContext(hwAccel);

    if (!m_hwDeviceCtx) {
        LOG_ERROR("Failed to get hardware device context for: {}", hwAccel);
        return false;
    }

    LOG_INFO("Successfully obtained hardware device context for {}", hwAccel);

    // 将硬件设备上下文分配给解码器上下文
    m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
    LOG_INFO("Successfully assigned hardware device context to decoder");
    return true;
}

QImage H264Decoder::decodeFrame(const rtc::binary& h264Data)
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_initialized) {
        LOG_ERROR("Decoder not initialized");
        return QImage();
    }
    
    if (h264Data.empty()) {
        return QImage();
    }
    
    // 设置数据包
    m_packet->data = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(h264Data.data()));
    m_packet->size = static_cast<int>(h264Data.size());
    
    // 发送数据包到解码器
    int ret = avcodec_send_packet(m_codecContext, m_packet);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error sending packet to decoder: {}", errbuf);
        return QImage();
    }
    
    // 接收解码后的帧
    ret = avcodec_receive_frame(m_codecContext, m_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        return QImage(); // 需要更多数据或结束
    } else if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error receiving frame from decoder: {}", errbuf);
        return QImage();
    }
    
    // 如果是硬件帧，需要转换到系统内存
    AVFrame* frameToConvert = m_frame;
    AVPixelFormat frameFormat = static_cast<AVPixelFormat>(m_frame->format);

    // 检查是否是硬件格式（任何非软件格式都需要转换）
    bool isHardwareFrame = (frameFormat != AV_PIX_FMT_YUV420P &&
                           frameFormat != AV_PIX_FMT_YUV422P &&
                           frameFormat != AV_PIX_FMT_YUV444P &&
                           frameFormat != AV_PIX_FMT_NV12 &&
                           frameFormat != AV_PIX_FMT_NV21) ||
                          (!m_hwAccelName.isEmpty() && frameFormat != AV_PIX_FMT_YUV420P);

    if (isHardwareFrame && m_swFrame && !m_hwAccelName.isEmpty()) {
        // RKMPP/DRM_PRIME 在不同 FFmpeg 版本上对直接 transfer 到 NV12 的支持不一致。
        // 这里优先选择更通用的 YUV420P 作为软帧目标，后续再统一转 NV12/RGB。
        AVPixelFormat transferTarget = AV_PIX_FMT_NV12;
        if (frameFormat == AV_PIX_FMT_DRM_PRIME || m_hwAccelName == "rkmpp") {
            transferTarget = AV_PIX_FMT_YUV420P;
        }

        LOG_DEBUG("Detected hardware frame format: {}, transferring to software {}", 
                  av_get_pix_fmt_name(frameFormat), av_get_pix_fmt_name(transferTarget));

        m_swFrame->format = transferTarget;
        m_swFrame->width = m_frame->width;
        m_swFrame->height = m_frame->height;

        // 为软件帧分配缓冲区 - 重用现有缓冲区或分配新的
        if (m_swFrame->buf[0]) {
            int required_size = av_image_get_buffer_size(transferTarget,
                                                       m_swFrame->width, m_swFrame->height, 32);
            if (m_swFrame->buf[0]->size >= required_size) {
                av_frame_unref(m_swFrame);
                m_swFrame->format = transferTarget;
                m_swFrame->width = m_frame->width;
                m_swFrame->height = m_frame->height;
                LOG_DEBUG("Reusing existing software frame buffer");
            } else {
                av_frame_unref(m_swFrame);
                m_swFrame->format = transferTarget;
                m_swFrame->width = m_frame->width;
                m_swFrame->height = m_frame->height;
                int ret2 = av_frame_get_buffer(m_swFrame, 32);
                if (ret2 < 0) {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret2, errbuf, sizeof(errbuf));
                    LOG_ERROR("Error allocating software frame buffer: {}", errbuf);
                    return QImage();
                }
                LOG_DEBUG("Allocated new software frame buffer");
            }
        } else {
            int ret2 = av_frame_get_buffer(m_swFrame, 32);
            if (ret2 < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE];
                av_strerror(ret2, errbuf, sizeof(errbuf));
                LOG_ERROR("Error allocating software frame buffer: {}", errbuf);
                return QImage();
            }
            LOG_DEBUG("Allocated initial software frame buffer");
        }

        int ret2 = av_hwframe_transfer_data(m_swFrame, m_frame, 0);
        if (ret2 < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret2, errbuf, sizeof(errbuf));
            LOG_ERROR("Error transferring frame data from hardware: {}", errbuf);
            return QImage();
        }

        LOG_DEBUG("Successfully transferred hardware frame to software {} format", av_get_pix_fmt_name(transferTarget));
        frameToConvert = m_swFrame;
    } else {
        LOG_DEBUG("Using software frame format: {}", av_get_pix_fmt_name(frameFormat));
        // 如果是软件解码但不是NV12格式，需要转换为NV12统一处理
        if (frameFormat != AV_PIX_FMT_NV12) {
            frameToConvert = convertToNV12(m_frame);
            if (!frameToConvert) {
                LOG_WARN("Failed to convert software frame to NV12, using original format");
                frameToConvert = m_frame;
            }
        } else {
            frameToConvert = m_frame;
        }
    }
    
    // 转换为QImage
    QImage result = avframeToQImage(frameToConvert);
    
    // 清理数据包引用
    av_packet_unref(m_packet);
    
    // 清理帧数据引用（保留帧对象用于重用）
    av_frame_unref(m_frame);
    if (m_swFrame && frameToConvert == m_swFrame) {
        // 如果使用了软件帧，也清理其引用
        av_frame_unref(m_swFrame);
    }
    
    // 清理转换产生的临时帧
    if (frameToConvert != m_frame && frameToConvert != m_swFrame) {
        av_frame_free(&frameToConvert);
    }
    
    return result;
}

AVFrame* H264Decoder::convertToNV12(AVFrame* inputFrame)
{
    if (!inputFrame) {
        return nullptr;
    }
    
    AVPixelFormat inputFormat = static_cast<AVPixelFormat>(inputFrame->format);
    if (inputFormat == AV_PIX_FMT_NV12) {
        // 已经是NV12格式，直接返回
        return inputFrame;
    }
    
    // 创建转换帧
    AVFrame* nv12Frame = av_frame_alloc();
    if (!nv12Frame) {
        LOG_ERROR("Failed to allocate NV12 conversion frame");
        return nullptr;
    }
    
    nv12Frame->format = AV_PIX_FMT_NV12;
    nv12Frame->width = inputFrame->width;
    nv12Frame->height = inputFrame->height;
    
    int ret = av_frame_get_buffer(nv12Frame, 32);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to allocate NV12 frame buffer: {}", errbuf);
        av_frame_free(&nv12Frame);
        return nullptr;
    }
    
    // 创建转换上下文
    struct SwsContext* swsCtx = sws_getContext(
        inputFrame->width, inputFrame->height, inputFormat,
        nv12Frame->width, nv12Frame->height, AV_PIX_FMT_NV12,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );
    
    if (!swsCtx) {
        LOG_ERROR("Failed to create sws context for NV12 conversion");
        av_frame_free(&nv12Frame);
        return nullptr;
    }
    
    // 执行转换
    int scaledHeight = sws_scale(swsCtx,
                                inputFrame->data, inputFrame->linesize, 0, inputFrame->height,
                                nv12Frame->data, nv12Frame->linesize);
    
    sws_freeContext(swsCtx);
    
    if (scaledHeight != inputFrame->height) {
        LOG_ERROR("Failed to convert frame to NV12: expected {} lines, got {}", inputFrame->height, scaledHeight);
        av_frame_free(&nv12Frame);
        return nullptr;
    }
    
    LOG_DEBUG("Successfully converted {} frame to NV12", av_get_pix_fmt_name(inputFormat));
    return nv12Frame;
}

QImage H264Decoder::avframeToQImage(AVFrame* frame)
{
    if (!frame) {
        return QImage();
    }
    
    int width = frame->width;
    int height = frame->height;
    AVPixelFormat inputFormat = static_cast<AVPixelFormat>(frame->format);
    
    // 对于某些硬件格式，先转换为标准格式
    AVFrame* frameToUse = frame;
    
    // 检查是否需要中间转换（某些FFmpeg版本对NV12->RGB24的sws_scale支持有问题）
    if (inputFormat == AV_PIX_FMT_NV12) {
        // 使用成员变量重用转换帧
        if (!m_convertFrame) {
            m_convertFrame = av_frame_alloc();
            if (!m_convertFrame) {
                LOG_ERROR("Failed to allocate convert frame");
                return QImage();
            }
        } else {
            av_frame_unref(m_convertFrame); // 清理之前的数据
        }
        
        m_convertFrame->format = AV_PIX_FMT_YUV420P;
        m_convertFrame->width = width;
        m_convertFrame->height = height;
        
        // 只在需要时重新分配缓冲区
        if (!m_convertFrame->buf[0] || 
            m_convertFrame->buf[0]->size < av_image_get_buffer_size(AV_PIX_FMT_YUV420P, width, height, 32)) {
            int ret = av_frame_get_buffer(m_convertFrame, 32);
            if (ret < 0) {
                LOG_ERROR("Failed to allocate convert frame buffer");
                return QImage();
            }
        }
        
        // 创建临时sws上下文进行NV12->YUV420P转换
        struct SwsContext* tempSwsCtx = sws_getContext(
            width, height, AV_PIX_FMT_NV12,
            width, height, AV_PIX_FMT_YUV420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (tempSwsCtx) {
            sws_scale(tempSwsCtx,
                      frame->data, frame->linesize, 0, height,
                      m_convertFrame->data, m_convertFrame->linesize);
            sws_freeContext(tempSwsCtx);
            frameToUse = m_convertFrame;
            inputFormat = AV_PIX_FMT_YUV420P;
        } else {
            LOG_WARN("Failed to create NV12->YUV420P converter, using direct conversion");
            frameToUse = frame;
            inputFormat = static_cast<AVPixelFormat>(frame->format);
        }
    }
    
    // 创建或更新图像格式转换器
    if (!m_swsContext || 
        sws_isSupportedInput(inputFormat) <= 0) {
        
        if (m_swsContext) {
            sws_freeContext(m_swsContext);
        }
        
        m_swsContext = sws_getContext(
            width, height, inputFormat,
            width, height, AV_PIX_FMT_RGB24,
            SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        
        if (!m_swsContext) {
            LOG_ERROR("Could not initialize sws context for format {}", av_get_pix_fmt_name(inputFormat));
            return QImage();
        }
    }
    
    // 创建QImage
    QImage image(width, height, QImage::Format_RGB888);
    
    // 设置目标数据指针
    uint8_t* dstData[1] = { image.bits() };
    int dstLinesize[1] = { static_cast<int>(image.bytesPerLine()) };
    
    // 转换YUV到RGB
    int result = sws_scale(m_swsContext,
              frameToUse->data, frameToUse->linesize, 0, height,
              dstData, dstLinesize);
    
    if (result != height) {
        LOG_ERROR("sws_scale failed: expected {} lines, got {}", height, result);
        return QImage();
    }
    
    return image;
}

void H264Decoder::cleanup()
{
    // 防止与 decodeFrame 或其他线程并发清理，使用互斥锁保证线程安全
    QMutexLocker locker(&m_mutex);

    // 标记为未初始化，阻止新的解码请求进入
    m_initialized = false;

    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }

    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }

    if (m_swFrame) {
        av_frame_free(&m_swFrame);
        m_swFrame = nullptr;
    }

    if (m_convertFrame) {
        av_frame_free(&m_convertFrame);
        m_convertFrame = nullptr;
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
    m_hwPixelFormat = AV_PIX_FMT_NONE;
    m_hwAccelName.clear();
    m_initialized = false;

    LOG_DEBUG("H264Decoder cleanup completed");
}

// 硬件解码的关键回调函数 - 根据FFmpeg官方示例
enum AVPixelFormat H264Decoder::get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    H264Decoder* decoder = static_cast<H264Decoder*>(ctx->opaque);
    if (!decoder) {
        LOG_ERROR("Decoder instance is null in get_hw_format callback");
        return AV_PIX_FMT_NONE;
    }
    
    const enum AVPixelFormat *p;
    
    LOG_DEBUG("get_hw_format called, target format: {}", av_get_pix_fmt_name(decoder->m_hwPixelFormat));
    LOG_DEBUG("Available pixel formats:");
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        LOG_DEBUG("  - {}", av_get_pix_fmt_name(*p));
    }
    
    // 首先检查是否有我们期望的硬件格式
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == decoder->m_hwPixelFormat) {
            LOG_INFO("Selected exact hardware pixel format: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }
    
    // 如果没有找到完全匹配的格式，根据硬件加速器类型选择最佳格式
    LOG_DEBUG("Target format {} not found, trying best available format for {}", 
             av_get_pix_fmt_name(decoder->m_hwPixelFormat), decoder->m_hwAccelName);
    
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        // 对于RKMPP，优先选择 DRM_PRIME
        if (decoder->m_hwAccelName == "rkmpp") {
            if (*p == AV_PIX_FMT_DRM_PRIME) {
                LOG_INFO("Selected DRM_PRIME format for Rockchip RKMPP hardware acceleration");
                decoder->m_hwPixelFormat = *p;
                return *p;
            }
        }

        // 对于QSV，优先选择D3D11/DXVA2格式（这是QSV实际使用的格式）
        if (decoder->m_hwAccelName == "qsv") {
            if (*p == AV_PIX_FMT_D3D11) {
                LOG_INFO("Selected D3D11 format for Intel QSV hardware acceleration");
                // 更新解码器的硬件像素格式，确保后续处理正确
                decoder->m_hwPixelFormat = *p;
                return *p;
            }
            if (*p == AV_PIX_FMT_DXVA2_VLD) {
                LOG_INFO("Selected DXVA2 format for Intel QSV hardware acceleration");
                decoder->m_hwPixelFormat = *p;
                return *p;
            }
            if (*p == AV_PIX_FMT_D3D11VA_VLD) {
                LOG_INFO("Selected D3D11VA format for Intel QSV hardware acceleration");
                decoder->m_hwPixelFormat = *p;
                return *p;
            }
            // QSV也可能直接支持QSV格式
            if (*p == AV_PIX_FMT_QSV) {
                LOG_INFO("Selected native QSV format for Intel QSV hardware acceleration");
                decoder->m_hwPixelFormat = *p;
                return *p;
            }
        }
        // 对于CUDA
        else if (decoder->m_hwAccelName == "cuda") {
            if (*p == AV_PIX_FMT_CUDA) {
                LOG_INFO("Selected CUDA format for hardware acceleration");
                decoder->m_hwPixelFormat = *p;
                return *p;
            }
        }
        // 对于DXVA2
        else if (decoder->m_hwAccelName == "dxva2") {
            if (*p == AV_PIX_FMT_DXVA2_VLD) {
                LOG_INFO("Selected DXVA2 format for hardware acceleration");
                decoder->m_hwPixelFormat = *p;
                return *p;
            }
        }
        // 对于D3D11VA
        else if (decoder->m_hwAccelName == "d3d11va") {
            if (*p == AV_PIX_FMT_D3D11VA_VLD || *p == AV_PIX_FMT_D3D11) {
                LOG_INFO("Selected D3D11VA format for hardware acceleration: {}", av_get_pix_fmt_name(*p));
                decoder->m_hwPixelFormat = *p;
                return *p;
            }
        }
    }
    
    // 次优选择：任何DirectX格式（用于Intel集成显卡）
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_D3D11 || *p == AV_PIX_FMT_DXVA2_VLD || *p == AV_PIX_FMT_D3D11VA_VLD) {
            LOG_INFO("Selected fallback DirectX format for hardware acceleration: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }
    
    // 最后选择：任何硬件格式
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA 
            #ifdef _WIN32
            || *p == AV_PIX_FMT_D3D12 || *p == AV_PIX_FMT_D3D11 
            || *p == AV_PIX_FMT_DXVA2_VLD || *p == AV_PIX_FMT_D3D11VA_VLD
            #endif
        ) {
            LOG_INFO("Selected any available hardware format: {}", av_get_pix_fmt_name(*p));
            return *p;
        }
    }
    
    // 如果只有软件格式可用，警告并选择YUV420P
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_YUV420P) {
            LOG_WARN("No hardware pixel formats available, falling back to SOFTWARE decoding with yuv420p");
            LOG_WARN("Hardware acceleration for {} will not be used", decoder->m_hwAccelName);
            return *p;
        }
    }
    
    LOG_ERROR("No suitable pixel format found");
    return AV_PIX_FMT_NONE;
}

bool H264Decoder::validateHardwareDecoding()
{
    // 如果没有硬件加速器名称，说明是软件解码，无需验证
    if (m_hwAccelName.isEmpty()) {
        return true;
    }
    
    LOG_DEBUG("Validating hardware decoding for: {}", m_hwAccelName);
    
    // 检查解码器上下文是否正确设置
    if (!m_codecContext) {
        LOG_ERROR("Codec context is null during validation");
        return false;
    }
    
    // 检查是否设置了get_format回调函数（硬件解码的关键）
    if (!m_codecContext->get_format) {
        LOG_WARN("get_format callback not set - hardware decoding may not work");
        return false;
    }
    
    // 检查硬件设备上下文是否存在
    if (!m_hwDeviceCtx) {
        LOG_WARN("Hardware device context is null - hardware acceleration not active");
        return false;
    }
    
    // 检查硬件像素格式是否设置
    if (m_hwPixelFormat == AV_PIX_FMT_NONE) {
        LOG_WARN("Hardware pixel format not set - may fall back to software");
        return false;
    }
    
    LOG_DEBUG("✓ Hardware decoding validation passed for: {}", m_hwAccelName);
    return true;
}
