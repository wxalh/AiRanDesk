#include "input_util.h"
#include <QRect>
#include <QScreen>
#include <QGuiApplication>

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
#include <windows.h>
#elif defined(Q_OS_LINUX)
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#undef KeyPress // 避免与Qt宏冲突
#undef KeyRelease
#undef None
#elif defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#include <CoreGraphics/CoreGraphics.h>
#endif

InputUtil::InputUtil(QObject *parent)
    : QObject{parent}
{
}

void InputUtil::execKeyboardEvent(int keyCode, const QString &dwFlags)
{

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(keyCode);
    input.ki.dwFlags = dwFlags == "down" ? 0 : KEYEVENTF_KEYUP;

    SendInput(1, &input, sizeof(INPUT));

#elif defined(Q_OS_LINUX)
    // Linux 实现 (X11)
    Display *display = XOpenDisplay(nullptr);
    if (display)
    {
        KeyCode code = XKeysymToKeycode(display, static_cast<KeySym>(keyCode));
        Bool isPress = (dwFlags == "down") ? True : False;
        XTestFakeKeyEvent(display, code, isPress, CurrentTime);
        XFlush(display);
        XCloseDisplay(display);
    }

#elif defined(Q_OS_MACOS)
    // macOS 实现 (CoreGraphics)
    CGEventRef event;
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStatePrivate);
    CGEventType type = (dwFlags == "down") ? kCGEventKeyDown : kCGEventKeyUp;

    event = CGEventCreateKeyboardEvent(source, static_cast<CGKeyCode>(keyCode), (type == kCGEventKeyDown));
    CGEventPost(kCGHIDEventTap, event);

    CFRelease(event);
    CFRelease(source);
#endif
}

void InputUtil::execMouseEvent(int button, qreal x_n, qreal y_n, int mouseData, const QString &dwFlags)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenRect = screen->geometry();
    // 统一坐标转换（考虑 macOS Retina 缩放）
    qreal scaleFactor = screen->devicePixelRatio();
    int x = static_cast<int>(x_n * screenRect.width() * scaleFactor);
    int y = static_cast<int>(y_n * screenRect.height() * scaleFactor);

#if defined(Q_OS_WIN64) || defined(Q_OS_WIN32)
    // Windows 实现
    SetCursorPos(x, y);
    if (dwFlags == "move")
    {
        return;
    }
    INPUT input = {0};
    input.type = INPUT_MOUSE;

    if (dwFlags == "doubleClick")
    {
        INPUT inputs[4] = {0};
        // 第一次点击（按下+释放）
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = button == Qt::LeftButton ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_RIGHTDOWN;
        inputs[1] = inputs[0];
        inputs[1].mi.dwFlags = button == Qt::LeftButton ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_RIGHTUP;

        // 第二次点击（需在系统双击时间间隔内）
        inputs[2] = inputs[0];
        inputs[3] = inputs[1];

        SendInput(4, inputs, sizeof(INPUT));
    }
    else if (dwFlags == "wheel")
    {
        input.mi.mouseData = mouseData;
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.dx = x * (65535 / GetSystemMetrics(SM_CXSCREEN)); // 坐标归一化
        input.mi.dy = y * (65535 / GetSystemMetrics(SM_CYSCREEN));
        SendInput(1, &input, sizeof(INPUT));
    }
    else
    {
        switch (button)
        {
        case Qt::LeftButton:
            input.mi.dwFlags = dwFlags == "down" ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
            break;
        case Qt::RightButton:
            input.mi.dwFlags = dwFlags == "down" ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
            break;
        case Qt::MiddleButton:
            input.mi.dwFlags = dwFlags == "down" ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
            break;
        }
        SendInput(1, &input, sizeof(INPUT));
    }

#elif defined(Q_OS_LINUX)
    // Linux 实现 (XTest)
    Display *display = XOpenDisplay(nullptr);
    if (display)
    {
        // 移动光标
        XTestFakeMotionEvent(display, -1, x, y, CurrentTime);

        if (dwFlags == "move")
        {
            return;
        }
        // 处理点击/滚轮
        if (dwFlags == "doubleClick")
        {
            int btn = (button == Qt::LeftButton) ? Button1 : Button3;
            XTestFakeButtonEvent(display, btn, True, CurrentTime);
            XTestFakeButtonEvent(display, btn, False, CurrentTime);
            XTestFakeButtonEvent(display, btn, True, CurrentTime);
            XTestFakeButtonEvent(display, btn, False, CurrentTime);
        }
        else if (dwFlags == "wheel")
        {
            int direction = (mouseData > 0) ? Button4 : Button5;
            XTestFakeButtonEvent(display, direction, True, CurrentTime);
            XTestFakeButtonEvent(display, direction, False, CurrentTime);
        }
        else
        {
            int btn;
            switch (button)
            {
            case Qt::LeftButton:
                btn = Button1;
                break;
            case Qt::RightButton:
                btn = Button3;
                break;
            case Qt::MiddleButton:
                btn = Button2;
                break;
            default:
                return;
            }
            XTestFakeButtonEvent(display, btn, (dwFlags == "down"), CurrentTime);
        }
        XFlush(display);
        XCloseDisplay(display);
    }

#elif defined(Q_OS_MACOS)
    // macOS 实现 (CoreGraphics)
    CGPoint pos = CGPointMake(x, y);
    CGEventType type;
    CGMouseButton btn = kCGMouseButtonLeft;

    // 确定按钮类型
    switch (button)
    {
    case Qt::LeftButton:
        btn = kCGMouseButtonLeft;
        break;
    case Qt::RightButton:
        btn = kCGMouseButtonRight;
        break;
    case Qt::MiddleButton:
        btn = kCGMouseButtonCenter;
        break;
    }

    // 处理事件类型
    if (dwFlags == "move")
    {
        CGEventRef event = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, pos, 0);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
    else if (dwFlags == "doubleClick")
    {
        CGEventRef event = CGEventCreateMouseEvent(
            nullptr, kCGEventLeftMouseDown, pos, kCGMouseButtonLeft);
        CGEventSetIntegerValueField(event, kCGMouseEventClickState, 2);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
    else if (dwFlags == "wheel")
    {
        CGEventRef event = CGEventCreateScrollWheelEvent(
            nullptr, kCGScrollEventUnitLine, 1, mouseData / 120);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
    else
    {
        type = (dwFlags == "down") ? ((btn == kCGMouseButtonLeft) ? kCGEventLeftMouseDown : kCGEventRightMouseDown) : ((btn == kCGMouseButtonLeft) ? kCGEventLeftMouseUp : kCGEventRightMouseUp);

        CGEventRef event = CGEventCreateMouseEvent(nullptr, type, pos, btn);
        CGEventPost(kCGHIDEventTap, event);
        CFRelease(event);
    }
#endif
}
