#ifndef CONVERT_H
#define CONVERT_H

#include <QObject>

class Convert
{
public:
    static QString formatFileSize(qint64 bytes);
};

#endif // CONVERT_H
