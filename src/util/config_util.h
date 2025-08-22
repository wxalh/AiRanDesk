#ifndef CONFIG_UTIL_H
#define CONFIG_UTIL_H

#include <QObject>
#include <QSettings>
#include <QUuid>
/* spdlog头文件引用 */
#include "logger_manager.h"

#define ConfigUtil ConfigUtilData::getInstance()

class ConfigUtilData : public QObject
{
    Q_OBJECT
public:
    explicit ConfigUtilData(QObject *parent = nullptr);
    ~ConfigUtilData();
    static ConfigUtilData* getInstance();
    QString getOrCreateUuid();
    void saveIni();
    void setLocalPwd(const QString &pwd);
    QString getLocalPwd();
public:
    QString filePath;
    QSettings *m_configIni;
    //帧率
    int fps;
    //是否显示UI
    bool showUI;
    //本机sn码
    QString local_id;
    QString local_pwd_md5;
    //websocket服务器
    QString wsUrl;

    QString ice_host;
    uint16_t ice_port;
    QString ice_username;
    QString ice_password;
    spdlog::level::level_enum logLevel;
    QString logLevelStr;
private:
    //本机访问密码
    QString local_pwd;
signals:
};

#endif // CONFIG_UTIL_H
