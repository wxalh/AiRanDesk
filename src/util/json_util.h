#ifndef JSONUTIL_H
#define JSONUTIL_H

#include <QObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonParseError>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QVariant>
#include <QMap>


// JSON对象构建器类，用于链式构建JSON对象
class JsonObjectBuilder
{
public:
    JsonObjectBuilder() = default;
    
    JsonObjectBuilder& add(const QString &key, const char* value);
    JsonObjectBuilder& add(const QString &key, const QString &value);
    JsonObjectBuilder& add(const QString &key, const std::string &value);
    JsonObjectBuilder& add(const std::string &key, const std::string &value);
    JsonObjectBuilder& add(const QString &key, int value);
    JsonObjectBuilder& add(const QString &key, qint64 value);
    JsonObjectBuilder& add(const QString &key, bool value);
    JsonObjectBuilder& add(const QString &key, double value);
    JsonObjectBuilder& add(const QString &key, const QJsonObject &value);
    JsonObjectBuilder& add(const QString &key, const QJsonArray &value);
    
    QJsonObject build() const { return m_object; }
    QByteArray toBytes() const;
    QString toString() const;
    
private:
    QJsonObject m_object;
};

class JsonUtil
{
public:
    // === 安全的JSON解析函数 ===
    
    /**
     * @brief 安全地从字节数组解析JSON对象
     * @param data 输入的字节数组
     * @return 解析成功返回JSON对象，失败返回空对象
     */
    static QJsonObject safeParseObject(const QByteArray &data);
    
    /**
     * @brief 安全地从字符串解析JSON对象
     * @param str 输入的字符串
     * @return 解析成功返回JSON对象，失败返回空对象
     */
    static QJsonObject safeParseObject(const QString &str);
    
    /**
     * @brief 安全地从字节数组解析JSON数组
     * @param data 输入的字节数组
     * @return 解析成功返回JSON数组，失败返回空数组
     */
    static QJsonArray safeParseArray(const QByteArray &data);
    
    // === 安全的值获取函数 ===
    
    /**
     * @brief 安全地获取字符串值
     * @param object JSON对象
     * @param key 键名
     * @param defaultValue 默认值
     * @return 获取到的字符串值，失败返回默认值
     */
    static QString getString(const QJsonObject &object, const QString &key, const QString &defaultValue = QString());
    
    /**
     * @brief 安全地获取整数值
     * @param object JSON对象
     * @param key 键名
     * @param defaultValue 默认值
     * @return 获取到的整数值，失败返回默认值
     */
    static int getInt(const QJsonObject &object, const QString &key, int defaultValue = 0);
    
    /**
     * @brief 安全地获取64位整数值
     * @param object JSON对象
     * @param key 键名
     * @param defaultValue 默认值
     * @return 获取到的64位整数值，失败返回默认值
     */
    static qint64 getInt64(const QJsonObject &object, const QString &key, qint64 defaultValue = 0);
    
    /**
     * @brief 安全地获取布尔值
     * @param object JSON对象
     * @param key 键名
     * @param defaultValue 默认值
     * @return 获取到的布尔值，失败返回默认值
     */
    static bool getBool(const QJsonObject &object, const QString &key, bool defaultValue = false);
    
    /**
     * @brief 安全地获取浮点数值
     * @param object JSON对象
     * @param key 键名
     * @param defaultValue 默认值
     * @return 获取到的浮点数值，失败返回默认值
     */
    static double getDouble(const QJsonObject &object, const QString &key, double defaultValue = 0.0);
    
    /**
     * @brief 安全地获取JSON对象
     * @param object JSON对象
     * @param key 键名
     * @return 获取到的JSON对象，失败返回空对象
     */
    static QJsonObject getObject(const QJsonObject &object, const QString &key);
    
    /**
     * @brief 安全地获取JSON数组
     * @param object JSON对象
     * @param key 键名
     * @return 获取到的JSON数组，失败返回空数组
     */
    static QJsonArray getArray(const QJsonObject &object, const QString &key);
    
    // === 验证函数 ===
    
    /**
     * @brief 检查JSON对象是否包含所有必需的键
     * @param object JSON对象
     * @param requiredKeys 必需的键列表
     * @return 如果包含所有必需键返回true，否则返回false
     */
    static bool hasRequiredKeys(const QJsonObject &object, const QStringList &requiredKeys);
    
    /**
     * @brief 检查JSON对象是否为空或无效
     * @param object JSON对象
     * @return 如果对象有效且非空返回true，否则返回false
     */
    static bool isValidObject(const QJsonObject &object);
    
    // === 构建函数 ===
    
    /**
     * @brief 创建JSON对象的构建器
     * @return JsonObjectBuilder实例
     */
    static JsonObjectBuilder createObject();
    
    /**
     * @brief 安全地将JSON对象转换为紧凑的字节数组
     * @param object JSON对象
     * @return 字节数组
     */
    static QByteArray toCompactBytes(const QJsonObject &object);
    
    /**
     * @brief 安全地将JSON对象转换为紧凑的字符串
     * @param object JSON对象
     * @return 字符串
     */
    static QString toCompactString(const QJsonObject &object);
    
    // === 兼容性函数（保持向后兼容）===
    
    /**
     * @brief 从字符串解析JSON对象（兼容原有代码）
     * @param str 输入字符串
     * @return JSON对象
     */
    static QJsonObject str2Json(const QString &str) { return safeParseObject(str); }
    
    /**
     * @brief 将JSON对象转换为QMap（兼容原有代码）
     * @param json_obj JSON对象
     * @return QMap
     */
    static QMap<QString, QVariant> json2Map(const QJsonObject &json_obj);
    
    /**
     * @brief 从字符串解析并转换为QMap（兼容原有代码）
     * @param str 输入字符串
     * @return QMap
     */
    static QMap<QString, QVariant> json2Map(const QString &str);
};


#endif // JSONUTIL_H
