#include "config_util.h"
#include <QCoreApplication>
#include <QCryptographicHash>

ConfigUtilData::ConfigUtilData(QObject *parent)
    : QObject{parent}
{
    filePath = QCoreApplication::applicationDirPath() + "/config.ini";
    m_configIni = new QSettings(filePath, QSettings::IniFormat);
    m_configIni->setIniCodec("UTF-8");

    local_id = getOrCreateUuid();

    m_configIni->beginGroup("local");
    local_pwd = m_configIni->value("local_pwd", "").toString();
    showUI = m_configIni->value("showUI", true).toBool();
    logLevelStr = m_configIni->value("logLevel", "info").toString();
    m_configIni->endGroup();

    m_configIni->beginGroup("remote");
    fps = m_configIni->value("fps", 15).toInt(); // 降低默认帧率从25到15
    m_configIni->endGroup();

    if (fps < 1 || fps > 60)
    {
        fps = 15;
    }
    m_configIni->beginGroup("signal_server");
    wsUrl = m_configIni->value("wsUrl", "").toString();
    m_configIni->endGroup();

    m_configIni->beginGroup("ice_server");
    ice_host = m_configIni->value("host", "").toString();
    ice_port = (uint16_t)(m_configIni->value("port", 3478).toUInt());
    ice_username = m_configIni->value("username", "").toString();
    ice_password = m_configIni->value("password", "").toString();
    m_configIni->endGroup();

    if (local_pwd.isEmpty() || QUuid(local_pwd).isNull())
    {
        local_pwd = QUuid::createUuid().toString(QUuid::WithoutBraces).toUpper();
    }

    if (logLevelStr == "trace")
    {
        logLevel = spdlog::level::trace;
    }
    else if (logLevelStr == "debug")
    {
        logLevel = spdlog::level::debug;
    }
    else if (logLevelStr == "info")
    {
        logLevel = spdlog::level::info;
    }
    else if (logLevelStr == "warn")
    {
        logLevel = spdlog::level::warn;
    }
    else if (logLevelStr == "error")
    {
        logLevel = spdlog::level::err;
    }
    else if (logLevelStr == "critical")
    {
        logLevel = spdlog::level::critical;
    }
    else
    {
        logLevel = spdlog::level::info; // 默认级别
    }

    setLocalPwd(local_pwd);

    SPDLOG_INFO("local control code: {} pwd: {}", local_id.toStdString(), local_pwd.toStdString());
}

ConfigUtilData::~ConfigUtilData()
{
}

ConfigUtilData *ConfigUtilData::getInstance()
{
    static ConfigUtilData configUtil;
    return &configUtil;
}

QString ConfigUtilData::getOrCreateUuid()
{
    // 设置组织名和应用名（确定存储路径）
    QCoreApplication::setOrganizationName("wxalh.com");
    QCoreApplication::setApplicationName("airan");
    QSettings settings; // 自动选择系统默认位置

    // 尝试读取存储的UUID
    QString uuidKey = "Global/Uuid";
    QString storedUuid = settings.value(uuidKey).toString().toUpper();

    // 检查UUID是否有效（非空且符合格式）
    QUuid uuid(storedUuid);
    if (!storedUuid.isEmpty() && !uuid.isNull())
    {
        return storedUuid;
    }

    // 生成新的UUID并存储
    QUuid newUuid = QUuid::createUuid();
    QString newUuidStr = newUuid.toString(QUuid::WithoutBraces).toUpper(); // 移除花括号
    settings.setValue(uuidKey, newUuidStr);
    settings.sync(); // 强制写入磁盘
    return newUuidStr;
}

void ConfigUtilData::saveIni()
{
    m_configIni->beginGroup("local");
    m_configIni->setValue("showUI", showUI);
    m_configIni->setValue("logLevel", logLevelStr);
    m_configIni->setValue("local_id", local_id);
    m_configIni->setValue("local_pwd", local_pwd);
    m_configIni->endGroup();

    m_configIni->beginGroup("remote");
    m_configIni->setValue("fps", fps);
    m_configIni->endGroup();

    m_configIni->beginGroup("signal_server");
    m_configIni->setValue("wsUrl", wsUrl);
    m_configIni->endGroup();

    // m_configIni->beginGroup("ice_server");
    // m_configIni->setValue("host", ice_host);
    // m_configIni->setValue("port", ice_port);
    // m_configIni->setValue("username", ice_username);
    // m_configIni->setValue("password", ice_password);
    // m_configIni->endGroup();

    m_configIni->sync();
}

void ConfigUtilData::setLocalPwd(const QString &pwd)
{
    this->local_pwd = pwd;
    QByteArray hashResult = QCryptographicHash::hash(local_pwd.toUtf8(), QCryptographicHash::Md5);
    this->local_pwd_md5 = hashResult.toHex().toUpper();
    saveIni();
}

QString ConfigUtilData::getLocalPwd()
{
    return local_pwd;
}
