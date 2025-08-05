#include "json_util.h"
#include "logger_manager.h"

// === 解析函数实现 ===

QJsonObject JsonUtil::safeParseObject(const QByteArray &data)
{
    if (data.isEmpty())
        return QJsonObject();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError)
    {
        LOG_ERROR("JsonUtil::safeParseObject: Parse error: {}", error.errorString());
        return QJsonObject();
    }
    
    if (!doc.isObject())
    {
        LOG_ERROR("JsonUtil::safeParseObject: Document is not an object");
        return QJsonObject();
    }
    
    return doc.object();
}

QJsonObject JsonUtil::safeParseObject(const QString &str)
{
    return safeParseObject(str.toUtf8());
}

QJsonArray JsonUtil::safeParseArray(const QByteArray &data)
{
    if (data.isEmpty())
        return QJsonArray();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    
    if (error.error != QJsonParseError::NoError)
    {
        LOG_ERROR("JsonUtil::safeParseArray: Parse error: {}", error.errorString());
        return QJsonArray();
    }
    
    if (!doc.isArray())
    {
        LOG_ERROR("JsonUtil::safeParseArray: Document is not an array");
        return QJsonArray();
    }
    
    return doc.array();
}

// === 值获取函数实现 ===

QString JsonUtil::getString(const QJsonObject &object, const QString &key, const QString &defaultValue)
{
    if (!object.contains(key))
        return defaultValue;
    
    QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : defaultValue;
}

int JsonUtil::getInt(const QJsonObject &object, const QString &key, int defaultValue)
{
    if (!object.contains(key))
        return defaultValue;
    
    QJsonValue value = object.value(key);
    return value.isDouble() ? static_cast<int>(value.toDouble()) : defaultValue;
}

qint64 JsonUtil::getInt64(const QJsonObject &object, const QString &key, qint64 defaultValue)
{
    if (!object.contains(key))
        return defaultValue;
    
    QJsonValue value = object.value(key);
    return value.isDouble() ? static_cast<qint64>(value.toDouble()) : defaultValue;
}

bool JsonUtil::getBool(const QJsonObject &object, const QString &key, bool defaultValue)
{
    if (!object.contains(key))
        return defaultValue;
    
    QJsonValue value = object.value(key);
    return value.isBool() ? value.toBool() : defaultValue;
}

double JsonUtil::getDouble(const QJsonObject &object, const QString &key, double defaultValue)
{
    if (!object.contains(key))
        return defaultValue;
    
    QJsonValue value = object.value(key);
    return value.isDouble() ? value.toDouble() : defaultValue;
}

QJsonObject JsonUtil::getObject(const QJsonObject &object, const QString &key)
{
    if (!object.contains(key))
        return QJsonObject();
    
    QJsonValue value = object.value(key);
    return value.isObject() ? value.toObject() : QJsonObject();
}

QJsonArray JsonUtil::getArray(const QJsonObject &object, const QString &key)
{
    if (!object.contains(key))
        return QJsonArray();
    
    QJsonValue value = object.value(key);
    return value.isArray() ? value.toArray() : QJsonArray();
}

// === 验证函数实现 ===

bool JsonUtil::hasRequiredKeys(const QJsonObject &object, const QStringList &requiredKeys)
{
    for (const QString &key : requiredKeys)
    {
        if (!object.contains(key) || object.value(key).isNull())
        {
            LOG_ERROR("JsonUtil::hasRequiredKeys: Missing required key: {}", key);
            return false;
        }
    }
    return true;
}

bool JsonUtil::isValidObject(const QJsonObject &object)
{
    return !object.isEmpty();
}

// === 构建函数实现 ===

JsonObjectBuilder JsonUtil::createObject()
{
    return JsonObjectBuilder();
}

QByteArray JsonUtil::toCompactBytes(const QJsonObject &object)
{
    QJsonDocument doc(object);
    return doc.toJson(QJsonDocument::Compact);
}

QString JsonUtil::toCompactString(const QJsonObject &object)
{
    return QString::fromUtf8(toCompactBytes(object));
}

// === 兼容性函数实现 ===

QMap<QString, QVariant> JsonUtil::json2Map(const QJsonObject &json_obj)
{
    QMap<QString, QVariant> maps;
    if (!json_obj.isEmpty())
    {
        for (auto it = json_obj.begin(); it != json_obj.end(); ++it)
        {
            maps.insert(it.key(), it.value().toVariant());
        }
    }
    return maps;
}

QMap<QString, QVariant> JsonUtil::json2Map(const QString &str)
{
    QJsonObject json_obj = safeParseObject(str);
    return json2Map(json_obj);
}

// === JsonObjectBuilder实现 ===

JsonObjectBuilder &JsonObjectBuilder::add(const QString &key, const char *value)
{
    m_object.insert(key, QString::fromUtf8(value));
    return *this;
}

JsonObjectBuilder &JsonObjectBuilder::add(const QString &key, const QString &value)
{
    m_object.insert(key, value);
    return *this;
}
JsonObjectBuilder& JsonObjectBuilder::add(const QString &key, const std::string &value)
{
    m_object.insert(key, QString::fromStdString(value));
    return *this;
}
JsonObjectBuilder& JsonObjectBuilder::add(const std::string &key, const std::string &value)
{
    m_object.insert(QString::fromStdString(key), QString::fromStdString(value));
    return *this;
}

JsonObjectBuilder& JsonObjectBuilder::add(const QString &key, int value)
{
    m_object.insert(key, value);
    return *this;
}

JsonObjectBuilder& JsonObjectBuilder::add(const QString &key, qint64 value)
{
    m_object.insert(key, static_cast<double>(value));
    return *this;
}

JsonObjectBuilder& JsonObjectBuilder::add(const QString &key, bool value)
{
    m_object.insert(key, value);
    return *this;
}

JsonObjectBuilder& JsonObjectBuilder::add(const QString &key, double value)
{
    m_object.insert(key, value);
    return *this;
}

JsonObjectBuilder& JsonObjectBuilder::add(const QString &key, const QJsonObject &value)
{
    m_object.insert(key, value);
    return *this;
}

JsonObjectBuilder& JsonObjectBuilder::add(const QString &key, const QJsonArray &value)
{
    m_object.insert(key, value);
    return *this;
}

QByteArray JsonObjectBuilder::toBytes() const
{
    return JsonUtil::toCompactBytes(m_object);
}

QString JsonObjectBuilder::toString() const
{
    return JsonUtil::toCompactString(m_object);
}
