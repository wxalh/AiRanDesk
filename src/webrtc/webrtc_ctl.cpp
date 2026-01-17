#include "webrtc_ctl.h"
#include "constant.h"
#include "logger_manager.h"
#include "h264_decoder.h"
#include "media_player.h"
#include "util/json_util.h"
#include "util/file_packet_util.h"
#include <QTimer>
#include <QApplication>
#include <QScreen>
#include <QThread>
#include <QDataStream>
#include <QUuid>
#include <iostream>

/**
 * WebRtcCtl类实现
 * 该类负责处理WebRTC客户端的所有功能，包括连接、媒体处理、数据
 * init pc -> setup tracks and datachannels -> on recv remote sdp ->send remote sdp -> send local sdp
 * -> on remote ice candidates -> send local ice candidates
 */
WebRtcCtl::WebRtcCtl(const QString &remoteId, const QString &remotePwdMd5,
                     bool isOnlyFile, bool adaptiveResolution, bool onlyRelay, QObject *parent)
    : QObject(parent),
      m_remoteId(remoteId),
      m_remotePwdMd5(remotePwdMd5),
      m_connected(false),
      m_isOnlyFile(isOnlyFile),
      m_adaptiveResolution(adaptiveResolution),
      m_onlyRelay(onlyRelay),
      m_sdpSent(false),
      m_waitingForKeyFrame(true) // 初始时等待关键帧
{
    // 初始化ICE服务器配置
    m_host = ConfigUtil->ice_host.toStdString();
    m_port = (uint16_t)ConfigUtil->ice_port;
    m_username = ConfigUtil->ice_username.toStdString();
    m_password = ConfigUtil->ice_password.toStdString();

    // 初始化文件分包工具
    m_filePacketUtil = std::make_unique<FilePacketUtil>(this);

    // 连接文件分包工具的信号
    connect(m_filePacketUtil.get(), &FilePacketUtil::fileDownloadCompleted,
            this, &WebRtcCtl::recvDownloadFile);
    connect(m_filePacketUtil.get(), &FilePacketUtil::fileReceived,
            this, &WebRtcCtl::recvDownloadFile);

    LOG_INFO("created for remote: {}", m_remoteId);
}

WebRtcCtl::~WebRtcCtl()
{
    LOG_DEBUG("destructor");
    destroy();
}

void WebRtcCtl::init()
{
    LOG_INFO("Creating PeerConnection for control side");

    if (!m_isOnlyFile)
    {
        // 初始化H264解码器（启用硬件加速）
        m_h264Decoder = std::make_unique<H264Decoder>();
        m_h264Decoder->initialize();
        // 初始化媒体播放器
        m_mediaPlayer = std::make_unique<MediaPlayer>();
        // m_mediaPlayer->startPlayback(); // 启动音频播放
    }
    // 初始化WebRTC
    initPeerConnection();
    if (!m_isOnlyFile)
    {
        // 创建接收轨道
        createTracks();
    }

    setupCallbacks();

    // 发送CONNECT消息给被控端
    JsonObjectBuilder connectMsgBuilder = JsonUtil::createObject()
                                              .add(Constant::KEY_ROLE, Constant::ROLE_CTL)
                                              .add(Constant::KEY_TYPE, Constant::TYPE_CONNECT)
                                              .add(Constant::KEY_RECEIVER, m_remoteId)
                                              .add(Constant::KEY_RECEIVER_PWD, m_remotePwdMd5)
                                              .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                              .add(Constant::KEY_IS_ONLY_FILE, m_isOnlyFile)
                                              .add(Constant::KEY_ONLY_RELAY, m_onlyRelay)
                                              .add(Constant::KEY_FPS, ConfigUtil->fps);

    // 如果启用了自适应分辨率，则包含控制端可显示的最大区域信息
    if (m_adaptiveResolution)
    {
        QScreen *screen = QApplication::primaryScreen();
        QRect screenGeometry = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);

        // 计算控制端窗口能够显示的最大内容区域（减去标题栏等UI开销）
        int titleBarHeight = 30;                                         // 估算标题栏高度
        int maxContentWidth = screenGeometry.width() - 20;               // 减去边距
        int maxContentHeight = screenGeometry.height() - titleBarHeight; // 减去各种UI开销

        connectMsgBuilder = connectMsgBuilder.add("control_max_width", maxContentWidth)
                                .add("control_max_height", maxContentHeight);

        LOG_INFO("Sending CONNECT message with adaptive resolution - max display area: {}x{}",
                 maxContentWidth, maxContentHeight);
    }
    else
    {
        LOG_INFO("Sending CONNECT message without adaptive resolution - client will use original resolution");
    }

    QJsonObject connectMsg = connectMsgBuilder.build();
    QString message = JsonUtil::toCompactString(connectMsg);
    emit sendWsCliTextMsg(message);
}

void WebRtcCtl::initPeerConnection()
{
    try
    {
        // 配置ICE服务器
        rtc::Configuration config;

        // STUN服务器
        rtc::IceServer stunServer(m_host, m_port);
        config.iceServers.push_back(stunServer);

        rtc::IceServer turnUdpServer(m_host, m_port, m_username, m_password, rtc::IceServer::RelayType::TurnUdp);
        config.iceServers.push_back(turnUdpServer);

        rtc::IceServer turnTcpServer(m_host, m_port, m_username, m_password, rtc::IceServer::RelayType::TurnTcp);
        config.iceServers.push_back(turnTcpServer);

        if (m_onlyRelay)
        {
            // 如果仅使用中继服务器，禁用STUN服务器
            config.iceServers.clear();
            config.iceServers.push_back(turnUdpServer);
            config.iceServers.push_back(turnTcpServer);
            config.iceTransportPolicy = rtc::TransportPolicy::Relay;
            LOG_INFO("Using only TURN servers for ICE transport");
        }

        // 创建PeerConnection
        m_peerConnection = std::make_shared<rtc::PeerConnection>(config);
        LOG_INFO("PeerConnection created successfully");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to initialize PeerConnection: {}", e.what());
    }
}

void WebRtcCtl::createTracks()
{
    if (!m_peerConnection)
    {
        LOG_ERROR("PeerConnection not available for creating tracks");
        return;
    }

    try
    {
        // 创建视频接收轨道 - 设置RTP解包器
        LOG_INFO("Creating video receive track");
        std::string video_name = Constant::TYPE_VIDEO.toStdString();
        rtc::Description::Video videoDesc(video_name); // 使用固定流名称匹配发送端
        videoDesc.addH264Codec(96);                    // 确保payload type匹配
        uint32_t videoSSRC = 1;
        std::string msid = Constant::TYPE_VIDEO_MSID.toStdString();
        videoDesc.addSSRC(videoSSRC, video_name, msid, video_name);
        videoDesc.setDirection(rtc::Description::Direction::RecvOnly);
        m_videoTrack = m_peerConnection->addTrack(videoDesc);

        // 为视频轨道设置H264 RTP解包器 - 这是必需的！
        auto h264Depacketizer = std::make_shared<rtc::H264RtpDepacketizer>();
        m_videoTrack->setMediaHandler(h264Depacketizer);

        // 创建音频接收轨道
        LOG_INFO("Creating audio receive track");
        std::string audio_name = Constant::TYPE_AUDIO.toStdString();
        rtc::Description::Audio audioDesc(audio_name); // 使用固定流名称匹配发送端
        audioDesc.addOpusCodec(111);                   // 确保payload type匹配
        audioDesc.setDirection(rtc::Description::Direction::RecvOnly);
        m_audioTrack = m_peerConnection->addTrack(audioDesc);

        LOG_INFO("Control side tracks created successfully");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to create tracks: {}", e.what());
    }
}

void WebRtcCtl::setupCallbacks()
{
    if (!m_peerConnection)
        return;

    // 连接状态回调
    m_peerConnection->onStateChange([this](rtc::PeerConnection::State state)
                                    {
        m_connected = (state == rtc::PeerConnection::State::Connected);

        std::string stateStr;
        if(state == rtc::PeerConnection::State::Connected){
            stateStr = "Connected";
            rtc::Candidate local;
            rtc::Candidate remote;
            if(m_peerConnection->getSelectedCandidatePair(&local, &remote)){
                LOG_INFO("Selected candidate pair: local={}, remote={}", std::string(local), std::string(remote));
            }
        }else if(state == rtc::PeerConnection::State::Connecting){
            stateStr = "Checking";  
        }else if(state == rtc::PeerConnection::State::New){
            stateStr = "New";
        }else if(state == rtc::PeerConnection::State::Failed){
            stateStr = "Failed";
        }else if(state == rtc::PeerConnection::State::Disconnected){
            stateStr = "Disconnected";
        }else if(state == rtc::PeerConnection::State::Closed){
            stateStr = "Closed";
        }else{
            stateStr = "Unknown";
        }
        LOG_DEBUG("Control side connection state: {}",stateStr); });

    // ICE连接状态回调
    m_peerConnection->onIceStateChange([this](rtc::PeerConnection::IceState state)
                                       {
                                        std::string stateStr;
                                        if(state == rtc::PeerConnection::IceState::Connected){
                                            stateStr = "Connected";
                                        }else if(state == rtc::PeerConnection::IceState::Checking){
                                            stateStr = "Checking";  
                                        }else if(state == rtc::PeerConnection::IceState::New){
                                            stateStr = "New";
                                        }else if(state == rtc::PeerConnection::IceState::Failed){
                                            stateStr = "Failed";
                                        }else if(state == rtc::PeerConnection::IceState::Disconnected){
                                            stateStr = "Disconnected";
                                        }else if(state == rtc::PeerConnection::IceState::Closed){
                                            stateStr = "Closed";
                                        }else if(state == rtc::PeerConnection::IceState::Completed){
                                            stateStr = "Completed";
                                        }else{
                                            stateStr = "Unknown";
                                        }
                                        LOG_INFO("Control side ICE state: {}", stateStr); });

    // ICE候选者收集回调
    m_peerConnection->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state)
                                             {
                                                std::string stateStr;
                                                if(state == rtc::PeerConnection::GatheringState::InProgress){
                                                    stateStr = "InProgress";
                                                }else if(state == rtc::PeerConnection::GatheringState::Complete){
                                                    stateStr = "Complete";
                                                }else if(state == rtc::PeerConnection::GatheringState::New){
                                                    stateStr = "New";
                                                }else{
                                                    stateStr = "Unknown";
                                                }
                                                LOG_INFO("Control side ICE gathering state: {}", stateStr); });
    m_peerConnection->onLocalDescription([this](rtc::Description description)
                                         {
        LOG_INFO("Control side local description set");
        try
        {
            QString sdp = QString::fromStdString(std::string(description));
            QString type = QString::fromStdString(description.typeString());
            if(type == Constant::TYPE_OFFER){
                return;
            }

            // 发送本地描述给被控端
            QJsonObject answerMsg = JsonUtil::createObject()
                                        .add(Constant::KEY_ROLE, Constant::ROLE_CTL)
                                        .add(Constant::KEY_TYPE, type)
                                        .add(Constant::KEY_RECEIVER, m_remoteId)
                                        .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                        .add(Constant::KEY_DATA, sdp)
                                        .build();

            QString message = JsonUtil::toCompactString(answerMsg);
            emit sendWsCliTextMsg(message);
            LOG_INFO("Sent local description ({}) to cli", message);
            m_sdpSent = true; // 标记已发送SDP
            if(!m_localCandidates.isEmpty()){
                foreach(const QString &candidate, m_localCandidates)
                {
                    emit sendWsCliTextMsg(candidate);
                    LOG_DEBUG("Sent local candidate to cli: {}", candidate);
                }
                m_localCandidates.clear(); // 清空已发送的候选者
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to send answer: {}", e.what());
        } });

    // 本地候选者回调
    m_peerConnection->onLocalCandidate([this](const rtc::Candidate &candidate)
                                       {
        QString candidateStr = QString::fromStdString(std::string(candidate));
        QString midStr = QString::fromStdString(candidate.mid());
        
        // 发送本地候选者给被控端
        QJsonObject candidateMsg = JsonUtil::createObject()
            .add(Constant::KEY_ROLE, Constant::ROLE_CTL)
            .add(Constant::KEY_TYPE, Constant::TYPE_CANDIDATE)
            .add(Constant::KEY_RECEIVER, m_remoteId)
            .add(Constant::KEY_SENDER, ConfigUtil->local_id)
            .add(Constant::KEY_DATA, candidateStr)
            .add(Constant::KEY_MID, midStr)
            .build();
        
        QString message = JsonUtil::toCompactString(candidateMsg);
        
        if(m_sdpSent) {
            emit sendWsCliTextMsg(message);
            LOG_DEBUG("Sent local candidate to cli: {}", message);
        }else{
            m_localCandidates.append(message);
        } });

    // 设置轨道回调 - 使用onMessage接收解包后的H264数据
    if (m_videoTrack)
    {
        LOG_INFO("Setting up video track message callback");
        m_videoTrack->onFrame([this](rtc::binary data, rtc::FrameInfo info)
                              {
            LOG_DEBUG("Video frame received: {}, timestamp: {}", Convert::formatFileSize(data.size()), info.timestamp);
            processVideoFrame(data, info); });
        LOG_INFO("Video track message callback set");
    }

    if (m_audioTrack)
    {
        LOG_INFO("Setting up audio track message callback");
        m_audioTrack->onFrame([this](rtc::binary data, rtc::FrameInfo info)
                              {
            LOG_DEBUG("Audio frame received: {}, ts: {}", Convert::formatFileSize(data.size()), info.timestamp);
            processAudioFrame(data, info); });
        LOG_INFO("Audio track message callback set");
    }

    // 接收到远程轨道回调（备用，以防某些情况下需要）
    m_peerConnection->onTrack([this](std::shared_ptr<rtc::Track> track)
                              {
        QString trackMid = QString::fromStdString(track->mid());
        LOG_INFO("Control side received additional track: {}", trackMid); });

    // 接收到数据通道回调
    m_peerConnection->onDataChannel([this](std::shared_ptr<rtc::DataChannel> channel)
                                    {
        QString channelLabel = QString::fromStdString(channel->label());
        LOG_INFO("Control side received data channel: {}", channelLabel);

        if (channelLabel == Constant::TYPE_FILE) {
            m_fileChannel = channel;
            setupFileChannelCallbacks();
        } else if (channelLabel == Constant::TYPE_FILE_TEXT) {
            m_fileTextChannel = channel;
            setupFileTextChannelCallbacks();
        } else if (channelLabel == Constant::TYPE_INPUT) {
            m_inputChannel = channel;
            setupInputChannelCallbacks();
        } });
}

void WebRtcCtl::setupFileChannelCallbacks()
{
    if (!m_fileChannel)
        return;

    QString channelLabel = QString::fromStdString(m_fileChannel->label());

    m_fileChannel->onOpen([this, channelLabel]()
                          { LOG_INFO("File channel opened: {}", channelLabel); });

    m_fileChannel->onClosed([this, channelLabel]()
                            { LOG_INFO("File channel closed: {}", channelLabel); });

    m_fileChannel->onError([this, channelLabel](const std::string &error)
                           { LOG_ERROR("File channel error: {}", error); });

    m_fileChannel->onMessage([this, channelLabel](const rtc::message_variant &message)
                             {
        if (std::holds_alternative<rtc::binary>(message)) {
            auto binaryData = std::get<rtc::binary>(message);
            LOG_DEBUG("File channel received binary data: {}", Convert::formatFileSize(binaryData.size()));
            
            m_filePacketUtil->processReceivedFragment(binaryData, channelLabel);
        } else if (std::holds_alternative<std::string>(message)) {
            // 文件通道不再处理文本消息，记录警告
            LOG_WARN("File channel received text message, but should use file_text channel instead");
        } });
}

void WebRtcCtl::setupFileTextChannelCallbacks()
{
    if (!m_fileTextChannel)
        return;

    QString channelLabel = QString::fromStdString(m_fileTextChannel->label());

    m_fileTextChannel->onOpen([this, channelLabel]()
                              { LOG_INFO("File text channel opened: {}", channelLabel); });

    m_fileTextChannel->onClosed([this, channelLabel]()
                                { LOG_INFO("File text channel closed: {}", channelLabel); });

    m_fileTextChannel->onError([this, channelLabel](const std::string &error)
                               { LOG_ERROR("File text channel error: {}", error); });

    m_fileTextChannel->onMessage([this, channelLabel](const rtc::message_variant &message)
                                 {
        if (std::holds_alternative<std::string>(message)) {
            // 处理文件相关的文本消息（文件列表、上传响应等）
            std::string data = std::get<std::string>(message);
            QByteArray dataArr = QByteArray::fromStdString(data);
            LOG_DEBUG("File text channel received message: {}", QString::fromUtf8(dataArr));
            
            QJsonObject object = JsonUtil::safeParseObject(dataArr);
            if (JsonUtil::isValidObject(object)) {
                QString msgType = JsonUtil::getString(object, Constant::KEY_MSGTYPE);
                LOG_DEBUG("Parsed message type: {}", msgType);
                
                if (msgType == Constant::TYPE_UPLOAD_FILE_RES) {
                    // 处理上传响应
                    QString cliPath = JsonUtil::getString(object, Constant::KEY_PATH_CLI);
                    bool status = JsonUtil::getBool(object, "status");
                    LOG_INFO("Upload response: {} - {}", cliPath, status);
                    emit recvUploadFileRes(status, cliPath);
                } else if (msgType == Constant::TYPE_FILE_LIST) {
                    // 处理文件列表响应
                    LOG_INFO("Emitting recvGetFileList signal");
                    emit recvGetFileList(object);
                } else if (msgType == Constant::TYPE_FILE_DOWNLOAD) {
                    // 处理文件下载响应
                    LOG_INFO("Emitting recvFileDownload signal");
                    if(object.contains("directoryEnd")){
                        QString ctlPath = JsonUtil::getString(object, Constant::KEY_PATH_CTL);
                        emit recvDownloadFile(true, ctlPath);
                    }
                } else if (object.contains("type") && JsonUtil::getString(object, "type") == Constant::TYPE_KEYFRAME_RESPONSE) {
                    // 处理关键帧响应
                    LOG_INFO("🔑 Received key frame response from remote");
                } else {
                    // 处理其他文件相关响应
                    LOG_INFO("Emitting recvGetFileList signal for unknown type");
                    emit recvGetFileList(object);
                }
            } else {
                LOG_ERROR("Failed to parse JSON message: {}", QString::fromUtf8(dataArr));
            }
        } else {
            LOG_WARN("File text channel received binary data, ignoring");
        } });
}

void WebRtcCtl::setupInputChannelCallbacks()
{
    if (!m_inputChannel)
        return;

    QString channelLabel = QString::fromStdString(m_inputChannel->label());

    m_inputChannel->onOpen([this, channelLabel]()
                           { LOG_INFO("Input channel opened: {}", channelLabel); });

    m_inputChannel->onClosed([this, channelLabel]()
                             { LOG_INFO("Input channel closed: {}", channelLabel); });

    m_inputChannel->onError([this, channelLabel](const std::string &error)
                            { LOG_ERROR("Input channel error: {}", error); });

    m_inputChannel->onMessage([this, channelLabel](const rtc::message_variant &message)
                              {
        if (std::holds_alternative<std::string>(message)) {
            // 输入通道现在只处理输入事件相关的消息
            LOG_DEBUG("Input channel message received (control side)");
        } else {
            LOG_DEBUG("Input channel binary message received (control side)"); 
        } });
}

void WebRtcCtl::parseWsMsg(const QJsonObject &object)
{
    // 检查必要字段
    if (!JsonUtil::hasRequiredKeys(object, {Constant::KEY_ROLE, Constant::KEY_TYPE}))
        return;

    QString role = JsonUtil::getString(object, Constant::KEY_ROLE);
    QString type = JsonUtil::getString(object, Constant::KEY_TYPE);

    // 只处理来自被控端的消息
    if (role != Constant::ROLE_CLI)
    {
        return;
    }

    // 处理SDP消息（Offer/Answer）
    if (type == Constant::TYPE_OFFER || type == Constant::TYPE_ANSWER)
    {
        QString data = JsonUtil::getString(object, Constant::KEY_DATA);
        if (!data.isEmpty())
        {
            try
            {
                LOG_INFO("Setting remote description: {}", type);
                rtc::Description desc(data.toStdString(), type.toStdString());
                m_peerConnection->setRemoteDescription(desc);
                LOG_INFO("Remote description set successfully");
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Failed to set remote description: {}", e.what());
            }
        }
    }
    // 处理ICE候选者
    else if (type == Constant::TYPE_CANDIDATE)
    {
        QString candidateStr = JsonUtil::getString(object, Constant::KEY_DATA);
        QString mid = JsonUtil::getString(object, Constant::KEY_MID);

        if (!candidateStr.isEmpty() && !mid.isEmpty())
        {
            try
            {
                m_peerConnection->addRemoteCandidate(rtc::Candidate(candidateStr.toStdString(), mid.toStdString()));
                LOG_DEBUG("Added remote candidate");
            }
            catch (const std::exception &e)
            {
                LOG_ERROR("Failed to add remote candidate: {}", e.what());
            }
        }
    }
}

void WebRtcCtl::onWsCliRecvBinaryMsg(const QByteArray &message)
{
    parseWsMsg(JsonUtil::safeParseObject(message));
}

void WebRtcCtl::onWsCliRecvTextMsg(const QString &message)
{
    parseWsMsg(JsonUtil::safeParseObject(message.toUtf8()));
}

void WebRtcCtl::inputChannelSendMsg(const rtc::message_variant &data)
{
    LOG_DEBUG("inputChannelSendMsg called - connected: {}, inputChannel: {}, isOpen: {}",
              m_connected,
              (m_inputChannel != nullptr),
              (m_inputChannel && m_inputChannel->isOpen()));

    if (m_connected && m_inputChannel && m_inputChannel->isOpen())
    {
        try
        {
            m_inputChannel->send(data);
            LOG_DEBUG("Successfully sent input channel message {}", std::get<std::string>(data));
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to send input channel message: {}", e.what());
        }
    }
    else
    {
        LOG_WARN("Input channel not ready for sending - connected: {}, channel exists: {}, channel open: {}",
                 m_connected,
                 (m_inputChannel != nullptr),
                 (m_inputChannel && m_inputChannel->isOpen()));
    }
}

void WebRtcCtl::fileChannelSendMsg(const rtc::message_variant &data)
{
    LOG_DEBUG("fileChannelSendMsg called - connected: {}, fileChannel: {}, isOpen: {}",
              m_connected,
              (m_fileChannel != nullptr),
              (m_fileChannel && m_fileChannel->isOpen()));

    if (m_connected && m_fileChannel && m_fileChannel->isOpen())
    {
        try
        {
            m_fileChannel->send(data);
            LOG_DEBUG("Successfully sent file channel message");
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to send file channel message: {}", e.what());
        }
    }
    else
    {
        LOG_WARN("File channel not ready for sending - connected: {}, channel exists: {}, channel open: {}",
                 m_connected,
                 (m_fileChannel != nullptr),
                 (m_fileChannel && m_fileChannel->isOpen()));
    }
}

void WebRtcCtl::fileTextChannelSendMsg(const rtc::message_variant &data)
{
    LOG_DEBUG("fileTextChannelSendMsg called - connected: {}, fileTextChannel: {}, isOpen: {}",
              m_connected,
              (m_fileTextChannel != nullptr),
              (m_fileTextChannel && m_fileTextChannel->isOpen()));

    if (m_connected && m_fileTextChannel && m_fileTextChannel->isOpen())
    {
        try
        {
            m_fileTextChannel->send(data);
            LOG_DEBUG("Successfully sent file text channel message");
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Failed to send file text channel message: {}", e.what());
        }
    }
    else
    {
        LOG_WARN("File text channel not ready for sending - connected: {}, channel exists: {}, channel open: {}",
                 m_connected,
                 (m_fileTextChannel != nullptr),
                 (m_fileTextChannel && m_fileTextChannel->isOpen()));
    }
}

void WebRtcCtl::uploadFile2CLI(const QString &ctlPath, const QString &cliPath)
{
    LOG_WARN("uploadFile2CLI called: {} -> {}", ctlPath, cliPath);

    if (!m_fileChannel || !m_fileChannel->isOpen())
    {
        LOG_ERROR("File channel not available");
        emit recvUploadFileRes(false, ctlPath);
        return;
    }

    // 构造完整的本地文件路径
    QString fullLocalPath = ctlPath;

    QFileInfo fileInfo(ctlPath);
    if (!fileInfo.exists())
    {
        LOG_ERROR("File does not exist: {}", ctlPath);
        emit recvUploadFileRes(false, ctlPath);
        return;
    }

    if (fileInfo.isFile())
    {
        // 上传单个文件
        uploadSingleFile(ctlPath, cliPath);
    }
    else if (fileInfo.isDir())
    {
        // 上传目录
        uploadDirectory(ctlPath, cliPath);
    }
    else
    {
        LOG_ERROR("Unknown file type: {}", ctlPath);
        emit recvUploadFileRes(false, ctlPath);
    }
}

void WebRtcCtl::uploadSingleFile(const QString &ctlPath, const QString &cliPath)
{
    QFileInfo fileInfo(ctlPath);
    if (!fileInfo.exists() || !fileInfo.isFile())
    {
        LOG_ERROR("File does not exist or is not a regular file: {}", ctlPath);
        emit recvUploadFileRes(false, cliPath);
        return;
    }

    // 创建包含文件信息的头部
    QJsonObject header = JsonUtil::createObject()
                             .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                             .add(Constant::KEY_PATH_CTL, ctlPath)
                             .add(Constant::KEY_PATH_CLI, cliPath)
                             .add(Constant::KEY_FILE_SIZE, static_cast<double>(fileInfo.size()))
                             .add("isDirectory", false)
                             .build();

    // 使用流式发送方法，避免将大文件加载到内存
    if (m_fileChannel && m_fileChannel->isOpen())
    {
        try
        {
            if (FilePacketUtil::sendFileStream(ctlPath, header, m_fileChannel))
            {
                LOG_INFO("Sent file stream: {} -> {} ({})",
                         ctlPath, cliPath, Convert::formatFileSize(fileInfo.size()));
            }
            else
            {
                LOG_ERROR("Failed to send file stream: {}", ctlPath);
                emit recvUploadFileRes(false, cliPath);
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Exception during file stream send: {}", e.what());
            emit recvUploadFileRes(false, cliPath);
        }
    }
    else
    {
        LOG_ERROR("File channel not available for uploading file");
        emit recvUploadFileRes(false, cliPath);
    }
}

void WebRtcCtl::uploadDirectory(const QString &ctlPath, const QString &cliPath)
{
    QDir dir(ctlPath);
    dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    dir.setSorting(QDir::Name | QDir::DirsFirst); // 按名称排序，文件夹优先
    // 遍历条目并分类
    QFileInfoList list = dir.entryInfoList();
    // 首先发送目录开始标记
    QJsonObject dirStartHeader = JsonUtil::createObject()
                                     .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                                     .add(Constant::KEY_PATH_CTL, ctlPath)
                                     .add(Constant::KEY_PATH_CLI, cliPath)
                                     .add("isDirectory", true)
                                     .add("directoryStart", true)
                                     .build();

    QByteArray dirStartHeaderBytes = JsonUtil::toCompactBytes(dirStartHeader);
    rtc::binary dirStartData;
    dirStartData.resize(dirStartHeaderBytes.size());
    std::memcpy(dirStartData.data(), dirStartHeaderBytes.constData(), dirStartHeaderBytes.size());
    fileTextChannelSendMsg(dirStartData);

    int fileCount = 0;
    bool hasErrors = false;

    for (const QFileInfo &fileInfo : list)
    {
        if (fileInfo.isFile())
        {
            QString relativePath = dir.relativeFilePath(fileInfo.absoluteFilePath());

            // 在远程路径中包含目录名
            QString fullRemotePath = QDir::cleanPath(cliPath + "/" + relativePath);

            // 发送单个文件（使用原始文件名作为显示名称）
            uploadSingleFile(fileInfo.absoluteFilePath(), fullRemotePath);
            fileCount++;
        }
    }

    if (fileCount == 0)
    {
        LOG_WARN("No files found in directory: {}", ctlPath);
        emit recvUploadFileRes(false, cliPath);
    }
    else
    {
        LOG_INFO("Uploaded directory: {} -> {} ({} files)", ctlPath, cliPath, fileCount);
        // 对于目录上传，我们假设成功（单个文件的错误会单独处理）
        emit recvUploadFileRes(true, cliPath);
    }
    // 发送目录结束标记
    QJsonObject dirEndHeader = JsonUtil::createObject()
                                   .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                                   .add(Constant::KEY_PATH_CTL, ctlPath)
                                   .add(Constant::KEY_PATH_CLI, cliPath)
                                   .add("isDirectory", true)
                                   .add("directoryEnd", true)
                                   .add("fileCount", fileCount)
                                   .build();

    QByteArray dirEndHeaderBytes = JsonUtil::toCompactBytes(dirEndHeader);
    rtc::binary dirEndData;
    dirEndData.resize(dirEndHeaderBytes.size());
    std::memcpy(dirEndData.data(), dirEndHeaderBytes.constData(), dirEndHeaderBytes.size());
    fileTextChannelSendMsg(dirEndData);

    LOG_INFO("Sent directory: {} -> {} ({} files)", ctlPath, cliPath, fileCount);
}

void WebRtcCtl::destroy()
{
    LOG_DEBUG("WebRtcCtl destroy started");
    m_connected = false;

    // 清理文件分包工具
    if (m_filePacketUtil)
    {
        m_filePacketUtil = nullptr;
    }

    // 清理数据通道（按顺序清理）
    if (m_inputChannel)
    {
        LOG_DEBUG("Cleaning up input channel");
        m_inputChannel->resetCallbacks();
        m_inputChannel->close();
        m_inputChannel = nullptr;
    }

    if (m_fileChannel)
    {
        LOG_DEBUG("Cleaning up file channel");
        m_fileChannel->resetCallbacks();
        m_fileChannel->close();
        m_fileChannel = nullptr;
    }

    if (m_fileTextChannel)
    {
        LOG_DEBUG("Cleaning up file text channel");
        m_fileTextChannel->resetCallbacks();
        m_fileTextChannel->close();
        m_fileTextChannel = nullptr;
    }

    // 清理轨道
    if (m_audioTrack)
    {
        LOG_DEBUG("Cleaning up audio track");
        m_audioTrack->resetCallbacks();
        m_audioTrack->close();
        m_audioTrack = nullptr;
    }

    if (m_videoTrack)
    {
        LOG_DEBUG("Cleaning up video track");
        m_videoTrack->resetCallbacks();
        m_videoTrack->close();
        m_videoTrack = nullptr;
    }

    // 清理媒体播放器
    if (m_mediaPlayer)
    {
        LOG_DEBUG("Stopping media player");
        m_mediaPlayer->stopPlayback();
        m_mediaPlayer = nullptr;
    }

    // 最后清理PeerConnection
    if (m_peerConnection)
    {
        LOG_DEBUG("Cleaning up peer connection");
        m_peerConnection->resetCallbacks();
        m_peerConnection->close();
        m_peerConnection = nullptr;
    }

    LOG_INFO("WebRtcCtl destroyed");
}

// 处理接收到的音频数据
void WebRtcCtl::processAudioFrame(const rtc::binary &audioData, const rtc::FrameInfo &frameInfo)
{
    LOG_DEBUG("Received audio frame: {}", Convert::formatFileSize(audioData.size()));

    if (audioData.empty())
    {
        LOG_WARN("Received empty audio frame");
        return;
    }

    try
    {
        // 将音频数据发送给媒体播放器
        if (m_mediaPlayer)
        {
            m_mediaPlayer->playAudioData(audioData);
        }
        else
        {
            LOG_WARN("MediaPlayer not initialized");
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Error processing audio frame: {}", e.what());
    }
}

// 处理接收到的视频数据
void WebRtcCtl::processVideoFrame(const rtc::binary &data, const rtc::FrameInfo &frameInfo)
{
    LOG_DEBUG("Received video frame: {}", Convert::formatFileSize(data.size()));

    if (data.empty())
    {
        return;
    }

    try
    {
        // 限制解码频率，避免内存压力过大
        static auto lastDecodeTime = std::chrono::steady_clock::now();
        static int frameDropCount = 0;
        static int minInterval = 1000 / ConfigUtil->fps; // 默认30fps
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastDecodeTime);

        if (elapsed.count() < minInterval)
        {
            frameDropCount++;
            if (frameDropCount % 100 == 0)
            {
                LOG_DEBUG("Dropped {} frames to reduce memory pressure", frameDropCount);
            }
            return;
        }
        lastDecodeTime = currentTime;

        // 解码H264数据为QImage
        if (m_h264Decoder)
        {
            // 解码器等待关键帧时：周期性请求关键帧（避免只触发一次，后续一直等不到 IDR）
            if (m_h264Decoder->isWaitingForKeyFrame())
            {
                static auto lastKeyframeRequestTime = std::chrono::steady_clock::now() - std::chrono::seconds(10);
                const auto now = std::chrono::steady_clock::now();
                const auto gapMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastKeyframeRequestTime).count();

                // 这里用节流避免刷屏/打爆信令；可按需调小/调大
                const int kRequestIntervalMs = 500;
                if (gapMs >= kRequestIntervalMs)
                {
                    LOG_WARN("⚠️ Decoder is waiting for key frame (waitFlag={}, gapMs={}), requesting...", m_waitingForKeyFrame, (int)gapMs);

                    // 1) 业务信令请求（datachannel）
                    requestKeyFrame();

                    // 2) WebRTC 原生 RTCP PLI/FIR 请求
                    if (m_videoTrack)
                    {
                        try
                        {
                            LOG_DEBUG("Calling videoTrack->requestKeyframe()...");
                            bool ok = m_videoTrack->requestKeyframe();
                            LOG_INFO("Requested keyframe via videoTrack (RTCP PLI/FIR), ok={}", ok);
                        }
                        catch (const std::exception &e)
                        {
                            LOG_WARN("Failed to request keyframe via videoTrack: {}", e.what());
                        }
                    }
                    else
                    {
                        LOG_DEBUG("videoTrack is null, skip requestKeyframe");
                    }

                    lastKeyframeRequestTime = now;
                    m_waitingForKeyFrame = true;
                }
            }

            QImage decodedFrame = m_h264Decoder->decodeFrame(data);
            if (!decodedFrame.isNull())
            {
                emit videoFrameDecoded(decodedFrame);
                m_waitingForKeyFrame = false;
                LOG_DEBUG("Successfully decoded video frame: {}x{}", decodedFrame.width(), decodedFrame.height());
            }
            else
            {
                // 解码失败处理 - 只在不是等待更多数据时请求关键帧
                static int consecutiveFailures = 0;
                consecutiveFailures++;
                
                if (consecutiveFailures >= 5 && !m_waitingForKeyFrame)
                {
                    LOG_WARN("⚠️ {} consecutive decode failures, requesting key frame", consecutiveFailures);
                    requestKeyFrame();
                    m_waitingForKeyFrame = true;
                    consecutiveFailures = 0;
                }
            }
        }
        else
        {
            LOG_WARN("H264 decoder not initialized");
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Error processing video frame: {}", e.what());
    }
}

// 请求关键帧
void WebRtcCtl::requestKeyFrame()
{
    if (!m_inputChannel || !m_inputChannel->isOpen())
    {
        LOG_WARN("Input channel not available for key frame request");
        return;
    }

    try
    {
        QJsonObject keyFrameRequest = JsonUtil::createObject()
                                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_KEYFRAME_REQUEST)
                                          .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                          .add(Constant::KEY_RECEIVER, m_remoteId)
                                          .add(Constant::KEY_RECEIVER_PWD, m_remotePwdMd5)
                                          .add("timestamp", QDateTime::currentMSecsSinceEpoch())
                                          .add("reason", "network_error_recovery")
                                          .build();

        QString message = JsonUtil::toCompactString(keyFrameRequest);
        m_inputChannel->send(message.toStdString());

        LOG_INFO("🔑 Requested key frame for error recovery via inputChannel");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send key frame request: {}", e.what());
    }
}
