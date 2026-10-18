// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef APPIMAGEUPDATE_H
#define APPIMAGEUPDATE_H

#include "mostQtHeaders.h"
#include "utilsVersion.h"
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryFile>
#include <memory>

// Fork-owned AppImage integration. Never elevates privileges or restarts the editor.
class AppImageUpdate : public QObject
{
    Q_OBJECT
public:
    struct Asset {
        QUrl url;
        QByteArray digest;
        qint64 size = 0;
        bool isValid() const { return url.isValid() && digest.size() == 32 && size > 0; }
    };
    explicit AppImageUpdate(QObject *parent = nullptr);
    static QString installedImage();
    static Asset releaseAsset(const QJsonObject &release);
    static QByteArray fileDigest(const QString &path);
    static bool isImage(const QString &path);
    static bool installVerified(const QString &target, const QString &candidate,
                                const QByteArray &oldDigest, const QByteArray &newDigest,
                                QString *backupPath, QString *error);
    bool download(const QJsonObject &currentRelease, const QJsonObject &nextRelease);
    QString pendingVersion() const;
    void installPending(int updateLevel);
    bool isDownloading() const { return !m_reply.isNull(); }
signals:
    void statusChanged(const QString &message);
    void failed(const QString &message);
private:
    QString cacheDirectory() const;
    QString manifestPath() const;
    QJsonObject pending() const;
    void finishDownload();
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QTemporaryFile> m_download;
    QJsonObject m_manifest;
    Asset m_asset;
    qint64 m_received = 0;
    QString m_writeError;
};

#endif
