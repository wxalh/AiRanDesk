#ifndef INPUT_UTIL_H
#define INPUT_UTIL_H

#include <QObject>

class InputUtil : public QObject
{
    Q_OBJECT
public:
    explicit InputUtil(QObject *parent = nullptr);
    static void execMouseEvent(int button, qreal x_n, qreal y_n,int mouseData,const QString& dwFlags);
    static void execKeyboardEvent(int keyCode,const QString& dwFlags);
signals:
};

#endif // INPUT_UTIL_H
