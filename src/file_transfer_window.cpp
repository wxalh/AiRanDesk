#include "file_transfer_window.h"
#include "ui_file_transfer_window.h"
#include "constant.h"
#include "util/json_util.h"
#include <QDir>
#include <QComboBox>
#include <QPushButton>
#include <QHeaderView>
#include <QStorageInfo>

FileTransferWindow::FileTransferWindow(QString remoteId, QString remotePwdMd5, WsCli *_ws_cli, QWidget *parent) : QWidget(parent), ui(new Ui::FileTransferWindow), connected(false), remote_id(remoteId), remote_pwd_md5(remotePwdMd5), m_rtc_ctl(remoteId, remotePwdMd5, true), m_ws(_ws_cli), currentLocalDir(QDir::home())
{
    initUI();
    initCLI();
    // 初始化WebRtcCtl
    emit initRtcCtl();
}

FileTransferWindow::~FileTransferWindow()
{
    disconnect();
    STOP_OBJ_THREAD(m_rtc_ctl_thread);
    delete ui;
}

void FileTransferWindow::initUI()
{
    ui->setupUi(this);
    setAttribute(Qt::WA_DeleteOnClose); // 关闭时自动触发deleteLater()
    // this->setWindowFlags(windowFlags()&~Qt::WindowMaximizeButtonHint);
    setWindowTitle("文件传输：" + remote_id);

    // 获取文件夹图标
    dirIcon = QApplication::style()->standardIcon(QStyle::SP_DirIcon);
    // 获取文件图标
    fileIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);

    // 设置表格样式
    setupFileTables();
    setupLogTable();

    // 填充初始文件数据
    populateLocalFiles();
    populateRemoteFiles();

    // 连接信号槽
    connect(ui->uploadButton, &QPushButton::clicked, this, &FileTransferWindow::onUploadButtonClicked);
    connect(ui->downloadButton, &QPushButton::clicked, this, &FileTransferWindow::onDownloadButtonClicked);

    // 设置传输记录窗口高度为一半
    ui->verticalLayout->setStretch(0, 2); // 上方文件区域占2份
    ui->verticalLayout->setStretch(1, 1); // 下方传输记录区域占1份

    // 初始化路径组合框显示
    updateLocalPathCombo();
}

void FileTransferWindow::initCLI()
{
    connect(&m_rtc_ctl, &WebRtcCtl::sendWsCliBinaryMsg, m_ws, &WsCli::sendWsCliBinaryMsg);
    connect(&m_rtc_ctl, &WebRtcCtl::sendWsCliTextMsg, m_ws, &WsCli::sendWsCliTextMsg);
    connect(m_ws, &WsCli::onWsCliRecvBinaryMsg, &m_rtc_ctl, &WebRtcCtl::onWsCliRecvBinaryMsg);
    connect(m_ws, &WsCli::onWsCliRecvTextMsg, &m_rtc_ctl, &WebRtcCtl::onWsCliRecvTextMsg);

    // 配置rtc工作逻辑
    connect(this, &FileTransferWindow::initRtcCtl, &m_rtc_ctl, &WebRtcCtl::init);
    connect(this, &FileTransferWindow::inputChannelSendMsg, &m_rtc_ctl, &WebRtcCtl::inputChannelSendMsg);
    connect(this, &FileTransferWindow::fileChannelSendMsg, &m_rtc_ctl, &WebRtcCtl::fileChannelSendMsg);
    connect(this, &FileTransferWindow::fileTextChannelSendMsg, &m_rtc_ctl, &WebRtcCtl::fileTextChannelSendMsg);
    connect(this, &FileTransferWindow::uploadFile2CLI, &m_rtc_ctl, &WebRtcCtl::uploadFile2CLI);

    connect(&m_rtc_ctl, &WebRtcCtl::recvGetFileList, this, &FileTransferWindow::recvGetFileList);
    connect(&m_rtc_ctl, &WebRtcCtl::recvDownloadFile, this, &FileTransferWindow::recvDownloadFile);
    connect(&m_rtc_ctl, &WebRtcCtl::recvUploadFileRes, this, &FileTransferWindow::recvUploadFileRes);

    m_rtc_ctl_thread.setObjectName("FileTransferWindow-WebRtcCtlThread");
    m_rtc_ctl.moveToThread(&m_rtc_ctl_thread);
    m_rtc_ctl_thread.start();
}
// 处理键盘事件
void FileTransferWindow::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        QWidget *focusedWidget = QApplication::focusWidget();
        if (QTableWidget *table = qobject_cast<QTableWidget *>(focusedWidget))
        {
            // 当前焦点在QTableWidget上
            if (table == ui->localTable)
            {
                QTableWidgetItem *currentItem = table->currentItem();
                if (currentItem)
                {
                    on_localTable_cellDoubleClicked(table->currentRow(), 0);
                }
            }
        }
    }
}
// 设置文件列表
void FileTransferWindow::setupFileTables()
{
    // 本地文件表格设置
    ui->localTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // ui->localTable->setAlternatingRowColors(true);
    ui->localTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->localTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);

    // 远程文件表格设置
    ui->remoteTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // ui->remoteTable->setAlternatingRowColors(true);
    ui->remoteTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->remoteTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
}
// 设置传输记录表格
void FileTransferWindow::setupLogTable()
{
    // 传输记录表格设置
    ui->transferLogTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // ui->transferLogTable->setAlternatingRowColors(true);
    ui->transferLogTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    ui->transferLogTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->transferLogTable->verticalHeader()->setVisible(false);

    // 修改列数和标题：发送路径、接收路径、状态、操作
    ui->transferLogTable->setColumnCount(4);
    ui->transferLogTable->setHorizontalHeaderLabels({"发送路径", "接收路径", "状态", "操作"});
}
// 填充本地文件列表
void FileTransferWindow::populateLocalFiles()
{
    currentLocalDir.setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    currentLocalDir.setSorting(QDir::Name | QDir::DirsFirst); // 按名称排序，文件夹优先
    // 遍历条目并分类
    QFileInfoList list = currentLocalDir.entryInfoList();

    int listSize = list.size() + 1;
    ui->localTable->setRowCount(listSize);

    // 创建上一级目录
    QTableWidgetItem *item_0_0 = ui->localTable->takeItem(0, 0);
    if (item_0_0)
        delete item_0_0; // 显式释放

    QTableWidgetItem *parentDir = new QTableWidgetItem("..");
    parentDir->setIcon(dirIcon);
    ui->localTable->setItem(0, 0, parentDir);
    // 填充当前目录文件
    for (int i = 0; i < list.size(); i++)
    {
        int row = i + 1;
        const QFileInfo &info = list.at(i);
        QTableWidgetItem *item_0 = ui->localTable->takeItem(row, 0);
        if (item_0)
            delete item_0; // 显式释放
        QTableWidgetItem *item_1 = ui->localTable->takeItem(row, 1);
        if (item_1)
            delete item_1; // 显式释放
        QTableWidgetItem *item_2 = ui->localTable->takeItem(row, 2);
        if (item_2)
            delete item_2; // 显式释放
        QTableWidgetItem *item_3 = ui->localTable->takeItem(row, 3);
        if (item_3)
            delete item_3; // 显式释放

        // 修改时间
        QDateTime lastModTime = info.lastModified();
        QString lastModTimeStr = lastModTime.toString("yyyy-MM-dd hh:mm:ss");
        QTableWidgetItem *lastModTimeItem = new QTableWidgetItem(lastModTimeStr);
        // lastModTimeItem->setTextAlignment(Qt::AlignCenter); // 水平+垂直居中
        ui->localTable->setItem(row, 2, lastModTimeItem);

        // 文件名称
        QString name = info.fileName();
        QTableWidgetItem *nameItem = new QTableWidgetItem(name);
        // nameItem->setTextAlignment(Qt::AlignCenter); // 水平+垂直居中
        ui->localTable->setItem(row, 0, nameItem);

        if (info.isDir())
        {
            nameItem->setIcon(dirIcon);
        }
        else
        {
            nameItem->setIcon(fileIcon);
            // 文件大小
            qint64 size = info.size();
            QString sizeStr = Convert::formatFileSize(size);
            QTableWidgetItem *sizeItem = new QTableWidgetItem(sizeStr);
            // sizeItem->setTextAlignment(Qt::AlignCenter); // 水平+垂直居中
            ui->localTable->setItem(row, 1, sizeItem);

            QString fileType = info.suffix();
            QTableWidgetItem *fileTypeItem = new QTableWidgetItem(fileType);
            // fileTypeItem->setTextAlignment(Qt::AlignCenter); // 水平+垂直居中
            ui->localTable->setItem(row, 3, fileTypeItem);
        }
    }
}
// 填充远程文件列表
void FileTransferWindow::populateRemoteFiles()
{
    // 创建上一级目录
    ui->remoteTable->setRowCount(remoteFiles.size() + 1);
    QTableWidgetItem *item_0_0 = ui->remoteTable->takeItem(0, 0);
    if (item_0_0)
        delete item_0_0; // 显式释放

    // 创建上一级目录
    QTableWidgetItem *parentDir = new QTableWidgetItem("..");
    parentDir->setIcon(dirIcon);
    ui->remoteTable->setItem(0, 0, parentDir);

    for (int i = 0; i < remoteFiles.size(); i++)
    {
        int row = i + 1;
        QTableWidgetItem *item_0 = ui->remoteTable->takeItem(row, 0);
        if (item_0)
            delete item_0; // 显式释放
        QTableWidgetItem *item_1 = ui->remoteTable->takeItem(row, 1);
        if (item_1)
            delete item_1; // 显式释放
        QTableWidgetItem *item_2 = ui->remoteTable->takeItem(row, 2);
        if (item_2)
            delete item_2; // 显式释放
        QTableWidgetItem *item_3 = ui->remoteTable->takeItem(row, 3);
        if (item_3)
            delete item_3; // 显式释放

        QJsonObject obj = remoteFiles.at(i).toObject();
        QString fileName = JsonUtil::getString(obj, Constant::KEY_NAME);
        if (!fileName.isEmpty())
        {
            QTableWidgetItem *nameItem = new QTableWidgetItem(fileName);
            bool isDir = JsonUtil::getBool(obj, Constant::KEY_IS_DIR);
            if (isDir)
            {
                nameItem->setIcon(dirIcon);
            }
            else
            {
                nameItem->setIcon(fileIcon);
            }
            ui->remoteTable->setItem(row, 0, nameItem);
        }

        qint64 fileSize = JsonUtil::getInt64(obj, Constant::KEY_FILE_SIZE);
        if (fileSize > 0)
        {
            QString fileSizeStr = Convert::formatFileSize(fileSize);
            ui->remoteTable->setItem(row, 1, new QTableWidgetItem(fileSizeStr));
        }

        QString lastModTime = JsonUtil::getString(obj, Constant::KEY_FILE_LAST_MOD_TIME);
        if (!lastModTime.isEmpty())
        {
            ui->remoteTable->setItem(row, 2, new QTableWidgetItem(lastModTime));
        }

        QString fileSuffix = JsonUtil::getString(obj, Constant::KEY_FILE_SUFFIX);
        if (!fileSuffix.isEmpty())
        {
            ui->remoteTable->setItem(row, 3, new QTableWidgetItem(fileSuffix));
        }
    }
}
// 处理上传按钮点击事件
void FileTransferWindow::onUploadButtonClicked()
{
    if (!connected)
    {
        return;
    }
    QList<QTableWidgetItem *> localSelected = ui->localTable->selectedItems();
    if (localSelected.isEmpty())
    {
        return;
    }
    QString fileName = localSelected[0]->text();
    // 检查文件是否存在
    if (!QFile::exists(currentLocalDir.absoluteFilePath(fileName)))
    {
        LOG_ERROR("文件不存在: " + fileName);
        if (ConfigUtil->showUI)
        {
            QMessageBox::warning(this, "错误", "文件不存在: " + fileName);
        }
        return;
    }
    // 在实际应用中实现传输逻辑
    int row = ui->transferLogTable->rowCount();
    ui->transferLogTable->insertRow(row);

    // 设置发送路径（本地完整路径）
    QString localFullPath = QDir::cleanPath(currentLocalDir.absolutePath() + "/" + fileName);
    ui->transferLogTable->setItem(row, 0, new QTableWidgetItem(localFullPath));

    // 设置接收路径（远程路径）
    QString remotePath = ui->remotePathCombo->currentText();
    QString remoteFullPath = QDir::cleanPath(remotePath + "/" + fileName);
    ui->transferLogTable->setItem(row, 1, new QTableWidgetItem(remoteFullPath));

    ui->transferLogTable->setItem(row, 2, new QTableWidgetItem("等待中"));
    ui->transferLogTable->setItem(row, 3, new QTableWidgetItem("上传"));

    // 获取当前远程路径
    emit uploadFile2CLI(localFullPath, remoteFullPath);
}
void FileTransferWindow::onDownloadButtonClicked()
{
    if (!connected)
    {
        return;
    }
    QList<QTableWidgetItem *> remoteSelected = ui->remoteTable->selectedItems();
    if (remoteSelected.isEmpty())
    {
        return;
    }
    QString fileName = remoteSelected[0]->text();
    QString fileSize = remoteSelected[1]->text();
    // 在实际应用中实现传输逻辑
    int row = ui->transferLogTable->rowCount();
    ui->transferLogTable->insertRow(row);

    // 设置发送路径（远程完整路径）
    QString remoteFullPath = QDir::cleanPath(currentRemotePath + "/" + fileName);
    ui->transferLogTable->setItem(row, 0, new QTableWidgetItem(remoteFullPath));

    // 设置接收路径（本地路径）
    QString localFullPath = QDir::cleanPath(currentLocalDir.absolutePath() + "/" + fileName);
    ui->transferLogTable->setItem(row, 1, new QTableWidgetItem(localFullPath));

    ui->transferLogTable->setItem(row, 2, new QTableWidgetItem("等待中"));
    ui->transferLogTable->setItem(row, 3, new QTableWidgetItem("下载"));

    // 通知远端发送该文件，统一通过文件通道发送
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_DOWNLOAD)
                          .add(Constant::KEY_PATH_CTL, localFullPath)
                          .add(Constant::KEY_PATH_CLI, remoteFullPath)           // 被控端期望的字段名
                          .add("isDirectory", fileSize.isEmpty()) // 是否是目录
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    // LOG_DEBUG(msg);
    rtc::message_variant msgStr(msg.toStdString());
    emit fileTextChannelSendMsg(msgStr);
}
// 接收文件消息
void FileTransferWindow::recvGetFileList(const QJsonObject &object)
{
    LOG_DEBUG("Received file list response: {}", JsonUtil::toCompactString(object));

    if (!connected)
    {
        connected = true;
    }

    ui->remotePathCombo->clear();
    // 解析文件列表
    if (object.contains(Constant::KEY_FOLDER_FILES))
    {
        remoteFiles = object.value(Constant::KEY_FOLDER_FILES).toArray();
        populateRemoteFiles();

        // 更新当前远程路径
        if (object.contains(Constant::KEY_PATH))
        {
            QString receivedPath = JsonUtil::getString(object, Constant::KEY_PATH);
            if (!receivedPath.isEmpty())
            {
                currentRemotePath = receivedPath;
                // 更新远程路径显示
                updateRemotePathCombo();
            }
        }
    }

    if (object.contains(Constant::KEY_FOLDER_MOUNTED))
    {
        QJsonArray mountedList = object.value(Constant::KEY_FOLDER_MOUNTED).toArray();
        // 遍历文件列表并添加到remoteFiles
        for (const QJsonValue &value : mountedList)
        {
            if (value.isString())
            {
                QString mountPath = value.toString();
                // 避免重复添加当前路径
                if (mountPath != currentRemotePath)
                {
                    ui->remotePathCombo->addItem(mountPath);
                }
            }
        }
        // 更新远程路径显示
        ui->remotePathCombo->setCurrentText(currentRemotePath);
    }
    else
    {
        // 如果没有挂载信息，只更新当前路径
        updateRemotePathCombo();
    }
}

// 处理本地路径选择变化
void FileTransferWindow::on_localPathCombo_textActivated(const QString &path)
{
    bool status = true;

    // 检查是否是有效的绝对路径
    if (QDir::isAbsolutePath(path))
    {
        QDir newDir(path);
        if (newDir.exists())
        {
            currentLocalDir = newDir;
        }
        else
        {
            status = false;
        }
    }
    else
    {
        // 相对路径处理（向上导航等）
        status = currentLocalDir.cd(path);
    }

    if (status)
    {
        populateLocalFiles();
        updateLocalPathCombo();
    }
}

// 处理本地文件列表双击事件
void FileTransferWindow::on_localTable_cellDoubleClicked(int row, int column)
{
    const QTableWidgetItem *item = ui->localTable->item(row, 0);
    QString filePath = item->text();

    bool status = false;
    if (filePath == "..")
    {
        // 向上一级目录
        status = currentLocalDir.cdUp();
    }
    else
    {
        // 进入子目录
        status = currentLocalDir.cd(filePath);
    }

    if (status)
    {
        populateLocalFiles();
        updateLocalPathCombo();
    }
}
void FileTransferWindow::on_remotePathCombo_textActivated(const QString &path)
{
    // 发送给远端，统一通过文件通道
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_LIST)
                          .add(Constant::KEY_PATH, path)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    LOG_DEBUG("Sending file list request (combo): {}", QString::fromUtf8(msg));

    rtc::message_variant msgStr(msg.toStdString());
    emit fileTextChannelSendMsg(msgStr);
}
// 处理远程路径选择变化
void FileTransferWindow::on_remoteTable_cellDoubleClicked(int row, int column)
{
    const QTableWidgetItem *item = ui->remoteTable->item(row, 0);
    QString path = item->text();
    if (!connected)
    {
        return;
    }

    QString filePath;
    if (path == "..")
    {
        // 向上一级目录
        filePath = currentRemotePath.mid(0, currentRemotePath.lastIndexOf('/') + 1);
        if (filePath.isEmpty())
        {
            filePath = Constant::FOLDER_HOME; // 根目录
        }
    }
    else
    {
        // 子目录
        filePath = QDir::cleanPath(currentRemotePath + '/' + path); // 确保路径格式正确
    }

    // 发送给远端，统一通过文件通道
    QJsonObject obj = JsonUtil::createObject()
                          .add(Constant::KEY_MSGTYPE, Constant::TYPE_FILE_LIST)
                          .add(Constant::KEY_PATH, filePath)
                          .build();

    QByteArray msg = JsonUtil::toCompactBytes(obj);
    LOG_DEBUG("Sending file list request (double-click): {}", QString::fromUtf8(msg));

    rtc::message_variant msgStr(msg.toStdString());
    emit fileTextChannelSendMsg(msgStr);
}

void FileTransferWindow::recvDownloadFile(bool status, const QString &filePath)
{
    int rowCount = ui->transferLogTable->rowCount();
    int targetRow = -1;
    for (int row = 0; row < rowCount; ++row)
    {
        QTableWidgetItem *item = ui->transferLogTable->item(row, 0);  // 第一列
        QTableWidgetItem *item1 = ui->transferLogTable->item(row, 1); // 第二列
        if ((item && item->text() == filePath) || (item1 && item1->text() == filePath))
        {
            targetRow = row;
            break;
        }
    }

    if (targetRow >= 0)
    {
        QTableWidgetItem *targetItem_2 = ui->transferLogTable->takeItem(targetRow, 2); // 删除第3列的旧项
        if (targetItem_2)
            delete targetItem_2; // 显式释放

        if (!status)
        {
            ui->transferLogTable->setItem(targetRow, 2, new QTableWidgetItem("失败"));
        }
        else
        {
            ui->transferLogTable->setItem(targetRow, 2, new QTableWidgetItem("成功"));
            // 刷新本地文件列表以显示新下载的文件
            populateLocalFiles();
        }
    }
}

// 更新本地路径组合框显示
void FileTransferWindow::updateLocalPathCombo()
{
    ui->localPathCombo->clear();

    // 添加当前路径
    QString currentPath = currentLocalDir.absolutePath();
    ui->localPathCombo->addItem(currentPath);
    // 添加驱动器根目录
    QList<QStorageInfo> volumes = QStorageInfo::mountedVolumes();
    for (const QStorageInfo &volume : volumes)
    {
        if (volume.isValid() && volume.isReady())
        {
            ui->localPathCombo->addItem(volume.rootPath());
        }
    }

    ui->localPathCombo->setCurrentText(currentPath);
}

// 更新远程路径组合框显示
void FileTransferWindow::updateRemotePathCombo()
{
    ui->remotePathCombo->clear();

    // 添加当前远程路径
    if (!currentRemotePath.isEmpty())
    {
        ui->remotePathCombo->addItem(currentRemotePath);
        ui->remotePathCombo->setCurrentText(currentRemotePath);
    }
}

void FileTransferWindow::recvUploadFileRes(bool status, const QString &filePath)
{
    if (status)
    {
        on_remotePathCombo_textActivated(ui->remotePathCombo->currentText());
    }
    // 从完整路径中提取文件名用于匹配
    int rowCount = ui->transferLogTable->rowCount();
    int targetRow = -1;
    for (int row = 0; row < rowCount; ++row)
    {
        QTableWidgetItem *item = ui->transferLogTable->item(row, 0); // 第一列
        QTableWidgetItem *item1 = ui->transferLogTable->item(row, 1); // 第二列
        if ((item && item->text() == filePath) || (item1 && item1->text() == filePath))
        {
            targetRow = row;
            break;
        }
    }

    if (targetRow >= 0)
    {
        QTableWidgetItem *targetItem_2 = ui->transferLogTable->takeItem(targetRow, 2); // 删除第2列的旧项
        if (targetItem_2)
            delete targetItem_2; // 显式释放

        ui->transferLogTable->setItem(targetRow, 2, new QTableWidgetItem(status ? "成功" : "失败"));
    }
}
