#ifndef CONSTANT_H
#define CONSTANT_H

#include <QString>
#include "convert.h"
#include <QThread>

#define DELETE_PTR_FUNC(obj_ptr) \
    if (obj_ptr)                 \
    {                            \
        obj_ptr->disconnect();   \
        delete obj_ptr;          \
        obj_ptr = nullptr;       \
    }

#define DELETELATER_PTR_FUNC(obj_ptr) \
    if (obj_ptr)                      \
    {                                 \
        obj_ptr->disconnect();        \
        obj_ptr->deleteLater();       \
        obj_ptr = nullptr;            \
    }

#define STOP_OBJ_THREAD(thread)                                                               \
    if (thread.isRunning())                                                                   \
    {                                                                                         \
        LOG_ERROR("{} thread stopping", thread.objectName());                                 \
        thread.quit();                                                                        \
        if (!thread.wait(200))                                                                \
        {                                                                                     \
            LOG_ERROR("{} thread did not quit gracefully, terminating", thread.objectName()); \
            thread.terminate();                                                               \
            thread.wait();                                                                    \
        }                                                                                     \
        LOG_INFO("{} thread stopped", thread.objectName());                                   \
    }

#define STOP_PTR_THREAD(thread_ptr)                                                                \
    if (thread_ptr && thread_ptr->isRunning())                                                     \
    {                                                                                              \
        LOG_ERROR("{} thread stopping", thread_ptr->objectName());                                 \
        thread_ptr->quit();                                                                        \
        if (!thread_ptr->wait(200))                                                                \
        {                                                                                          \
            LOG_ERROR("{} thread did not quit gracefully, terminating", thread_ptr->objectName()); \
            thread_ptr->terminate();                                                               \
            thread_ptr->wait();                                                                    \
        }                                                                                          \
        LOG_INFO("{} thread stopped", thread_ptr->objectName());                                   \
    }

namespace Constant
{
    static const QString MSG_TYPE_BINARY = "binary";
    static const QString MSG_TYPE_TEXT = "text";
    static const QString MSG_TYPE_OTHER = "other";

    static const QString KEY_NAME = "name";
    static const QString KEY_STATUS = "status";
    static const QString KEY_SN = "sn";
    static const QString KEY_ROLE = "role";
    static const QString KEY_SENDER = "sender";
    static const QString KEY_RECEIVER = "receiver";
    static const QString KEY_TYPE = "type"; // websocket发送消息的type
    static const QString KEY_DATA = "data";
    static const QString KEY_MID = "mid";
    static const QString KEY_HEIGHT = "height";
    static const QString KEY_WIDTH = "width";
    static const QString KEY_FPS = "fps";
    static const QString KEY_LABEL_NAME = "label_name";
    static const QString KEY_IS_ONLY_FILE = "is_only_file";
    static const QString KEY_ONLY_RELAY = "only_relay";

    static const QString KEY_FILE_PATH = "file_path";
    static const QString KEY_FILE_DATA = "file_data";
    static const QString KEY_FILE_NUM = "file_num";
    static const QString KEY_FILE_NOW_NUM = "file_now_num";
    static const QString KEY_FILE_SIZE = "file_size";
    static const QString KEY_FILE_SUFFIX = "file_suffix";
    static const QString KEY_IS_DIR = "is_dir";
    static const QString KEY_FILE_LAST_MOD_TIME = "file_last_mod_time";
    static const QString KEY_RECEIVER_PWD = "receiver_pwd";
    static const QString KEY_MSGTYPE = "msgType"; // datachannel发送消息的type
    static const QString KEY_PATH = "path";
    static const QString KEY_PATH_CLI = "path_cli";
    static const QString KEY_PATH_CTL = "path_ctl";

    static const QString ROLE_CLI = "cli";
    static const QString ROLE_CTL = "ctl";
    static const QString ROLE_SERVER = "server";

    static const QString TYPE_LIST_FOLDER_FILES = "listFolderFiles";
    static const QString TYPE_UPLOAD_FILE = "upload_file";
    static const QString TYPE_UPLOAD_FILE_RES = "upload_file_res";
    static const QString TYPE_DOWNLOAD_FILE = "download_file";
    static const QString TYPE_DOWNLOAD_FILE_RES = "download_file_res";
    static const QString TYPE_FILE_LIST = "file_list";
    static const QString TYPE_FILE_DOWNLOAD = "file_download";
    static const QString TYPE_FILE_UPLOAD = "file_upload";

    static const QString TYPE_OFFER = "offer";
    static const QString TYPE_ANSWER = "answer";
    static const QString TYPE_CANDIDATE = "candidate";
    static const QString TYPE_FILE = "file_airan";
    static const QString TYPE_FILE_TEXT = "file_text_airan";
    static const QString TYPE_VIDEO = "video_airan";
    static const QString TYPE_VIDEO_MSID = "video_stream1_airan";
    static const QString TYPE_AUDIO = "audio_airan";
    static const QString TYPE_INPUT = "input_airan"; // 键盘鼠标输入通道
    static const QString TYPE_DIR = "dir";
    static const QString TYPE_CONNECT = "connect";
    static const QString TYPE_CONNECTED = "connected";
    static const QString TYPE_ONLINE_ONE = "onlineOne";
    static const QString TYPE_ONLINE_LIST = "onlineList";
    static const QString TYPE_OFFLINE_ONE = "offlineOne";
    static const QString TYPE_KEYFRAME_REQUEST = "keyframe_request";
    static const QString TYPE_KEYFRAME_RESPONSE = "keyframe_response";
    static const QString TYPE_ERROR = "error";
    // 控制消息类型和附加字段
    static const QString TYPE_KEYBOARD = "keyboard";
    static const QString TYPE_MOUSE = "mouse";

    static const QString KEY_KEY = "key";
    static const QString KEY_DWFLAGS = "dwFlags";
    static const QString KEY_X = "x";
    static const QString KEY_Y = "y";
    static const QString KEY_DOWN = "down";
    static const QString KEY_UP = "up";
    static const QString KEY_MOVE = "move";
    static const QString KEY_WHEEL = "wheel";
    static const QString KEY_DOUBLECLICK = "doubleClick";
    static const QString KEY_BUTTON = "button";
    static const QString KEY_MOUSEDATA = "mouseData";
    static const QString KEY_FOLDER_MOUNTED = "mounted";   // 这个常量用于表示已挂载的驱动器
    static const QString KEY_FOLDER_FILES = "folderFiles"; // 这个常量用于表示文件夹中的文件

    static const QString START_CAPTURE = "startCapture";
    static const QString STOP_CAPTURE = "stopCapture";
    static const QString START_MEDIA_STREAM = "startMediaStream";
    static const QString STOP_MEDIA_STREAM = "stopMediaStream";

    static const QString FOLDER_HOME = "home"; // 这个常量用于表示用户的主目录或应用程序的根目录
}

#endif // CONSTANT_H
// End of MSG src/constant.h
