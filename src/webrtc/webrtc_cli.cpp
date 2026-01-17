#include "webrtc_cli.h"
#include "constant.h"
#include "convert.h"
#include "util/json_util.h"
#include "util/file_packet_util.h"
#include "logger_manager.h"
#include "media_capture.h"
#include <QStorageInfo>
#include <QDir>
#include <QUuid>
#include <QDataStream>
#include <QtConcurrent>
#include <QGuiApplication>
#include <QScreen>
#include <QThread>
#include <iostream>

/**
 * WebRtcCli类实现
 * 该类负责处理WebRTC客户端的所有功能，包括连接、媒体处理、数据
 * init pc -> setup tracks -> create data channels -> on gathering complete ->send local sdp
 * -> on remote sdp -> set remote description -> send ice candidates
 * -> on ice candidate -> add ice candidate
 */
WebRtcCli::WebRtcCli(const QString &remoteId, int fps, bool isOnlyFile,
                     int controlMaxWidth, int controlMaxHeight, bool isOnlyRelay, QObject *parent)
    : QObject(parent),
      m_remoteId(remoteId),
      m_isOnlyFile(isOnlyFile), // 默认不是仅文件传输
      m_currentDir(QDir::home()),
      m_connected(false),
      m_channelsReady(false),
      m_sdpSent(false), // 初始状态未发送本地描述
      m_destroying(false),
      m_fps(fps),
      m_mediaCapture(nullptr),
      m_onlyRelay(isOnlyRelay)
{

    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
    m_screen_width = screenGeometry.width();
    m_screen_height = screenGeometry.height();

    // 根据控制端最大显示区域和被控端实际分辨率计算合适的编码分辨率
    calculateOptimalResolution(controlMaxWidth, controlMaxHeight);

    // 初始化ICE服务器配置
    m_host = ConfigUtil->ice_host.toStdString();
    m_port = (uint16_t)ConfigUtil->ice_port;
    m_username = ConfigUtil->ice_username.toStdString();
    m_password = ConfigUtil->ice_password.toStdString();

    // 初始化时间戳
    m_baseTimestamp = QDateTime::currentMSecsSinceEpoch();

    // 初始化文件分包工具类
    m_filePacketUtil = new FilePacketUtil(this);

    // 连接文件接收信号
    connect(m_filePacketUtil, &FilePacketUtil::fileDownloadCompleted, this, &WebRtcCli::handleFileReceived);
    connect(m_filePacketUtil, &FilePacketUtil::fileReceived, this, &WebRtcCli::handleFileReceived);

    LOG_INFO("created for remote: {}", m_remoteId);
}

WebRtcCli::~WebRtcCli()
{
    LOG_DEBUG("WebRtcCli destructor");

    // 先调用destroy停止所有活动
    destroy();

    // 确保媒体捕获完全停止后再删除
    if (m_mediaCapture)
    {
        // 断开信号连接避免回调到已析构的对象
        disconnect(m_mediaCapture, nullptr, this, nullptr);

        // 使用 deleteLater() 进行线程安全的删除
        m_mediaCapture->deleteLater();
        m_mediaCapture = nullptr;
    }
}

void WebRtcCli::init()
{
    LOG_INFO("Creating PeerConnection and tracks for client side");

    // 初始化媒体捕获
    if (!m_isOnlyFile && !m_mediaCapture)
    {
        m_mediaCapture = new MediaCapture(); // 移除父对象参数
        connect(m_mediaCapture, &MediaCapture::videoFrameReady, this, &WebRtcCli::onVideoFrameReady);
        connect(m_mediaCapture, &MediaCapture::audioFrameReady, this, &WebRtcCli::onAudioFrameReady);

        // 连接关键帧请求信号
        connect(this, &WebRtcCli::requestKeyFrameFromCapture, m_mediaCapture, &MediaCapture::requestKeyFrame);
    }

    // 初始化WebRTC
    initPeerConnection();

    setupCallbacks();
    // 创建轨道和数据通道
    createTracksAndChannels();
}

void WebRtcCli::populateLocalFiles()
{
    // 获取已挂载的驱动器路径
    QJsonArray mountedPaths;
    QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes)
    {
        if (volume.isValid() && volume.isReady())
        {
            mountedPaths.append(volume.rootPath());
        }
    }

    m_currentDir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    m_currentDir.setSorting(QDir::Name | QDir::DirsFirst);

    QFileInfoList list = m_currentDir.entryInfoList();

    QJsonArray fileArray;
    for (const QFileInfo &entry : list)
    {
        QJsonObject fileObj = JsonUtil::createObject()
                                  .add(Constant::KEY_NAME, entry.fileName())
                                  .add(Constant::KEY_IS_DIR, entry.isDir())
                                  .add(Constant::KEY_FILE_SIZE, static_cast<double>(entry.size()))
                                  .add(Constant::KEY_FILE_LAST_MOD_TIME, entry.lastModified().toString(Qt::ISODate))
                                  .build();
        fileArray.append(fileObj);
    }

    QJsonObject responseMsg = JsonUtil::createObject()
                                  .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                  .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_LIST)
                                  .add(Constant::KEY_PATH, m_currentDir.absolutePath())
                                  .add(Constant::KEY_FOLDER_FILES, fileArray)
                                  .add(Constant::KEY_FOLDER_MOUNTED, mountedPaths)
                                  .build();

    sendFileTextChannelMessage(responseMsg);
}

// WebRTC核心功能
void WebRtcCli::initPeerConnection()
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

void WebRtcCli::createTracksAndChannels()
{
    if (!m_peerConnection)
    {
        LOG_ERROR("PeerConnection not available for creating tracks");
        return;
    }

    try
    {
        if (!m_isOnlyFile)
        {
            // 创建视频轨道 - 严格按照官方示例配置
            LOG_INFO("Creating video track");
            std::string video_name = Constant::TYPE_VIDEO.toStdString();
            rtc::Description::Video videoDesc(video_name); // 使用固定流名称匹配接收端
            videoDesc.addH264Codec(96);                    // H264 payload type

            // 设置SSRC和媒体流标识 - 关键配置
            uint32_t videoSSRC = 1;
            std::string msid = Constant::TYPE_VIDEO_MSID.toStdString();
            videoDesc.addSSRC(videoSSRC, video_name, msid, video_name);
            videoDesc.setDirection(rtc::Description::Direction::SendOnly);
            m_videoTrack = m_peerConnection->addTrack(videoDesc);

            // 为视频轨道设置RTP打包器链
            auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(videoSSRC, video_name, 96, rtc::H264RtpPacketizer::ClockRate);
            // 使用StartSequence分隔符，因为FFMPEG输出的是Annex-B格式（带有0x00000001起始码）
            auto h264Packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfig);

            // 添加RTCP SR报告器
            auto srReporter = std::make_shared<rtc::RtcpSrReporter>(rtpConfig);
            h264Packetizer->addToChain(srReporter);

            // 添加RTCP NACK响应器
            auto nackResponder = std::make_shared<rtc::RtcpNackResponder>();
            h264Packetizer->addToChain(nackResponder);

            m_videoTrack->setMediaHandler(h264Packetizer);

            // 创建音频轨道
            LOG_INFO("Creating audio track");
            rtc::Description::Audio audioDesc(Constant::TYPE_AUDIO.toStdString()); // 使用固定流名称匹配接收端
            audioDesc.addOpusCodec(111);                                           // Opus payload type

            // 设置SSRC和媒体流标识
            uint32_t audioSSRC = 2;
            audioDesc.addSSRC(audioSSRC, Constant::TYPE_AUDIO.toStdString(), msid, Constant::TYPE_AUDIO.toStdString());
            audioDesc.setDirection(rtc::Description::Direction::SendOnly);
            m_audioTrack = m_peerConnection->addTrack(audioDesc);

            // 创建输入数据通道
            LOG_INFO("Creating input data channel");
            m_inputChannel = m_peerConnection->createDataChannel(Constant::TYPE_INPUT.toStdString());
            setupInputChannelCallbacks();
        }
        // 创建文件数据通道（用于二进制文件传输）
        LOG_INFO("Creating file data channel");
        m_fileChannel = m_peerConnection->createDataChannel(Constant::TYPE_FILE.toStdString());
        setupFileChannelCallbacks();

        // 创建文件文本数据通道（用于文件列表、目录切换等文本消息）
        LOG_INFO("Creating file text data channel");
        m_fileTextChannel = m_peerConnection->createDataChannel(Constant::TYPE_FILE_TEXT.toStdString());
        setupFileTextChannelCallbacks();

        m_channelsReady = true;
        LOG_INFO("All tracks and channels created successfully");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to create tracks and channels: {}", e.what());
    }
}

void WebRtcCli::setupCallbacks()
{
    if (!m_peerConnection)
        return;

    // 连接状态回调
    m_peerConnection->onStateChange([this](rtc::PeerConnection::State state)
                                    {
        // 如果正在销毁，不处理回调
        if (m_destroying) {
            LOG_DEBUG("Ignoring state change callback during destruction");
            return;
        }
        m_connected = (state == rtc::PeerConnection::State::Connected);

        std::string stateStr;
        if(m_connected){
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
        LOG_INFO("Client side connection state: {}", stateStr);
        if(m_isOnlyFile){
            return; // 仅文件传输模式不处理连接状态
        }
        if (m_connected) {
            LOG_INFO("WebRTC connection established, starting media capture");
            startMediaCapture();
        }else if(state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed) {
            LOG_INFO("WebRTC connection lost, stopping media capture");
            stopMediaCapture();
        } });

    // ICE连接状态回调
    m_peerConnection->onIceStateChange([this](rtc::PeerConnection::IceState state)
                                       {
        std::string stateStr;
        if (state == rtc::PeerConnection::IceState::Connected)
        {
            stateStr = "Connected";
        }
        else if (state == rtc::PeerConnection::IceState::Checking)
        {
            stateStr = "Checking";
        }
        else if (state == rtc::PeerConnection::IceState::New)
        {
            stateStr = "New";
        }
        else if (state == rtc::PeerConnection::IceState::Failed)
        {
            stateStr = "Failed";
        }
        else if (state == rtc::PeerConnection::IceState::Disconnected)
        {
            stateStr = "Disconnected";
        }
        else if (state == rtc::PeerConnection::IceState::Closed)
        {
            stateStr = "Closed";
        }
        else if (state == rtc::PeerConnection::IceState::Completed)
        {
            stateStr = "Completed";
        }
        else
        {
            stateStr = "Unknown";
        }
        LOG_INFO("Client side ICE state: {}", stateStr); });

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
                                                LOG_DEBUG("Client side gathering state: {}", stateStr); });

    m_peerConnection->onLocalDescription([this](rtc::Description description)
                                         {
               try {
		            QString sdp = QString::fromStdString(std::string(description));
                    QString type = QString::fromStdString(description.typeString());
                    if(type == Constant::TYPE_ANSWER){
                        return;
                    }
                    // 发送本地描述给控制端
                    QJsonObject offerMsg = JsonUtil::createObject()
                    .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                    .add(Constant::KEY_TYPE, type)
                    .add(Constant::KEY_RECEIVER, m_remoteId)
                    .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                    .add(Constant::KEY_DATA, sdp)
                    .build();
                    
                    QString message = JsonUtil::toCompactString(offerMsg);
                    emit sendWsCliTextMsg(message);
                    LOG_INFO("Sent local description ({}) to ctl", message);
                    m_sdpSent=true; // 标记已发送本地描述
                    if(!m_localCandidates.isEmpty()){
                        foreach(const QString &candidate, m_localCandidates)
                        {
                            emit sendWsCliTextMsg(candidate);
                            LOG_DEBUG("Sent local candidate to cli: {}", candidate);
                        }
                        m_localCandidates.clear(); // 清空已发送的候选者
                    }
                } catch (const std::exception &e) {
                LOG_ERROR("Failed to send local description: {}", e.what());
            } });
    // 本地候选者回调
    m_peerConnection->onLocalCandidate([this](const rtc::Candidate &candidate)
                                       {
        
        QString candidateStr = QString::fromStdString(std::string(candidate));
        QString midStr = QString::fromStdString(candidate.mid());
        
        // 发送本地候选者给控制端
        QJsonObject candidateMsg = JsonUtil::createObject()
            .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
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
}

void WebRtcCli::setupFileChannelCallbacks()
{
    if (!m_fileChannel)
        return;

    m_fileChannel->onOpen([this]()
                          { LOG_INFO("File channel opened"); });

    m_fileChannel->onMessage([this](auto data)
                             {
        if (std::holds_alternative<rtc::binary>(data)) {
            auto binaryData = std::get<rtc::binary>(data);
            LOG_DEBUG("File channel received binary data: {}", Convert::formatFileSize(binaryData.size()));
            // 所有数据都按分包格式处理
            m_filePacketUtil->processReceivedFragment(binaryData, "file");
        } else if (std::holds_alternative<std::string>(data)) {
            // 文件通道不再处理文本消息，记录警告
            LOG_WARN("File channel received text message, but should use file_text channel instead");
        } });

    m_fileChannel->onError([](std::string error)
                           { LOG_ERROR("File channel error: {}", error); });

    m_fileChannel->onClosed([this]()
                            { 
                                LOG_INFO("File channel closed"); 
                                if(m_isOnlyFile) {
                                    // 仅文件传输模式下，销毁客户端
                                    emit destroyCli();
                                } });
}

void WebRtcCli::setupFileTextChannelCallbacks()
{
    if (!m_fileTextChannel)
        return;

    m_fileTextChannel->onOpen([this]()
                              {
                                  LOG_INFO("File text channel opened");
                                  populateLocalFiles(); // 在文本通道开启时发送初始文件列表
                              });

    m_fileTextChannel->onMessage([this](auto data)
                                 {
        if (std::holds_alternative<std::string>(data)) {
            // 处理来自控制端的文件文本消息
            std::string message = std::get<std::string>(data);
            QString msgStr = QString::fromUtf8(message.c_str(), message.length());
            LOG_DEBUG("File text channel received message: {}", msgStr);
            
            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(msgStr.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                LOG_ERROR("File text channel message parse error: {}", parseError.errorString());
                return;
            }
            
            QJsonObject object = doc.object();
            parseFileMsg(object);
        } else {
            LOG_WARN("File text channel received binary data, ignoring");
        } });

    m_fileTextChannel->onError([](std::string error)
                               { LOG_ERROR("File text channel error: {}", error); });

    m_fileTextChannel->onClosed([]()
                                { LOG_INFO("File text channel closed"); });
}

void WebRtcCli::setupInputChannelCallbacks()
{
    if (!m_inputChannel)
        return;

    m_inputChannel->onOpen([this]()
                           { LOG_INFO("Input channel opened"); });

    m_inputChannel->onMessage([this](auto data)
                              {
        if (std::holds_alternative<std::string>(data)) {
            // 处理来自控制端的输入消息
            std::string message = std::get<std::string>(data);
            QString msgStr = QString::fromUtf8(message.c_str(), message.length());
            
            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(msgStr.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                LOG_ERROR("Input channel message parse error: {}", parseError.errorString());
                return;
            }
            
            QJsonObject object = doc.object();
            parseInputMsg(object);
        } });

    m_inputChannel->onError([](std::string error)
                            { LOG_ERROR("Input channel error: {}", error); });

    m_inputChannel->onClosed([]()
                             { LOG_INFO("Input channel closed"); });
}

void WebRtcCli::destroy()
{
    disconnect();
    // 设置销毁标志防止回调执行
    m_destroying = true;
    m_connected = false;
    m_channelsReady = false;
    // 停止媒体捕获
    if (m_mediaCapture)
    {
        m_mediaCapture->stopCapture();
        m_mediaCapture->stopAudioCapture();
    }

    // 清理轨道和通道
    if (m_videoTrack)
    {
        m_videoTrack.reset();
    }
    if (m_audioTrack)
    {
        m_audioTrack.reset();
    }
    if (m_fileChannel)
    {
        m_fileChannel.reset();
    }
    if (m_fileTextChannel)
    {
        m_fileTextChannel.reset();
    }
    if (m_inputChannel)
    {
        m_inputChannel.reset();
    }

    // 关闭PeerConnection
    if (m_peerConnection)
    {
        m_peerConnection->close();
        m_peerConnection.reset();
    }

    // 清理分包数据
    m_uploadFragments.clear();

    LOG_INFO("WebRtcCli destroyed");
}

// WebSocket消息处理
void WebRtcCli::onWsCliRecvBinaryMsg(const QByteArray &message)
{
    parseWsMsg(JsonUtil::safeParseObject(message));
}

void WebRtcCli::onWsCliRecvTextMsg(const QString &message)
{
    parseWsMsg(JsonUtil::safeParseObject(message.toUtf8()));
}

// 简化实现的存根方法
void WebRtcCli::parseWsMsg(const QJsonObject &object)
{
    QString type = JsonUtil::getString(object, Constant::KEY_TYPE);
    if (type.isEmpty())
    {
        LOG_ERROR("parseWsMsg: Missing or empty message type");
        return;
    }

    // 处理来自控制端的信令消息
    if (type == Constant::TYPE_OFFER || type == Constant::TYPE_ANSWER)
    {
        QString data = JsonUtil::getString(object, Constant::KEY_DATA);
        if (!data.isEmpty())
        {
            setRemoteDescription(data, type);
            LOG_DEBUG("parseWsMsg: Processed {} message", type);
        }
        else
        {
            LOG_ERROR("parseWsMsg: Empty data for {} message", type);
        }
    }
    else if (type == Constant::TYPE_CANDIDATE)
    {
        QString data = JsonUtil::getString(object, Constant::KEY_DATA);
        QString mid = JsonUtil::getString(object, Constant::KEY_MID);
        if (!data.isEmpty())
        {
            addIceCandidate(data, mid);
            LOG_DEBUG("parseWsMsg: Processed candidate message");
        }
        else
        {
            LOG_ERROR("parseWsMsg: Empty data for candidate message");
        }
    }
}
void WebRtcCli::parseFileMsg(const QJsonObject &object)
{
    QString msgType = JsonUtil::getString(object, Constant::KEY_MSGTYPE);
    if (msgType.isEmpty())
    {
        LOG_ERROR("parseFileMsg: Missing msgType");
        return;
    }

    if (msgType == Constant::TYPE_FILE_LIST)
    {
        QString path = JsonUtil::getString(object, Constant::KEY_PATH);
        LOG_INFO("Processing file list request for path: {}", path);
        if (path.isEmpty())
        {
            LOG_ERROR("parseFileMsg: Missing path for file list request");
            return;
        }
        if (path == Constant::FOLDER_HOME)
        {
            m_currentDir = QDir::home();
        }
        else
        {
            m_currentDir.setPath(path);
        }

        populateLocalFiles();
    }
    else if (msgType == Constant::TYPE_FILE_DOWNLOAD)
    {
        QString cliPath = JsonUtil::getString(object, Constant::KEY_PATH_CLI);
        QString ctlPath = JsonUtil::getString(object, Constant::KEY_PATH_CTL);
        if (cliPath.isEmpty() || ctlPath.isEmpty())
        {
            LOG_ERROR("parseFileMsg: Missing file paths for download request");
            return;
        }
        sendFile(cliPath, ctlPath);
    }
    else if (msgType == Constant::TYPE_FILE_UPLOAD)
    {
        // 上传文件现在通过文件通道的二进制数据处理，不再需要输入通道处理
        LOG_INFO("File upload request received, waiting for binary data on file channel");
    }
    else
    {
        LOG_WARNING("parseFileMsg: Unknown message type: {}", msgType);
    }
}
void WebRtcCli::parseInputMsg(const QJsonObject &object)
{
    QString msgType = JsonUtil::getString(object, Constant::KEY_MSGTYPE);
    if (msgType.isEmpty())
    {
        LOG_ERROR("parseInputMsg: Missing msgType");
        return;
    }
    QString senderId = JsonUtil::getString(object, Constant::KEY_SENDER);
    if (senderId.isEmpty() || senderId != m_remoteId)
    {
        LOG_WARNING("parseInputMsg: Ignoring message from unknown sender: {}", senderId);
        return;
    }
    QString remoteId = JsonUtil::getString(object, Constant::KEY_RECEIVER);
    QString remotePwd = JsonUtil::getString(object, Constant::KEY_RECEIVER_PWD);
    if (remoteId.isEmpty() || remoteId != ConfigUtil->local_id || remotePwd != ConfigUtil->local_pwd_md5)
    {
        LOG_WARNING("parseInputMsg: Ignoring message for unknown receiver: {}, expected: {}, pwd: {}, expected: {}",
                    remoteId, ConfigUtil->local_id, remotePwd, ConfigUtil->local_pwd_md5);
        return;
    }
    if (msgType == Constant::TYPE_MOUSE)
    {
        // 处理鼠标事件
        handleMouseEvent(object);
    }
    else if (msgType == Constant::TYPE_KEYBOARD)
    {
        // 处理键盘事件
        handleKeyboardEvent(object);
    }
    else if (msgType == Constant::TYPE_KEYFRAME_REQUEST)
    {
        // 处理来自控制端的关键帧请求
        LOG_INFO("🔑 Received key frame request from control side");

        // 通知媒体捕获组件生成关键帧
        if (m_mediaCapture)
        {
            emit requestKeyFrameFromCapture();
        }

        // 发送响应确认
        QJsonObject response = JsonUtil::createObject()
                                   .add(Constant::KEY_MSGTYPE, Constant::TYPE_KEYFRAME_RESPONSE)
                                   .add(Constant::KEY_SENDER, ConfigUtil->local_id)
                                   .add(Constant::KEY_RECEIVER, m_remoteId)
                                   .add("timestamp", QDateTime::currentMSecsSinceEpoch())
                                   .add("status", "requested")
                                   .build();

        sendInputChannelMessage(response);
        LOG_INFO("🔑 Sent key frame response to control side");
    }
    else
    {
        LOG_WARNING("parseInputMsg: Unknown input message type: {}", msgType);
    }
}
void WebRtcCli::setRemoteDescription(const QString &data, const QString &type)
{
    if (!m_peerConnection)
        return;

    try
    {
        rtc::Description::Type descType;
        if (type == Constant::TYPE_OFFER)
        {
            descType = rtc::Description::Type::Offer;
        }
        else if (type == Constant::TYPE_ANSWER)
        {
            descType = rtc::Description::Type::Answer;
        }
        else
        {
            LOG_ERROR("Unknown description type: {}", type);
            return;
        }

        rtc::Description description(data.toStdString(), descType);
        m_peerConnection->setRemoteDescription(description);

        LOG_INFO("Set remote description: {}", type);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to set remote description: {}", e.what());
    }
}
void WebRtcCli::addIceCandidate(const QString &candidate, const QString &mid)
{
    if (!m_peerConnection)
        return;

    try
    {
        rtc::Candidate rtcCandidate(candidate.toStdString(), mid.toStdString());
        m_peerConnection->addRemoteCandidate(rtcCandidate);
        LOG_DEBUG("Added ICE candidate");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to add ICE candidate: {}", e.what());
    }
}
void WebRtcCli::startMediaCapture()
{
    if (!m_mediaCapture)
    {
        LOG_ERROR("Media capture not initialized");
        return;
    }

    try
    {
        LOG_INFO("Starting media capture with intelligent resolution selection");

        // 使用智能计算的编码分辨率
        m_mediaCapture->startCapture(m_encode_width, m_encode_height, m_fps);
        LOG_INFO("Media capture started with intelligent resolution: {}x{}, local screen: {}x{}",
                 m_encode_width, m_encode_height, m_screen_width, m_screen_height);
        // m_mediaCapture->startAudioCapture();
        LOG_INFO("Media capture started successfully");
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to start media capture: {}", e.what());
    }
}
void WebRtcCli::stopMediaCapture()
{
    if (!m_mediaCapture)
    {
        LOG_WARN("Media capture is null, cannot stop");
        return;
    }

    try
    {
        LOG_INFO("Stopping media capture");
        m_mediaCapture->stopCapture();
        m_mediaCapture->stopAudioCapture();
        LOG_INFO("Media capture stop requested successfully");
        m_destroying = true;

        emit destroyCli(); // 通知销毁客户端
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to stop media capture: {}", e.what());
    }
}
void WebRtcCli::onVideoFrameReady(const rtc::binary &frameData)
{
    if (!m_videoTrack || !m_connected)
        return;

    // 验证H264数据有效性
    if (frameData.empty())
    {
        LOG_WARN("Received empty video frame data");
        return;
    }

    // 检查是否为有效的H264数据（应该包含NAL单元起始码）
    bool hasValidStartCode = false;
    if (frameData.size() >= 4)
    {
        // 检查0x00000001起始码
        if (frameData[0] == std::byte(0x00) &&
            frameData[1] == std::byte(0x00) &&
            frameData[2] == std::byte(0x00) &&
            frameData[3] == std::byte(0x01))
        {
            hasValidStartCode = true;
        }
        // 检查0x000001起始码
        else if (frameData[0] == std::byte(0x00) &&
                 frameData[1] == std::byte(0x00) &&
                 frameData[2] == std::byte(0x01))
        {
            hasValidStartCode = true;
        }
    }

    if (!hasValidStartCode)
    {
        LOG_WARN("H264 frame data does not contain valid start code, size: {}", Convert::formatFileSize(frameData.size()));
        if (frameData.size() >= 8)
        {
            LOG_DEBUG("First 8 bytes: {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                      static_cast<uint8_t>(frameData[0]), static_cast<uint8_t>(frameData[1]),
                      static_cast<uint8_t>(frameData[2]), static_cast<uint8_t>(frameData[3]),
                      static_cast<uint8_t>(frameData[4]), static_cast<uint8_t>(frameData[5]),
                      static_cast<uint8_t>(frameData[6]), static_cast<uint8_t>(frameData[7]));
        }
        return;
    }

    try
    {
        // 发送视频帧 - 使用官方示例的方式
        if (m_videoTrack->isOpen())
        {
            // 计算时间戳（微秒）
            qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
            qint64 timestamp_us = (currentTime - m_baseTimestamp) * 1000; // 转为微秒

            // 使用chrono duration发送帧
            m_videoTrack->sendFrame(frameData, std::chrono::duration<double, std::micro>(timestamp_us));
            LOG_TRACE("Sent video frame: {}, timestamp: {} us", Convert::formatFileSize(frameData.size()), timestamp_us);
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send video frame: {}", e.what());
    }
}
void WebRtcCli::onAudioFrameReady(const rtc::binary &frameData)
{
    if (!m_audioTrack || !m_connected)
        return;

    try
    {
        // 计算时间戳
        qint64 currentTime = QDateTime::currentMSecsSinceEpoch();
        qint64 timestamp = currentTime - m_baseTimestamp;
        rtc::FrameInfo frameInfo(timestamp);
        // 发送音频帧
        if (m_audioTrack->isOpen())
        {
            m_audioTrack->sendFrame(frameData, frameInfo);
            // 记录日志
            LOG_TRACE("Sent audio frame: {}, timestamp: {}", Convert::formatFileSize(frameData.size()), timestamp);
        }
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send audio frame: {}", e.what());
    }
}

void WebRtcCli::sendFile(const QString &cliPath, const QString &ctlPath)
{
    QFileInfo info(cliPath);
    if (!info.exists())
    {
        LOG_ERROR("File or directory does not exist: {}", cliPath);
        sendFileErrorResponse(cliPath, "File or directory does not exist");
        return;
    }

    if (info.isFile())
    {
        // 发送单个文件
        sendSingleFile(cliPath, ctlPath);
    }
    else if (info.isDir())
    {
        // 发送文件夹中的所有文件
        sendDirectory(cliPath, ctlPath);
    }
    else
    {
        LOG_ERROR("Unknown file type: {}", cliPath);
        sendFileErrorResponse(cliPath, "Unknown file type");
    }
}

void WebRtcCli::sendSingleFile(const QString &cliPath, const QString &ctlPath)
{
    QFileInfo fileInfo(cliPath);
    if (!fileInfo.exists() || !fileInfo.isFile())
    {
        LOG_ERROR("File does not exist or is not a regular file: {}", cliPath);
        sendFileErrorResponse(cliPath, "File does not exist or is not a regular file");
        return;
    }

    QString absCtlPath = ctlPath;
    if (!absCtlPath.endsWith(fileInfo.fileName()))
    {
        absCtlPath = QDir::cleanPath(absCtlPath + "/" + fileInfo.fileName());
    }

    // 创建包含文件信息的头部
    QJsonObject header = JsonUtil::createObject()
                             .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                             .add(Constant::KEY_PATH_CLI, cliPath)
                             .add(Constant::KEY_PATH_CTL, absCtlPath)
                             .add(Constant::KEY_FILE_SIZE, static_cast<double>(fileInfo.size()))
                             .add("isDirectory", false)
                             .build();

    // 使用流式发送方法，避免将大文件加载到内存
    if (m_fileChannel && m_fileChannel->isOpen())
    {
        try
        {
            if (FilePacketUtil::sendFileStream(cliPath, header, m_fileChannel))
            {
                LOG_INFO("Sent file stream: {} -> {} ({})",
                         cliPath, absCtlPath, Convert::formatFileSize(fileInfo.size()));
            }
            else
            {
                LOG_ERROR("Failed to send file stream: {}", cliPath);
                sendFileErrorResponse(cliPath, "Failed to send file stream");
            }
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Exception during file stream send: {}", e.what());
            sendFileErrorResponse(cliPath, "Exception during file stream send");
        }
    }
    else
    {
        LOG_ERROR("File channel not available for sending file");
        sendFileErrorResponse(cliPath, "File channel not available");
    }
}

void WebRtcCli::sendDirectory(const QString &cliPath, const QString &ctlPath)
{
    QDir dir(cliPath);
    dir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    dir.setSorting(QDir::Name | QDir::DirsFirst); // 按名称排序，文件夹优先
    // 遍历条目并分类
    QFileInfoList list = dir.entryInfoList();

    // 首先发送目录开始标记
    QJsonObject dirStartHeader = JsonUtil::createObject()
                                     .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                                     .add(Constant::KEY_PATH_CLI, cliPath)
                                     .add(Constant::KEY_PATH_CTL, ctlPath)
                                     .add("isDirectory", true)
                                     .add("directoryStart", true)
                                     .build();

    sendFileTextChannelMessage(dirStartHeader);

    int fileCount = 0;
    for (const QFileInfo &fileInfo : list)
    {
        if (fileInfo.isFile())
        {
            QString relativePath = dir.relativeFilePath(fileInfo.absoluteFilePath());

            // 在远程路径中包含目录名
            QString fullRemotePath = QDir::cleanPath(ctlPath + "/" + relativePath);

            // 发送单个文件（使用原始文件名作为显示名称）
            sendSingleFile(fileInfo.absoluteFilePath(), fullRemotePath);
            fileCount++;
        }
    }

    // 发送目录结束标记
    QJsonObject dirEndHeader = JsonUtil::createObject()
                                   .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                                   .add(Constant::KEY_PATH_CLI, cliPath)
                                   .add(Constant::KEY_PATH_CTL, ctlPath)
                                   .add("isDirectory", true)
                                   .add("directoryEnd", true)
                                   .add("fileCount", fileCount)
                                   .build();

    sendFileTextChannelMessage(dirEndHeader);

    LOG_INFO("Sent directory: {} -> {} ({} files)", cliPath, ctlPath, fileCount);
}

void WebRtcCli::sendFileErrorResponse(const QString &filePath, const QString &error)
{
    QJsonObject errorMsg = JsonUtil::createObject()
                               .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                               .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                               .add(Constant::KEY_PATH, filePath)
                               .add("error", error)
                               .build();

    sendFileTextChannelMessage(errorMsg);
}

void WebRtcCli::sendUploadResponse(const QString &fileName, bool success, const QString &message)
{
    QJsonObject responseMsg = JsonUtil::createObject()
                                  .add(Constant::KEY_MSGTYPE, Constant::TYPE_UPLOAD_FILE_RES)
                                  .add(Constant::KEY_PATH_CLI, fileName)
                                  .add("status", success)
                                  .add("message", message)
                                  .build();

    sendFileTextChannelMessage(responseMsg);
}

void WebRtcCli::handleFileReceived(bool status, const QString &tempPath)
{
    LOG_INFO("Received complete file from FilePacketUtil, status: {}, tempPath: {}", status, tempPath);

    sendUploadResponse(tempPath, status, status ? "Upload successful" : "Upload failed");
}

void WebRtcCli::saveUploadedFile(const QString &filePath, const QByteArray &data)
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly))
    {
        LOG_ERROR("Failed to open file for writing: {}", filePath);

        // 发送错误响应
        QJsonObject errorMsg = JsonUtil::createObject()
                                   .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                   .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                                   .add(Constant::KEY_PATH, filePath)
                                   .add("error", "Failed to save file")
                                   .build();

        sendFileTextChannelMessage(errorMsg);
        return;
    }

    file.write(data);
    file.close();

    // 发送成功响应
    QJsonObject successMsg = JsonUtil::createObject()
                                 .add(Constant::KEY_ROLE, Constant::ROLE_CLI)
                                 .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_UPLOAD)
                                 .add(Constant::KEY_PATH, filePath)
                                 .add("success", true)
                                 .add("size", data.size())
                                 .build();

    sendFileTextChannelMessage(successMsg);
    LOG_INFO("Saved uploaded file: {} ({})", filePath, Convert::formatFileSize(data.size()));
}
void WebRtcCli::handleMouseEvent(const QJsonObject &object)
{
    int button = JsonUtil::getInt(object, Constant::KEY_BUTTON, -1);
    qreal x = JsonUtil::getDouble(object, Constant::KEY_X, -1);
    qreal y = JsonUtil::getDouble(object, Constant::KEY_Y, -1);
    int mouseData = JsonUtil::getInt(object, Constant::KEY_MOUSEDATA, -1);
    QString flags = JsonUtil::getString(object, Constant::KEY_DWFLAGS, "");
    if (x < 0 || y < 0)
    {
        LOG_ERROR("handleMouseEvent: Invalid mouse event data");
        return;
    }

    // 使用InputUtil处理鼠标事件
    InputUtil::execMouseEvent(button, x, y, mouseData, flags);
    LOG_DEBUG("Handled mouse event: {} at ({}, {})", flags, x, y);
}
void WebRtcCli::handleKeyboardEvent(const QJsonObject &object)
{
    int key = JsonUtil::getInt(object, Constant::KEY_KEY, -1);
    QString flags = JsonUtil::getString(object, Constant::KEY_DWFLAGS, "");

    if (key == -1 || flags.isEmpty())
    {
        LOG_ERROR("handleKeyboardEvent: Invalid keyboard event data");
        return;
    }

    // 使用InputUtil处理键盘事件
    InputUtil::execKeyboardEvent(key, flags);
    LOG_DEBUG("Handled keyboard event: {} {}", flags, key);
}
void WebRtcCli::sendFileChannelMessage(const QJsonObject &message)
{
    if (!m_connected || !m_fileChannel || !m_fileChannel->isOpen())
    {
        LOG_ERROR("File channel not available");
        return;
    }

    QString jsonStr = JsonUtil::toCompactString(message);
    std::string stdStr = jsonStr.toStdString();

    try
    {
        m_fileChannel->send(stdStr);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send file channel message: {}", e.what());
    }
}

void WebRtcCli::sendFileTextChannelMessage(const QJsonObject &message)
{
    if (!m_connected || !m_fileTextChannel || !m_fileTextChannel->isOpen())
    {
        LOG_ERROR("File text channel not available");
        return;
    }

    QString jsonStr = JsonUtil::toCompactString(message);
    std::string stdStr = jsonStr.toStdString();

    try
    {
        m_fileTextChannel->send(stdStr);
        LOG_DEBUG("Sent file text channel message: {}", jsonStr);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send file text channel message: {}", e.what());
    }
}

void WebRtcCli::sendInputChannelMessage(const QJsonObject &message)
{
    if (!m_inputChannel || !m_inputChannel->isOpen())
    {
        LOG_ERROR("Input channel not available");
        return;
    }

    QString jsonStr = JsonUtil::toCompactString(message);
    std::string stdStr = jsonStr.toStdString();

    try
    {
        m_inputChannel->send(stdStr);
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to send input channel message: {}", e.what());
    }
}

void WebRtcCli::calculateOptimalResolution(int controlMaxWidth, int controlMaxHeight)
{
    LOG_INFO("Calculating optimal encoding resolution - Control max display area: {}x{}, Local screen: {}x{}",
             controlMaxWidth, controlMaxHeight, m_screen_width, m_screen_height);

    // 如果控制端没有发送最大显示区域信息（-1），则使用被控端原始分辨率
    if (controlMaxWidth == -1 || controlMaxHeight == -1)
    {
        m_encode_width = m_screen_width;
        m_encode_height = m_screen_height;
        LOG_INFO("Using original local screen resolution: {}x{} (adaptive resolution disabled)",
                 m_encode_width, m_encode_height);
    }
    // 比较被控端实际分辨率和控制端最大显示区域，选择较小的
    else if (m_screen_width <= controlMaxWidth && m_screen_height <= controlMaxHeight)
    {
        // 被控端分辨率小于等于控制端显示区域，使用被控端实际分辨率
        m_encode_width = m_screen_width;
        m_encode_height = m_screen_height;
        LOG_INFO("Using local screen resolution: {}x{} (fits within control display area)",
                 m_encode_width, m_encode_height);
    }
    else
    {
        // 被控端分辨率大于控制端显示区域，需要按比例缩放保持宽高比
        double localAspectRatio = (double)m_screen_width / m_screen_height;
        double controlAspectRatio = (double)controlMaxWidth / controlMaxHeight;

        if (localAspectRatio > controlAspectRatio)
        {
            // 被控端更宽，以控制端宽度为准，按比例计算高度
            m_encode_width = controlMaxWidth;
            m_encode_height = (int)(controlMaxWidth / localAspectRatio);
        }
        else
        {
            // 被控端更高，以控制端高度为准，按比例计算宽度
            m_encode_height = controlMaxHeight;
            m_encode_width = (int)(controlMaxHeight * localAspectRatio);
        }

        LOG_INFO("Scaled to maintain aspect ratio: {}x{} (local aspect: {:.3f}, control aspect: {:.3f})",
                 m_encode_width, m_encode_height, localAspectRatio, controlAspectRatio);
    }

    // 确保编码分辨率按 16 对齐（MF/QSV/NVENC 等硬编更容易接受；同时仍满足 H264 偶数要求）
    // 向上取整到 16 的倍数： (x + 15) & ~15
    // 向下取整到 16 的倍数： x & ~15
    m_encode_width = m_encode_width & ~15;
    m_encode_height = m_encode_height & ~15;

    LOG_INFO("Final encoding resolution (16-aligned): {}x{}", m_encode_width, m_encode_height);
}
