#include "convert.h"

QString Convert::formatFileSize(qint64 bytes)
{
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    int unitIndex = 0;
    double size = static_cast<double>(bytes);

    // 循环除以1024直到单位合适或到达最大单位
    while (size >= 1024 && unitIndex < units.size() - 1) {
        size /= 1024.0;
        unitIndex++;
    }
    return QString("%1 %2").arg(size, 0, 'f', 2).arg(units[unitIndex]);
}
