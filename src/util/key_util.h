#ifndef KEY_UTIL_H
#define KEY_UTIL_H
#include <QMap>
#include <QObject>

class KeyUtil: public QObject{
    Q_OBJECT
private:
    static void initMap(QMap<qint32,qint32>& m_KeyMap){
        m_KeyMap.insert(Qt::Key_Left,0x25);
        m_KeyMap.insert(Qt::Key_Up,0x26);
        m_KeyMap.insert(Qt::Key_Right,0x27);
        m_KeyMap.insert(Qt::Key_Down,0x28);
        m_KeyMap.insert(Qt::Key_Backspace,0x08);
        m_KeyMap.insert(Qt::Key_Tab,0x09);
        m_KeyMap.insert(Qt::Key_Clear,0x0C);
        m_KeyMap.insert(Qt::Key_Return,0x0D);
        m_KeyMap.insert(Qt::Key_Enter,0x0D);
        m_KeyMap.insert(Qt::Key_Shift,0x10);
        m_KeyMap.insert(Qt::Key_Control,0x11);
        m_KeyMap.insert(Qt::Key_Alt,0x12);
        m_KeyMap.insert(Qt::Key_Pause,0x13);
        m_KeyMap.insert(Qt::Key_CapsLock,0x14);
        m_KeyMap.insert(Qt::Key_Escape,0x1B);
        m_KeyMap.insert(Qt::Key_Space,0x20);
        m_KeyMap.insert(Qt::Key_PageUp,0x21);
        m_KeyMap.insert(Qt::Key_PageDown,0x22);
        m_KeyMap.insert(Qt::Key_End,0x23);
        m_KeyMap.insert(Qt::Key_Home,0x24);
        m_KeyMap.insert(Qt::Key_Select,0x29);
        m_KeyMap.insert(Qt::Key_Print,0x2A);
        m_KeyMap.insert(Qt::Key_Execute,0x2B);
        m_KeyMap.insert(Qt::Key_Printer,0x2C);
        m_KeyMap.insert(Qt::Key_Insert,0x2D);
        m_KeyMap.insert(Qt::Key_Delete,0x2E);
        m_KeyMap.insert(Qt::Key_Help,0x2F);
        m_KeyMap.insert(Qt::Key_0,0x30);
        m_KeyMap.insert(Qt::Key_ParenRight,0x30); // )
        m_KeyMap.insert(Qt::Key_1,0x31);
        m_KeyMap.insert(Qt::Key_Exclam,0x31); // !
        m_KeyMap.insert(Qt::Key_2,0x32);
        m_KeyMap.insert(Qt::Key_At,0x32); // @
        m_KeyMap.insert(Qt::Key_3,0x33);
        m_KeyMap.insert(Qt::Key_NumberSign,0x33); // #
        m_KeyMap.insert(Qt::Key_4,0x34);
        m_KeyMap.insert(Qt::Key_Dollar,0x34); // $
        m_KeyMap.insert(Qt::Key_5,0x35);
        m_KeyMap.insert(Qt::Key_Percent,0x35); // %
        m_KeyMap.insert(Qt::Key_6,0x36);
        m_KeyMap.insert(Qt::Key_AsciiCircum,0x36); // ^
        m_KeyMap.insert(Qt::Key_7,0x37);
        m_KeyMap.insert(Qt::Key_Ampersand,0x37); // &
        m_KeyMap.insert(Qt::Key_8,0x38);
        m_KeyMap.insert(Qt::Key_Asterisk,0x38); // *
        m_KeyMap.insert(Qt::Key_9,0x39);
        m_KeyMap.insert(Qt::Key_ParenLeft,0x39); // (
        m_KeyMap.insert(Qt::Key_A,0x41);
        m_KeyMap.insert(Qt::Key_B,0x42);
        m_KeyMap.insert(Qt::Key_C,0x43);
        m_KeyMap.insert(Qt::Key_D,0x44);
        m_KeyMap.insert(Qt::Key_E,0x45);
        m_KeyMap.insert(Qt::Key_F,0x46);
        m_KeyMap.insert(Qt::Key_G,0x47);
        m_KeyMap.insert(Qt::Key_H,0x48);
        m_KeyMap.insert(Qt::Key_I,0x49);
        m_KeyMap.insert(Qt::Key_J,0x4A);
        m_KeyMap.insert(Qt::Key_K,0x4B);
        m_KeyMap.insert(Qt::Key_L,0x4C);
        m_KeyMap.insert(Qt::Key_M,0x4D);
        m_KeyMap.insert(Qt::Key_N,0x4E);
        m_KeyMap.insert(Qt::Key_O,0x4F);
        m_KeyMap.insert(Qt::Key_P,0x50);
        m_KeyMap.insert(Qt::Key_Q,0x51);
        m_KeyMap.insert(Qt::Key_R,0x52);
        m_KeyMap.insert(Qt::Key_S,0x53);
        m_KeyMap.insert(Qt::Key_T,0x54);
        m_KeyMap.insert(Qt::Key_U,0x55);
        m_KeyMap.insert(Qt::Key_V,0x56);
        m_KeyMap.insert(Qt::Key_W,0x57);
        m_KeyMap.insert(Qt::Key_X,0x58);
        m_KeyMap.insert(Qt::Key_Y,0x59);
        m_KeyMap.insert(Qt::Key_Z,0x5A);
        m_KeyMap.insert(Qt::Key_multiply,0x6A);
        m_KeyMap.insert(Qt::Key_F1,0x70);
        m_KeyMap.insert(Qt::Key_F2,0x71);
        m_KeyMap.insert(Qt::Key_F3,0x72);
        m_KeyMap.insert(Qt::Key_F4,0x73);
        m_KeyMap.insert(Qt::Key_F5,0x74);
        m_KeyMap.insert(Qt::Key_F6,0x75);
        m_KeyMap.insert(Qt::Key_F7,0x76);
        m_KeyMap.insert(Qt::Key_F8,0x77);
        m_KeyMap.insert(Qt::Key_F9,0x78);
        m_KeyMap.insert(Qt::Key_F10,0x79);
        m_KeyMap.insert(Qt::Key_F11,0x7A);
        m_KeyMap.insert(Qt::Key_F12,0x7B);
        m_KeyMap.insert(Qt::Key_F13,0x7C);
        m_KeyMap.insert(Qt::Key_F14,0x7D);
        m_KeyMap.insert(Qt::Key_F15,0x7E);
        m_KeyMap.insert(Qt::Key_F16,0x7F);
        m_KeyMap.insert(Qt::Key_F17,0x80);
        m_KeyMap.insert(Qt::Key_F18,0x81);
        m_KeyMap.insert(Qt::Key_F19,0x82);
        m_KeyMap.insert(Qt::Key_F20,0x83);
        m_KeyMap.insert(Qt::Key_F21,0x84);
        m_KeyMap.insert(Qt::Key_F22,0x85);
        m_KeyMap.insert(Qt::Key_F23,0x86);
        m_KeyMap.insert(Qt::Key_F24,0x87);
        m_KeyMap.insert(Qt::Key_NumLock,0x90);
        m_KeyMap.insert(Qt::Key_ScrollLock,0x91);
        m_KeyMap.insert(Qt::Key_VolumeDown,0xAE);
        m_KeyMap.insert(Qt::Key_VolumeUp,0xAF);
        m_KeyMap.insert(Qt::Key_VolumeMute,0xAD);
        m_KeyMap.insert(Qt::Key_MediaStop,0xB2);
        m_KeyMap.insert(Qt::Key_MediaPlay,0xB3);
        m_KeyMap.insert(Qt::Key_Plus,0xBB); // +
        m_KeyMap.insert(Qt::Key_Minus,0xBD); // -
        m_KeyMap.insert(Qt::Key_Underscore,0xBD); // _
        m_KeyMap.insert(Qt::Key_Equal,0xBB); // =
        m_KeyMap.insert(Qt::Key_Semicolon,0xBA); // ;
        m_KeyMap.insert(Qt::Key_Colon,0xBA); // :
        m_KeyMap.insert(Qt::Key_Comma,0xBC); // ,
        m_KeyMap.insert(Qt::Key_Less,0xBC); // <
        m_KeyMap.insert(Qt::Key_Period,0xBE); // .
        m_KeyMap.insert(Qt::Key_Greater,0xBE); // >
        m_KeyMap.insert(Qt::Key_Slash,0xBF);  // /
        m_KeyMap.insert(Qt::Key_Question,0xBF); // ?
        m_KeyMap.insert(Qt::Key_BracketLeft,0xDB); // [
        m_KeyMap.insert(Qt::Key_BraceLeft,0xDB); // {
        m_KeyMap.insert(Qt::Key_BracketRight,0xDD); // ]
        m_KeyMap.insert(Qt::Key_BraceRight,0xDD); // }
        m_KeyMap.insert(Qt::Key_Bar,0xDC); // |
        m_KeyMap.insert(Qt::Key_Backslash,0xDC); /* \\ */
        m_KeyMap.insert(Qt::Key_Apostrophe,0xDE); /* ' */
        m_KeyMap.insert(Qt::Key_QuoteDbl,0xDE); // "
        m_KeyMap.insert(Qt::Key_QuoteLeft,0xC0); // `
        m_KeyMap.insert(Qt::Key_AsciiTilde,0xC0); // ~
        m_KeyMap.insert(Qt::Key_Menu,0x5D); // ~
    }
public:
    static qint32 qtKeyToWinKey(qint32 qtKey){
        static QMap<qint32,qint32> m_KeyMap;
        if(m_KeyMap.empty()){
            initMap(m_KeyMap);
        }
        return m_KeyMap.value(qtKey);
    }
};

#endif // KEY_UTIL_H
