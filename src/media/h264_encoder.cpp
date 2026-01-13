#include "h264_encoder.h"
#include "logger_manager.h"
#include <QDebug>
#include <cstdio>

// 硬件设备上下文管理器 - 单例模式，避免重复创建硬件上下文
class HardwareContextManager
{
public:
    static HardwareContextManager &instance()
    {
        static HardwareContextManager instance;
        return instance;
    }

    AVBufferRef *getDeviceContext(const QString &hwAccel)
    {
        QMutexLocker locker(&m_mutex);

        if (m_contexts.contains(hwAccel))
        {
            AVBufferRef *ctx = m_contexts[hwAccel];
            if (ctx)
            {
                return av_buffer_ref(ctx);
            }
            else
            {
                m_contexts.remove(hwAccel);
            }
        }

        AVHWDeviceType deviceType = av_hwdevice_find_type_by_name(hwAccel.toUtf8().data());
        if (deviceType == AV_HWDEVICE_TYPE_NONE)
        {
            LOG_ERROR("Hardware device type not found: {}", hwAccel);
            return nullptr;
        }

        AVBufferRef *newCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&newCtx, deviceType, nullptr, nullptr, 0);
        if (ret < 0)
        {
            if (hwAccel == "qsv")
            {
                ret = av_hwdevice_ctx_create(&newCtx, deviceType, "auto", nullptr, 0);
            }

            if (ret < 0)
            {
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
    QMap<QString, AVBufferRef *> m_contexts;
    QMutex m_mutex;
};

H264Encoder::H264Encoder(QObject *parent)
    : QObject(parent), m_codecContext(nullptr), m_codec(nullptr), m_frame(nullptr)
    , m_hwFrame(nullptr), m_packet(nullptr), m_swsContext(nullptr), m_hwDeviceCtx(nullptr)
    , m_width(0), m_height(0), m_fps(30), m_bitrate(2000000), m_frameCount(0)
    , m_hwPixelFormat(AV_PIX_FMT_NONE), m_initialized(false), m_forceKeyFrame(false)
{
    m_h264Bsf = nullptr;
}

H264Encoder::~H264Encoder()
{
    cleanup();
}

QStringList H264Encoder::getAvailableHWAccels()
{
    QStringList hwAccels;

    // 检查常见的硬件加速器，按优先级排序
    const char *accelNames[] = {
        "nvidia",       // NVIDIA CUDA
        "cuda",       // NVIDIA CUDA
        "nvenc",        // NVIDIA
        "amf",          // AMD
        "vaapi",       // Intel VAAPI
        "qsv",          // Intel Quick Sync (优先检测)
        "vulkan",     // Vulkan
        // "mf",     // Microsoft Media Foundation
        "videotoolbox", // macOS
        "v4l2m2m",      // Linux V4L2
        "omx",          // OpenMAX
        "rkmpp",       // Rockchip MPP
        "mpp",         // MPP
        "mppenc",     // MPP Encoder
        nullptr};

    for (int i = 0; accelNames[i]; ++i)
    {
        QString codecName = QString("h264_%1").arg(accelNames[i]);
        const AVCodec *codec = avcodec_find_encoder_by_name(codecName.toUtf8().data());
        if (codec)
        {
            // 实际测试编码器是否可用（创建上下文并尝试打开）
            AVCodecContext *testContext = avcodec_alloc_context3(codec);
            if (testContext)
            {
                // 设置最小测试参数
                testContext->width = 640;
                testContext->height = 480;
                testContext->time_base = AVRational{1, 30};
                testContext->framerate = AVRational{30, 1};
                testContext->pix_fmt = AV_PIX_FMT_NV12;
                
                // 对于QSV，调整分辨率为16的倍数
                if (QString(accelNames[i]) == "qsv")
                {
                    testContext->width = 640;
                    testContext->height = 480;
                }
                
                int ret = avcodec_open2(testContext, codec, nullptr);
                if (ret >= 0)
                {
                    LOG_INFO("✓ Hardware encoder {} is available and working", codecName);
                    hwAccels << accelNames[i];
                    avcodec_free_context(&testContext);
                }
                else
                {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    LOG_DEBUG("✗ Hardware encoder {} found but cannot be opened: {}", codecName, errbuf);
                    avcodec_free_context(&testContext);
                }
            }
        }
        else
        {
            LOG_DEBUG("Hardware encoder not found: {}", codecName);
        }
    }

    return hwAccels;
}

bool H264Encoder::initialize(int width, int height, int fps, int bitrate)
{
    QMutexLocker locker(&m_mutex);

    if (m_initialized)
    {
        LOG_INFO("Encoder already initialized, cleaning up first");
        cleanup();
    }

    m_width = width;
    m_height = height;
    m_fps = fps;
    m_bitrate = bitrate;

    // 优先尝试硬件加速
    QStringList hwAccels = getAvailableHWAccels();

    bool success = false;
    if (!hwAccels.isEmpty())
    {
        for (const QString &hwAccel : hwAccels)
        {
            LOG_INFO("Trying hardware acceleration: {}", hwAccel);
            if (initializeCodec(hwAccel))
            {
                LOG_INFO("Successfully initialized H264 encoder with {} acceleration", hwAccel);
                success = true;
                break;
            }
        }
    }

    // 如果硬件加速失败，使用软件编码
    if (!success)
    {
        LOG_INFO("Hardware acceleration not available, using software encoding");
        success = initializeCodec();
    }

    if (success)
    {
        m_initialized = true;
        QString accelType = m_hwAccelName.isEmpty() ? "software" : m_hwAccelName;
        LOG_INFO("🎯 H264 encoder successfully initialized with {} acceleration", accelType);

        // 性能优化提示
        if (m_hwAccelName.isEmpty())
        {
            LOG_INFO("💡 Using optimized software encoding - consider upgrading GPU drivers for hardware acceleration");
        }
        else
        {
            LOG_INFO("🚀 Hardware acceleration active - optimal performance enabled");
        }
    }
    else
    {
        LOG_ERROR("❌ Failed to initialize H264 encoder with any method");
        cleanup();
    }

    return success;
}

bool H264Encoder::initializeCodec(const QString &hwAccel)
{
    // 查找编码器
    QString codecName = hwAccel.isEmpty() ? "libx264" : QString("h264_%1").arg(hwAccel);
    m_codec = avcodec_find_encoder_by_name(codecName.toUtf8().data());

    if (!m_codec)
    {
        LOG_ERROR("Codec {} not found", codecName);
        return false;
    }

    LOG_INFO("Found codec: {}", codecName);

    // 创建编码器上下文
    m_codecContext = avcodec_alloc_context3(m_codec);
    if (!m_codecContext)
    {
        LOG_ERROR("Could not allocate video codec context");
        return false;
    }

    // 设置编码参数
    m_codecContext->bit_rate = m_bitrate;
    m_codecContext->width = m_width;
    m_codecContext->height = m_height;
    m_codecContext->time_base = AVRational{1, m_fps};
    m_codecContext->framerate = AVRational{m_fps, 1};
    m_codecContext->gop_size = m_fps;     // 每1秒一个关键帧（更频繁，避免花屏）
    m_codecContext->max_b_frames = 0;     // 不使用B帧，只使用I帧和P帧
    m_codecContext->keyint_min = m_fps / 2; // 最小关键帧间隔0.5秒

    // 网络自适应优化：针对高延迟网络的编码参数
    m_codecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecContext->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecContext->slices = 4;

    // 设置编码预设和调优
    if (hwAccel.isEmpty())
    {
        // 软件编码优化 - 统一使用NV12格式
        m_codecContext->pix_fmt = AV_PIX_FMT_NV12;
        m_hwPixelFormat = AV_PIX_FMT_NONE;
        m_hwDeviceCtx = nullptr;

        // 验证分辨率参数 - 确保分辨率是偶数（H264要求）
        if (m_width % 2 != 0 || m_height % 2 != 0)
        {
            LOG_WARN("Adjusting resolution from {}x{} to make it even for H264 compatibility", m_width, m_height);
            m_width = (m_width + 1) & ~1;
            m_height = (m_height + 1) & ~1;
            m_codecContext->width = m_width;
            m_codecContext->height = m_height;
        }

        // 验证比特率是否合理
        int minBitrate = m_width * m_height * m_fps * 0.05;
        int maxBitrate = m_width * m_height * m_fps * 0.5;
        if (m_bitrate < minBitrate)
        {
            m_bitrate = minBitrate;
            m_codecContext->bit_rate = m_bitrate;
            LOG_WARN("Adjusted bitrate to minimum safe value: {}", m_bitrate);
        }
        else if (m_bitrate > maxBitrate)
        {
            m_bitrate = maxBitrate;
            m_codecContext->bit_rate = m_bitrate;
            LOG_WARN("Adjusted bitrate to maximum safe value: {}", m_bitrate);
        }

        LOG_INFO("Setting software encoding parameters: {}x{}, {}fps, {}bps", m_width, m_height, m_fps, m_bitrate);
        
        // 基础编码选项
        av_opt_set(m_codecContext->priv_data, "preset", "fast", 0);
        av_opt_set(m_codecContext->priv_data, "tune", "zerolatency", 0);
        av_opt_set(m_codecContext->priv_data, "profile", "baseline", 0); // 使用baseline profile提高兼容性
        
        // 构建完整的x264参数字符串，确保Annex-B格式和重复SPS/PPS
        QString x264Params = QString("keyint=%1:min-keyint=%2:no-scenecut:repeat-headers=1:bframes=0:b-adapt=0")
                            .arg(m_fps)
                            .arg(m_fps / 2);
        av_opt_set(m_codecContext->priv_data, "x264-params", x264Params.toStdString().c_str(), 0);
        
        LOG_INFO("Software encoder configured with baseline profile, Annex-B format and repeat headers (GOP: {} frames)", m_fps);
    }
    else
    {
        // 硬件加速初始化
        LOG_INFO("Setting hardware encoding parameters: {}x{}, {}fps, {}bps", m_width, m_height, m_fps, m_bitrate);
        if (!initializeHardwareAccel(hwAccel))
        {
            return false;
        }
    }

    // 打开编码器
    int ret = avcodec_open2(m_codecContext, m_codec, nullptr);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Could not open codec {} ({}x{}, {}fps, {}bps): {} (error code: {})",
                  codecName, m_width, m_height, m_fps, m_bitrate, errbuf, ret);

        // 如果是软件编码器，尝试使用更保守的参数
        if (hwAccel.isEmpty() && ret == AVERROR(EINVAL))
        {
            LOG_WARN("Trying with more conservative software encoding parameters");

            // 重置编码器上下文
            avcodec_free_context(&m_codecContext);
            m_codecContext = avcodec_alloc_context3(m_codec);
            if (!m_codecContext)
            {
                LOG_ERROR("Could not allocate video codec context for retry");
                return false;
            }

            // 使用更保守的参数
            m_codecContext->bit_rate = m_width * m_height * m_fps * 0.1;
            m_codecContext->width = m_width;
            m_codecContext->height = m_height;
            m_codecContext->time_base = AVRational{1, m_fps};
            m_codecContext->framerate = AVRational{m_fps, 1};
            m_codecContext->gop_size = m_fps * 3; // 每3秒一个关键帧
            m_codecContext->max_b_frames = 0;
            m_codecContext->keyint_min = m_fps;
            m_codecContext->pix_fmt = AV_PIX_FMT_NV12;

            av_opt_set(m_codecContext->priv_data, "preset", "ultrafast", 0);
            av_opt_set(m_codecContext->priv_data, "profile", "baseline", 0);

            ret = avcodec_open2(m_codecContext, m_codec, nullptr);
            if (ret < 0)
            {
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("Failed even with conservative parameters: {}", errbuf);
                return false;
            }
            else
            {
                LOG_INFO("Successfully opened codec with conservative parameters");
            }
        }
        else
        {
            return false;
        }
    }

    // 初始化 Annex-B 输出适配（必须：下游 WebRTC 打包器和 decoder 都在按起始码解析）
    if (!initAnnexBBsf())
    {
        LOG_WARN("Failed to initialize H264 bitstream filter (h264_mp4toannexb). Will output raw packets as-is.");
    }

    // 分配帧
    m_frame = av_frame_alloc();
    if (!m_frame)
    {
        LOG_ERROR("Could not allocate video frame");
        return false;
    }

    // 所有编码器都统一分配buffer，包括QSV
    m_frame->format = m_codecContext->pix_fmt;
    m_frame->width = m_codecContext->width;
    m_frame->height = m_codecContext->height;

    ret = av_frame_get_buffer(m_frame, 32);
    if (ret < 0)
    {
        LOG_ERROR("Could not allocate video frame data");
        return false;
    }

    // 分配数据包
    m_packet = av_packet_alloc();
    if (!m_packet)
    {
        LOG_ERROR("Could not allocate packet");
        return false;
    }

    // 初始化图像格式转换器 - 统一使用NV12格式，所有编码器都支持
    // 注意：这里暂不创建SwsContext，在qimageToAVFrame中动态创建
    // 因为分辨率可能被对齐，需要在转换时确定正确的参数

    m_hwAccelName = hwAccel;
    return true;
}

bool H264Encoder::initAnnexBBsf()
{
    // 释放旧的（比如重复 initialize）
    if (m_h264Bsf)
    {
        av_bsf_free(&m_h264Bsf);
        m_h264Bsf = nullptr;
    }

    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (!bsf)
    {
        LOG_WARN("Bitstream filter not found: h264_mp4toannexb");
        return false;
    }

    int ret = av_bsf_alloc(bsf, &m_h264Bsf);
    if (ret < 0 || !m_h264Bsf)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN("av_bsf_alloc failed: {}", errbuf);
        m_h264Bsf = nullptr;
        return false;
    }

    if (!m_codecContext)
    {
        LOG_WARN("Codec context not ready for BSF init");
        av_bsf_free(&m_h264Bsf);
        m_h264Bsf = nullptr;
        return false;
    }

    // 把编码器参数传给 BSF（SPS/PPS/extradata 等）
    ret = avcodec_parameters_from_context(m_h264Bsf->par_in, m_codecContext);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN("avcodec_parameters_from_context failed: {}", errbuf);
        av_bsf_free(&m_h264Bsf);
        m_h264Bsf = nullptr;
        return false;
    }

    m_h264Bsf->time_base_in = m_codecContext->time_base;

    ret = av_bsf_init(m_h264Bsf);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN("av_bsf_init failed: {}", errbuf);
        av_bsf_free(&m_h264Bsf);
        m_h264Bsf = nullptr;
        return false;
    }

    LOG_INFO("H264 bitstream filter initialized: h264_mp4toannexb (force Annex-B output)");
    return true;
}

rtc::binary H264Encoder::getAnnexBExtradata() const
{
    rtc::binary out;

    if (!m_codecContext || !m_codecContext->extradata || m_codecContext->extradata_size <= 0)
    {
        return out;
    }

    // 首选：用 h264_mp4toannexb 把 AVCC extradata 转成 Annex-B（若 extradata 本身已是 Annex-B 也能正常返回）
    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (!bsf)
    {
        return out;
    }

    AVBSFContext *ctx = nullptr;
    int ret = av_bsf_alloc(bsf, &ctx);
    if (ret < 0 || !ctx)
    {
        return out;
    }

    // 拷贝 codec 参数（让 bsf 知道 extradata 的解析方式）
    ret = avcodec_parameters_from_context(ctx->par_in, m_codecContext);
    if (ret < 0)
    {
        av_bsf_free(&ctx);
        return out;
    }

    // 强制把 extradata 输入给 bsf（内部会转成 Annex-B 风格的 SPS/PPS）
    if (ctx->par_in->extradata && ctx->par_in->extradata_size > 0)
    {
        ctx->par_in->extradata = (uint8_t *)av_mallocz(m_codecContext->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!ctx->par_in->extradata)
        {
            av_bsf_free(&ctx);
            return out;
        }
        memcpy(ctx->par_in->extradata, m_codecContext->extradata, m_codecContext->extradata_size);
        ctx->par_in->extradata_size = m_codecContext->extradata_size;
    }

    ctx->time_base_in = m_codecContext->time_base;

    ret = av_bsf_init(ctx);
    if (ret < 0)
    {
        av_bsf_free(&ctx);
        return out;
    }

    // 让 bsf 产出带起始码的 SPS/PPS：做法是送一个空 packet 触发输出
    AVPacket *in = av_packet_alloc();
    if (!in)
    {
        av_bsf_free(&ctx);
        return out;
    }

    in->data = nullptr;
    in->size = 0;

    // 某些版本要求先 send_packet/再 receive_packet；即使 EAGAIN/EOF 也无所谓
    (void)av_bsf_send_packet(ctx, in);
    av_packet_free(&in);

    for (;;)
    {
        AVPacket *p = av_packet_alloc();
        if (!p)
        {
            break;
        }

        ret = av_bsf_receive_packet(ctx, p);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            av_packet_free(&p);
            break;
        }
        if (ret < 0)
        {
            av_packet_free(&p);
            break;
        }

        size_t oldSize = out.size();
        out.resize(oldSize + static_cast<size_t>(p->size));
        memcpy(out.data() + oldSize, p->data, static_cast<size_t>(p->size));
        av_packet_free(&p);
    }

    av_bsf_free(&ctx);
    return out;
}

bool H264Encoder::annexBContainsSpsPps(const rtc::binary &annexb)
{
    if (annexb.size() < 5)
    {
        return false;
    }

    bool hasSps = false;
    bool hasPps = false;

    auto isStartCode4 = [&](size_t i) -> bool {
        return i + 3 < annexb.size() &&
               static_cast<uint8_t>(annexb[i]) == 0x00 &&
               static_cast<uint8_t>(annexb[i + 1]) == 0x00 &&
               static_cast<uint8_t>(annexb[i + 2]) == 0x00 &&
               static_cast<uint8_t>(annexb[i + 3]) == 0x01;
    };
    auto isStartCode3 = [&](size_t i) -> bool {
        return i + 2 < annexb.size() &&
               static_cast<uint8_t>(annexb[i]) == 0x00 &&
               static_cast<uint8_t>(annexb[i + 1]) == 0x00 &&
               static_cast<uint8_t>(annexb[i + 2]) == 0x01;
    };

    for (size_t i = 0; i + 4 < annexb.size(); ++i)
    {
        size_t nalOffset = 0;
        if (isStartCode4(i))
        {
            nalOffset = i + 4;
        }
        else if (isStartCode3(i))
        {
            nalOffset = i + 3;
        }
        else
        {
            continue;
        }

        if (nalOffset >= annexb.size())
        {
            continue;
        }

        uint8_t nalType = static_cast<uint8_t>(annexb[nalOffset]) & 0x1F;
        if (nalType == 7)
        {
            hasSps = true;
        }
        else if (nalType == 8)
        {
            hasPps = true;
        }

        if (hasSps && hasPps)
        {
            return true;
        }
    }

    return false;
}

rtc::binary H264Encoder::packetToAnnexBBinary(const AVPacket *packet)
{
    // 兜底：没有 BSF 或 packet 为空，则原样输出
    if (!packet || packet->size <= 0)
    {
        return rtc::binary();
    }

    if (!m_h264Bsf)
    {
        return avpacketToBinary(const_cast<AVPacket *>(packet));
    }

    // av_bsf_send_packet 会接管引用计数：这里用 ref packet 避免影响调用方 packet 生命周期
    AVPacket *in = av_packet_alloc();
    if (!in)
    {
        return avpacketToBinary(const_cast<AVPacket *>(packet));
    }

    int ret = av_packet_ref(in, packet);
    if (ret < 0)
    {
        av_packet_free(&in);
        return avpacketToBinary(const_cast<AVPacket *>(packet));
    }

    ret = av_bsf_send_packet(m_h264Bsf, in);
    // send_packet 成功后 BSF 会持有/释放 in；失败则我们释放
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_WARN("av_bsf_send_packet failed: {}", errbuf);
        av_packet_free(&in);
        return avpacketToBinary(const_cast<AVPacket *>(packet));
    }

    rtc::binary result;

    for (;;)
    {
        AVPacket *out = av_packet_alloc();
        if (!out)
        {
            break;
        }

        ret = av_bsf_receive_packet(m_h264Bsf, out);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            av_packet_free(&out);
            break;
        }
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_WARN("av_bsf_receive_packet failed: {}", errbuf);
            av_packet_free(&out);
            break;
        }

        rtc::binary one = avpacketToBinary(out);
        result.insert(result.end(), one.begin(), one.end());
        av_packet_free(&out);
    }

    // 兜底：关键帧如果没带 SPS/PPS，就把 extradata 的 SPS/PPS 前置，提升随机花屏恢复能力。
    // 注：只在 packet 自身被标记为关键帧时做（避免每帧都塞头，带宽抖动）。
    if ((packet->flags & AV_PKT_FLAG_KEY) != 0)
    {
        if (!annexBContainsSpsPps(result))
        {
            rtc::binary extra = getAnnexBExtradata();
            if (!extra.empty())
            {
                // 确保 extra 里也有起始码；若 bsf 未产出，则不前置
                if (extra.size() >= 4)
                {
                    rtc::binary merged;
                    merged.reserve(extra.size() + result.size());
                    merged.insert(merged.end(), extra.begin(), extra.end());
                    merged.insert(merged.end(), result.begin(), result.end());
                    result.swap(merged);
                    LOG_DEBUG("Prepended SPS/PPS extradata to keyframe packet (size: {} + {})", extra.size(), result.size());
                }
            }
        }
    }

    return result;
}
void H264Encoder::forceKeyFrame()
{
    QMutexLocker locker(&m_mutex);
    m_forceKeyFrame = true;
    LOG_INFO("🔑 Force key frame requested");
}
rtc::binary H264Encoder::encodeFrame(const QImage &image)
{
    QMutexLocker locker(&m_mutex);

    if (!m_initialized)
    {
        LOG_ERROR("Encoder not initialized");
        return rtc::binary();
    }

    // 确保图像格式为RGB888
    QImage rgbImage = image;
    if (rgbImage.format() != QImage::Format_RGB888)
    {
        rgbImage = rgbImage.convertToFormat(QImage::Format_RGB888);
    }

    // 不在这里进行QImage缩放，让FFmpeg的SwsContext处理缩放以获得更好的质量
    // 转换为AVFrame（FFmpeg会自动处理分辨率转换）
    AVFrame *inputFrame = qimageToAVFrame(rgbImage);
    if (!inputFrame)
    {
        LOG_ERROR("Failed to convert QImage to AVFrame with scaling");
        return rtc::binary();
    }

    AVFrame *encodingFrame = inputFrame;

    // 如果使用硬件加速，需要将软件帧传输到硬件
    if (m_hwPixelFormat != AV_PIX_FMT_NONE && m_hwDeviceCtx)
    {
        AVFrame *hw = transferToHardware(inputFrame);
        av_frame_free(&inputFrame);
        encodingFrame = hw;
        if (!encodingFrame)
        {
            LOG_ERROR("Failed to transfer frame to hardware");
            return rtc::binary();
        }
    }

    // 强制第一帧为关键帧，并确保包含SPS/PPS参数集
    // 同时每隔一定帧数（GOP大小）强制生成关键帧，防止长时间无关键帧导致花屏
    bool needKeyFrame = (m_frameCount == 0 || m_forceKeyFrame || (m_frameCount % (m_fps * 2) == 0));
    
    if (needKeyFrame)
    {
        encodingFrame->pict_type = AV_PICTURE_TYPE_I;

        // 注意：部分 FFmpeg 版本的 AVFrame 没有 key_frame 字段（例如 4.4 系列头文件）。
        // 这里用 flags 做兼容标记；真正强制 IDR 主要依赖 pict_type + 编码器侧参数/请求。
#ifdef AV_FRAME_FLAG_KEY
        encodingFrame->flags |= AV_FRAME_FLAG_KEY;
#endif

        // 对于libx264，强制立即输出关键帧
        if (m_hwAccelName.isEmpty()) {
            encodingFrame->pict_type = AV_PICTURE_TYPE_I;
        }

        if (m_frameCount % (m_fps * 2) == 0 && m_frameCount > 0)
        {
            LOG_DEBUG("🔑 Auto-generating IDR frame at frame {} (every 2 seconds for robustness)", m_frameCount);
        }
        else
        {
            LOG_INFO("🔑 Forcing IDR frame (frame count: {}, force key: {})", m_frameCount, m_forceKeyFrame);
        }
        m_forceKeyFrame = false; // 重置强制关键帧标志
    }

    // 编码帧
    int ret = avcodec_send_frame(m_codecContext, encodingFrame);

    // 增加帧计数
    m_frameCount++;

    // 只释放我们当前持有的 encodingFrame，避免 inputFrame 再次释放导致崩溃
    av_frame_free(&encodingFrame);

    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Error sending frame to encoder: {}", errbuf);
        return rtc::binary();
    }

    rtc::binary result;

    // 接收编码后的数据包
    while (ret >= 0)
    {
        ret = avcodec_receive_packet(m_codecContext, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        {
            break;
        }
        else if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Error receiving packet from encoder: {}", errbuf);
            break;
        }

        if (m_packet->size > 0)
        {
            // 关键：统一转 Annex-B，最大兼容性（WebRTC packetizer / 自家 decoder 都按起始码解析）
            rtc::binary packetData = packetToAnnexBBinary(m_packet);
            if (!packetData.empty())
            {
                result.insert(result.end(), packetData.begin(), packetData.end());
            }
        }
        else
        {
            LOG_WARN("Received empty packet from encoder");
        }

        av_packet_unref(m_packet);
    }

    if (result.empty())
    {
        LOG_DEBUG("No encoded data produced (encoder buffering)");
    }

    return result;
}

AVFrame *H264Encoder::qimageToAVFrame(const QImage &image)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
    {
        LOG_ERROR("Failed to allocate AVFrame");
        return nullptr;
    }

    // 统一使用NV12格式，所有编码器都支持
    AVPixelFormat targetFormat = AV_PIX_FMT_NV12;

    frame->format = targetFormat;
    frame->width = m_width;
    frame->height = m_height;

    // 确保帧时间基准设置正确
    frame->pts = AV_NOPTS_VALUE;

    // 为帧分配缓冲区，使用32字节对齐
    int ret = av_frame_get_buffer(frame, 32);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Could not allocate video frame data: {}", errbuf);
        av_frame_free(&frame);
        return nullptr;
    }

    // RGB数据指针
    const uint8_t *srcData[1] = {image.constBits()};
    int srcLinesize[1] = {static_cast<int>(image.bytesPerLine())};

    // 检查SwsContext是否有效，或者需要重新创建
    AVPixelFormat currentTargetFormat = AV_PIX_FMT_NV12;

    // 获取输入图像的实际尺寸
    int inputWidth = image.width();
    int inputHeight = image.height();

    // 检查是否需要重新创建SwsContext（输入尺寸改变或首次创建）
    static int lastInputWidth = -1;
    static int lastInputHeight = -1;

    if (!m_swsContext || inputWidth != lastInputWidth || inputHeight != lastInputHeight)
    {
        // 重新创建SwsContext以适应新的输入尺寸
        if (m_swsContext)
        {
            sws_freeContext(m_swsContext);
        }

        m_swsContext = sws_getContext(
            inputWidth, inputHeight, AV_PIX_FMT_RGB24, // 输入：实际图像尺寸
            m_width, m_height, currentTargetFormat,    // 输出：编码器尺寸
            SWS_BILINEAR, nullptr, nullptr, nullptr    // 使用双线性插值获得更好质量
        );

        if (!m_swsContext)
        {
            LOG_ERROR("SwsContext creation failed for RGB24 to NV12 conversion ({}x{} -> {}x{})",
                      inputWidth, inputHeight, m_width, m_height);
            av_frame_free(&frame);
            return nullptr;
        }

        lastInputWidth = inputWidth;
        lastInputHeight = inputHeight;

        LOG_DEBUG("Created SwsContext for RGB24 to NV12 conversion with scaling: {}x{} -> {}x{}",
                  inputWidth, inputHeight, m_width, m_height);
    }

    // 转换RGB到NV12格式（同时进行缩放）
    int swsRet = sws_scale(m_swsContext,
                           srcData, srcLinesize, 0, inputHeight, // 使用输入图像的高度
                           frame->data, frame->linesize);

    if (swsRet != m_height)
    { // 输出应该是编码器的高度
        LOG_ERROR("sws_scale failed: expected {} lines, got {}", m_height, swsRet);
        av_frame_free(&frame);
        return nullptr;
    }

    return frame;
}

rtc::binary H264Encoder::avpacketToBinary(AVPacket *packet)
{
    rtc::binary data;
    data.resize(packet->size);

    for (int i = 0; i < packet->size; ++i)
    {
        data[i] = static_cast<std::byte>(packet->data[i]);
    }

    // 详细调试：检查输出数据的NAL单元类型
    if (data.size() >= 5)
    {
        // 查找所有NAL单元
        int nalCount = 0;
        for (size_t i = 0; i + 4 < data.size(); ++i)
        {
            if (static_cast<uint8_t>(data[i]) == 0x00 &&
                static_cast<uint8_t>(data[i+1]) == 0x00 &&
                static_cast<uint8_t>(data[i+2]) == 0x00 &&
                static_cast<uint8_t>(data[i+3]) == 0x01)
            {
                uint8_t nalType = static_cast<uint8_t>(data[i+4]) & 0x1F;
                const char* nalTypeName = "Unknown";
                switch(nalType) {
                    case 1: nalTypeName = "Non-IDR"; break;
                    case 5: nalTypeName = "IDR"; break;
                    case 6: nalTypeName = "SEI"; break;
                    case 7: nalTypeName = "SPS"; break;
                    case 8: nalTypeName = "PPS"; break;
                    case 9: nalTypeName = "AUD"; break;
                }
                
                if (nalCount == 0) {
                    LOG_DEBUG("H264 packet: size={}, NAL units found:", packet->size);
                }
                LOG_DEBUG("  NAL[{}] at offset {}: type={} ({})", nalCount, i, nalType, nalTypeName);
                nalCount++;
                
                i += 4; // 跳过起始码
            }
        }
        
        if (nalCount == 0) {
            LOG_WARN("⚠️ No Annex-B start codes found in packet! First 4 bytes: {:02x} {:02x} {:02x} {:02x}",
                     static_cast<uint8_t>(data[0]), static_cast<uint8_t>(data[1]),
                     static_cast<uint8_t>(data[2]), static_cast<uint8_t>(data[3]));
        }
    }

    return data;
}

AVFrame *H264Encoder::transferToHardware(AVFrame *swFrame)
{
    if (!m_hwDeviceCtx || m_hwPixelFormat == AV_PIX_FMT_NONE)
    {
        // 不需要硬件传输，直接返回原帧
        return av_frame_clone(swFrame);
    }

    // 检查硬件帧上下文是否有效
    if (!m_codecContext || !m_codecContext->hw_frames_ctx)
    {
        LOG_WARN("Hardware frames context not available, falling back to software frame");
        return av_frame_clone(swFrame);
    }

    // 创建硬件帧
    AVFrame *hwFrame = av_frame_alloc();
    if (!hwFrame)
    {
        LOG_ERROR("Failed to allocate hardware frame");
        return nullptr;
    }

    // 为硬件帧分配缓冲区 - 必须先分配才能设置属性
    int ret = av_hwframe_get_buffer(m_codecContext->hw_frames_ctx, hwFrame, 0);
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to allocate hardware frame buffer: {}", errbuf);
        av_frame_free(&hwFrame);
        return nullptr;
    }

    // 设置硬件帧属性（在分配缓冲区后）
    hwFrame->width = m_codecContext->width;
    hwFrame->height = m_codecContext->height;

    // 如果软件帧尺寸与硬件帧尺寸不同，需要先缩放
    AVFrame *scaledFrame = swFrame;
    if (swFrame->width != hwFrame->width || swFrame->height != hwFrame->height)
    {
        scaledFrame = av_frame_alloc();
        if (!scaledFrame)
        {
            LOG_ERROR("Failed to allocate scaled frame");
            av_frame_free(&hwFrame);
            return nullptr;
        }

        scaledFrame->format = swFrame->format;
        scaledFrame->width = hwFrame->width;
        scaledFrame->height = hwFrame->height;

        ret = av_frame_get_buffer(scaledFrame, 32);
        if (ret < 0)
        {
            LOG_ERROR("Failed to allocate scaled frame buffer");
            av_frame_free(&hwFrame);
            av_frame_free(&scaledFrame);
            return nullptr;
        }

        // 创建临时的sws上下文进行缩放
        struct SwsContext *tempSwsCtx = sws_getContext(
            swFrame->width, swFrame->height, static_cast<AVPixelFormat>(swFrame->format),
            scaledFrame->width, scaledFrame->height, static_cast<AVPixelFormat>(scaledFrame->format),
            SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!tempSwsCtx)
        {
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
    if (ret < 0)
    {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("Failed to transfer data to hardware frame: {}", errbuf);
        av_frame_free(&hwFrame);
        if (scaledFrame != swFrame)
        {
            av_frame_free(&scaledFrame);
        }
        return nullptr;
    }

    // 清理临时帧
    if (scaledFrame != swFrame)
    {
        av_frame_free(&scaledFrame);
    }

    return hwFrame;
}

bool H264Encoder::initializeHardwareAccel(const QString &hwAccel)
{
    if (!m_codecContext)
    {
        LOG_ERROR("initializeHardwareAccel called with null codec context");
        return false;
    }

    // QSV 单独走（它对 hw_frames_ctx/对齐更敏感）
    if (hwAccel == "qsv")
    {
        return initializeQSV();
    }

    // 关键修正：NVENC/AMF/MF/D3D12VA 这类编码器绝大多数期望的是 system-memory NV12 输入，
    // 不需要也不应该配置 hw_frames_ctx，更不要把 pix_fmt 设成 CUDA/VAAPI 等硬件像素格式。
    // 否则 avcodec_send_frame() 很容易报 "Generic error in an external library"。

    // 先默认走“软件帧输入”（NV12）
    m_hwPixelFormat = AV_PIX_FMT_NONE;
    m_codecContext->pix_fmt = AV_PIX_FMT_NV12;

    // 分辨率对齐：保守处理，避免硬编吃不下（尤其是 NVENC/D3D12VA 对奇数分辨率很敏感）
    if ((m_codecContext->width & 1) || (m_codecContext->height & 1))
    {
        int w = (m_codecContext->width + 1) & ~1;
        int h = (m_codecContext->height + 1) & ~1;
        LOG_WARN("Aligning HW encoder resolution from {}x{} to {}x{}", m_codecContext->width, m_codecContext->height, w, h);
        m_codecContext->width = w;
        m_codecContext->height = h;
        m_width = w;
        m_height = h;
    }

    // 仅对明确需要 hwframe 的编码器才去创建/绑定 hwdevice + hwframes。
    // Windows 常见：
    // - h264_nvenc / h264_mf / h264_d3d12va : NV12 system-memory
    // - h264_vaapi / h264_videotoolbox     : 通常需要 hw pix_fmt
    bool needHwFrames = false;

    if (hwAccel == "vaapi")
    {
        needHwFrames = true;
        m_hwPixelFormat = AV_PIX_FMT_VAAPI;
        m_codecContext->pix_fmt = m_hwPixelFormat;
    }
    else if (hwAccel == "videotoolbox")
    {
        needHwFrames = true;
        m_hwPixelFormat = AV_PIX_FMT_VIDEOTOOLBOX;
        m_codecContext->pix_fmt = m_hwPixelFormat;
    }
    else if (hwAccel == "cuda")
    {
        // h264_cuda 很少见且行为与 nvenc 不同；若用户真要走它，则按 hwframe 路径处理。
        needHwFrames = true;
        m_hwPixelFormat = AV_PIX_FMT_CUDA;
        m_codecContext->pix_fmt = m_hwPixelFormat;
    }

    if (needHwFrames)
    {
        m_hwDeviceCtx = HardwareContextManager::instance().getDeviceContext(hwAccel);
        if (!m_hwDeviceCtx)
        {
            LOG_ERROR("Failed to create/get hardware device context for {}", hwAccel);
            m_hwPixelFormat = AV_PIX_FMT_NONE;
            m_codecContext->pix_fmt = AV_PIX_FMT_NV12;
            return false;
        }

        m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        if (!m_codecContext->hw_device_ctx)
        {
            LOG_ERROR("Failed to ref hw_device_ctx for {}", hwAccel);
            return false;
        }

        AVBufferRef *hwFramesRef = av_hwframe_ctx_alloc(m_hwDeviceCtx);
        if (!hwFramesRef)
        {
            LOG_ERROR("Failed to allocate hwframe context for {}", hwAccel);
            return false;
        }

        AVHWFramesContext *framesCtx = reinterpret_cast<AVHWFramesContext *>(hwFramesRef->data);
        framesCtx->format = m_hwPixelFormat;
        framesCtx->sw_format = AV_PIX_FMT_NV12;
        framesCtx->width = m_codecContext->width;
        framesCtx->height = m_codecContext->height;
        framesCtx->initial_pool_size = 20;

        int ret = av_hwframe_ctx_init(hwFramesRef);
        if (ret < 0)
        {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("Failed to init hwframe context for {}: {}", hwAccel, errbuf);
            av_buffer_unref(&hwFramesRef);
            return false;
        }

        m_codecContext->hw_frames_ctx = hwFramesRef;
    }

    // 硬编专有参数（稳定优先、低延迟）
    if (hwAccel == "nvenc")
    {
        // 说明：不同 FFmpeg/NVENC 版本可用值不同；这里选择相对保守且广泛支持的取值
        av_opt_set(m_codecContext->priv_data, "preset", "p4", 0);
        av_opt_set(m_codecContext->priv_data, "tune", "ll", 0);
        av_opt_set(m_codecContext->priv_data, "rc", "cbr", 0);
        av_opt_set(m_codecContext->priv_data, "forced-idr", "1", 0);
        av_opt_set(m_codecContext->priv_data, "repeat-headers", "1", 0);

        // 明确 profile，避免某些驱动/构建默认高 profile 导致兼容性问题
        av_opt_set(m_codecContext->priv_data, "profile", "baseline", 0);

        // 若驱动支持，启用 0-latency（不支持会被忽略/返回错误，FFmpeg 不会因此崩）
        av_opt_set(m_codecContext->priv_data, "zerolatency", "1", 0);
    }
    else if (hwAccel == "amf")
    {
        av_opt_set(m_codecContext->priv_data, "usage", "lowlatency", 0);
        av_opt_set(m_codecContext->priv_data, "rc", "cbr", 0);
        av_opt_set(m_codecContext->priv_data, "repeat-headers", "1", 0);
        av_opt_set(m_codecContext->priv_data, "profile", "baseline", 0);
    }
    else if (hwAccel == "mf")
    {
        // MF 通常吃 NV12 system-memory
        m_codecContext->pix_fmt = AV_PIX_FMT_NV12;

        // 有些 build 支持下列选项，有些不支持；不支持的话 opt_set 失败也不致命
        av_opt_set(m_codecContext->priv_data, "rate_control", "cbr", 0);
    }
    else if (hwAccel == "d3d12va")
    {
        // d3d12va 编码器同样通常吃 NV12 system-memory
        m_codecContext->pix_fmt = AV_PIX_FMT_NV12;
    }
    else if (hwAccel == "vaapi")
    {
        av_opt_set(m_codecContext->priv_data, "rc_mode", "CBR", 0);
        av_opt_set(m_codecContext->priv_data, "low_power", "1", 0);
        av_opt_set(m_codecContext->priv_data, "idr_interval", "1", 0);
    }

    m_codecContext->max_b_frames = 0;

    LOG_INFO("Hardware encoder pre-configured: hwAccel={}, pix_fmt={}, hwPixFmt={}, hwFramesCtx={}",
             hwAccel,
             av_get_pix_fmt_name(m_codecContext->pix_fmt),
             (m_hwPixelFormat == AV_PIX_FMT_NONE ? "none" : av_get_pix_fmt_name(m_hwPixelFormat)),
             (m_codecContext->hw_frames_ctx ? "yes" : "no"));

    return true;
}

bool H264Encoder::initializeQSV()
{
    if (!m_codecContext)
    {
        return false;
    }

    // QSV 对分辨率对齐很敏感：按 16 对齐
    int alignedW = (m_codecContext->width + 15) & ~15;
    int alignedH = (m_codecContext->height + 15) & ~15;
    if (alignedW != m_codecContext->width || alignedH != m_codecContext->height)
    {
        LOG_WARN("Aligning QSV resolution from {}x{} to {}x{}", m_codecContext->width, m_codecContext->height, alignedW, alignedH);
        m_codecContext->width = alignedW;
        m_codecContext->height = alignedH;
        m_width = alignedW;
        m_height = alignedH;
    }

    // QSV：优先走 NV12 system-memory 输入（更兼容，避免复杂的 hwframe 管线）
    m_codecContext->pix_fmt = AV_PIX_FMT_NV12;
    m_hwPixelFormat = AV_PIX_FMT_NONE;

    // QSV 常用稳定参数（不一定每个 build 都支持，设置失败不会致命）
    av_opt_set(m_codecContext->priv_data, "async_depth", "1", 0);
    av_opt_set(m_codecContext->priv_data, "look_ahead", "0", 0);
    av_opt_set(m_codecContext->priv_data, "b", "0", 0);
    av_opt_set(m_codecContext->priv_data, "bf", "0", 0);
    av_opt_set(m_codecContext->priv_data, "repeat-headers", "1", 0);

    LOG_INFO("QSV encoder pre-configured: pix_fmt=NV12, aligned {}x{}", m_codecContext->width, m_codecContext->height);
    return true;
}

void H264Encoder::cleanup()
{
    if (m_packet)
    {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }

    if (m_frame)
    {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }

    if (m_hwFrame)
    {
        av_frame_free(&m_hwFrame);
        m_hwFrame = nullptr;
    }

    if (m_swsContext)
    {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }

    if (m_codecContext)
    {
        avcodec_free_context(&m_codecContext);
        m_codecContext = nullptr;
    }

    // 释放硬件设备上下文的引用（共享管理器会处理实际的释放）
    if (m_hwDeviceCtx)
    {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }

    // 释放 BSF
    if (m_h264Bsf)
    {
        av_bsf_free(&m_h264Bsf);
        m_h264Bsf = nullptr;
    }

    m_codec = nullptr;
    m_hwPixelFormat = AV_PIX_FMT_NONE;
    m_hwAccelName.clear();
    m_initialized = false;

    LOG_DEBUG("H264Encoder cleanup completed");
}
