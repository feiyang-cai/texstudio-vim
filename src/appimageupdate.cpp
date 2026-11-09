// SPDX-License-Identifier: GPL-2.0-or-later
#include "appimageupdate.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#ifdef Q_OS_LINUX
#include <unistd.h>
#endif

namespace {
constexpr qint64 maximumImageSize = 1024LL * 1024 * 1024;
bool copyBytes(QIODevice &input, QIODevice &output, QCryptographicHash *hash = nullptr)
{
    while (!input.atEnd()) {
        const QByteArray bytes = input.read(1024 * 1024);
        if (bytes.isEmpty() || output.write(bytes) != bytes.size()) return false;
        if (hash) hash->addData(bytes);
    }
    return true;
}
bool stagedName(const QString &name)
{
    return QRegularExpression("^download-[a-zA-Z0-9]+\\.AppImage$").match(name).hasMatch();
}
}

AppImageUpdate::AppImageUpdate(QObject *parent) : QObject(parent) {}

bool AppImageUpdate::isImage(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray header = file.read(20);
    return header.size() == 20 && header.left(4) == QByteArray("\x7f" "ELF", 4)
            && header[4] == 2 && header[5] == 1 // ELF64, little endian
            && header.mid(8, 3) == QByteArray("AI\x02", 3)
            && header.mid(18, 2) == QByteArray("\x3e\x00", 2); // x86_64
}

QString AppImageUpdate::installedImage()
{
#if defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    const QString path = QFileInfo(qEnvironmentVariable("APPIMAGE")).canonicalFilePath();
    const QFileInfo info(path);
    if (getuid() != 0 && info.ownerId() == getuid() && info.isWritable()
            && info.isExecutable() && QFileInfo(info.absolutePath()).isWritable() && isImage(path))
        return path;
#endif
    return {};
}

AppImageUpdate::Asset AppImageUpdate::releaseAsset(const QJsonObject &release)
{
    const QString tag = release.value("tag_name").toString();
    const Version version = Version::fromVimTag(tag);
    if (release.value("draft").toBool() || release.value("published_at").toString().isEmpty()
            || !version.isValid() || version.commitsAfter != 0) return {};
    const QString name = tag + "-x86_64.AppImage";
    const QUrl expected("https://github.com/feiyang-cai/texstudio-vim/releases/download/" + tag + "/" + name);
    Asset found;
    for (const auto &value : release.value("assets").toArray()) {
        const auto asset = value.toObject();
        if (asset.value("name").toString() != name) continue;
        if (found.isValid()) return {}; // ambiguous payload
        const QString digest = asset.value("digest").toString();
        const double size = asset.value("size").toDouble();
        if (asset.value("state").toString() != "uploaded"
                || QUrl(asset.value("browser_download_url").toString()) != expected
                || !QRegularExpression("^sha256:[0-9a-fA-F]{64}$").match(digest).hasMatch()
                || size <= 0 || size > maximumImageSize || size != qint64(size)) return {};
        found = {expected, QByteArray::fromHex(digest.mid(7).toLatin1()), qint64(size)};
    }
    return found;
}

QByteArray AppImageUpdate::fileDigest(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return hash.result();
}

bool AppImageUpdate::installVerified(const QString &target, const QString &candidate,
                                    const QByteArray &oldDigest, const QByteArray &newDigest,
                                    QString *backupPath, QString *error)
{
    auto fail = [&](const QString &reason) { if (error) *error = reason; return false; };
    if (backupPath) backupPath->clear();
    if (oldDigest.size() != 32 || newDigest.size() != 32 || target == candidate
            || !isImage(target) || !isImage(candidate) || fileDigest(candidate) != newDigest)
        return fail(tr("The update package could not be verified."));
    if (fileDigest(target) != oldDigest)
        return fail(tr("The installed AppImage changed. It has not been replaced."));
    const QFile::Permissions permissions = QFileInfo(target).permissions();
    QTemporaryFile backup(target + ".previous-XXXXXX.AppImage");
    QFile original(target);
    if (!backup.open() || !original.open(QIODevice::ReadOnly) || !copyBytes(original, backup)
            || !backup.flush() || !backup.setPermissions(permissions))
        return fail(tr("Could not back up the current AppImage."));
    original.close();
    backup.close();
    if (fileDigest(backup.fileName()) != oldDigest)
        return fail(tr("The installed AppImage changed during backup."));
    QSaveFile replacement(target);
    replacement.setDirectWriteFallback(false);
    QFile update(candidate);
    QCryptographicHash copied(QCryptographicHash::Sha256);
    if (!update.open(QIODevice::ReadOnly) || !replacement.open(QIODevice::WriteOnly)
            || !replacement.setPermissions(permissions) || !copyBytes(update, replacement, &copied))
        return fail(tr("Could not install the update. The current AppImage is unchanged."));
    update.close();
    if (copied.result() != newDigest || fileDigest(candidate) != newDigest || QFileInfo(target).isSymLink()
            || fileDigest(target) != oldDigest) {
        replacement.cancelWriting();
        return fail(tr("An AppImage changed during installation. The update was cancelled."));
    }
    if (!replacement.commit()) return fail(tr("Could not replace the AppImage atomically."));
    backup.setAutoRemove(false);
    if (backupPath) *backupPath = backup.fileName();
    return true;
}

QString AppImageUpdate::cacheDirectory() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/texstudio-vim/updates";
}

QString AppImageUpdate::manifestPath() const
{
    const QString image = installedImage();
    if (image.isEmpty()) return {};
    const auto key = QCryptographicHash::hash(image.toUtf8(), QCryptographicHash::Sha256).toHex();
    return cacheDirectory() + "/" + QString::fromLatin1(key) + ".json";
}

QJsonObject AppImageUpdate::pending() const
{
    const QString path = manifestPath();
    if (path.isEmpty()) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 16384) return {};
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value("target").toString() != installedImage()
            || !stagedName(object.value("file").toString())) return {};
    return object;
}

QString AppImageUpdate::pendingVersion() const
{
    const auto object = pending();
    const Version current = Version::current();
    const QString tag = object.value("to_tag").toString();
    if (object.value("from_tag").toString() != Version::versionToString(current)
            || current.commitsAfter != 0 || !(Version::fromVimTag(tag) > current)) return {};
    const QString candidate = cacheDirectory() + "/" + object.value("file").toString();
    const QByteArray digest = QByteArray::fromHex(object.value("new_digest").toString().toLatin1());
    if (digest.size() != 32 || QFileInfo(candidate).size() != object.value("size").toDouble()
            || !isImage(candidate) || fileDigest(candidate) != digest) return {};
    return tag;
}

bool AppImageUpdate::download(const QJsonObject &currentRelease, const QJsonObject &nextRelease)
{
    if (isDownloading()) return true;
    const QString target = installedImage();
    const Version current = Version::current();
    const QString from = currentRelease.value("tag_name").toString();
    const QString to = nextRelease.value("tag_name").toString();
    const Asset oldAsset = releaseAsset(currentRelease);
    m_asset = releaseAsset(nextRelease);
    // Match the on-disk image to the running published version. This prevents
    // overwriting a newer image installed externally while this process runs.
    if (target.isEmpty() || current.commitsAfter != 0 || from != Version::versionToString(current)
            || !(Version::fromVimTag(to) > current) || !oldAsset.isValid() || !m_asset.isValid()
            || QFileInfo(target).size() != oldAsset.size || fileDigest(target) != oldAsset.digest)
        return false;
    const Version staged = Version::fromVimTag(pendingVersion());
    if (staged.isValid() && !(Version::fromVimTag(to) > staged)) {
        emit statusChanged(tr("Update %1 is downloaded and will install when you close texstudio-vim.")
                           .arg(Version::versionToString(staged)));
        return true;
    }
    if (!QDir().mkpath(cacheDirectory())
            || !QFile::setPermissions(cacheDirectory(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return false;
    m_download.reset(new QTemporaryFile(cacheDirectory() + "/download-XXXXXX.AppImage"));
    if (!m_download->open()) { m_download.reset(); return false; }
    m_manifest = {{"target", target}, {"from_tag", from}, {"to_tag", to},
                  {"old_digest", QString::fromLatin1(oldAsset.digest.toHex())},
                  {"new_digest", QString::fromLatin1(m_asset.digest.toHex())},
                  {"file", QFileInfo(m_download->fileName()).fileName()}, {"size", double(m_asset.size)}};
    m_received = 0;
    m_writeError.clear();
    if (!m_network) m_network = new QNetworkAccessManager(this);
    QNetworkRequest request(m_asset.url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(60000);
#endif
    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        const QByteArray bytes = m_reply->readAll();
        m_received += bytes.size();
        if (m_received > m_asset.size || m_download->write(bytes) != bytes.size()) {
            m_writeError = tr("The download size or available disk space is invalid.");
            m_reply->abort();
        }
    });
    connect(m_reply, &QNetworkReply::finished, this, &AppImageUpdate::finishDownload);
    emit statusChanged(tr("Downloading %1. Installation will wait until texstudio-vim closes.").arg(to));
    return true;
}

void AppImageUpdate::finishDownload()
{
    if (!m_download) return;
    QNetworkReply *reply = m_reply;
    if (reply && reply->bytesAvailable()) {
        const QByteArray bytes = reply->readAll();
        m_received += bytes.size();
        if (m_received > m_asset.size || m_download->write(bytes) != bytes.size())
            m_writeError = tr("The download size or available disk space is invalid.");
    }
    const bool networkOk = reply && reply->error() == QNetworkReply::NoError;
    if (reply) reply->deleteLater();
    m_reply.clear();
    bool ok = networkOk && m_writeError.isEmpty() && m_received == m_asset.size && m_download->flush();
    m_download->close();
    ok = ok && isImage(m_download->fileName()) && fileDigest(m_download->fileName()) == m_asset.digest;
    const QString manifest = manifestPath();
    QLockFile lock(manifest + ".lock");
    if (ok && !manifest.isEmpty() && lock.tryLock(0)) {
        const QJsonObject previous = pending();
        const Version previousVersion = Version::fromVimTag(pendingVersion());
        const Version newVersion = Version::fromVimTag(m_manifest.value("to_tag").toString());
        if (previous.value("from_tag") == m_manifest.value("from_tag")
                && previousVersion.isValid() && previousVersion > newVersion) {
            m_download.reset();
            emit statusChanged(tr("A newer update is already downloaded."));
            return;
        }
        QSaveFile file(manifest);
        const auto data = QJsonDocument(m_manifest).toJson(QJsonDocument::Compact);
        ok = file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
                && file.write(data) == data.size() && file.commit();
        if (ok) {
            m_download->setAutoRemove(false);
            const QString oldFile = previous.value("file").toString();
            if (stagedName(oldFile) && oldFile != m_manifest.value("file").toString())
                QFile::remove(cacheDirectory() + "/" + oldFile);
        }
    } else ok = false;
    m_download.reset();
    if (ok) emit statusChanged(tr("Update %1 is downloaded and will install when you close texstudio-vim.")
                              .arg(m_manifest.value("to_tag").toString()));
    else emit failed(tr("Automatic update could not be downloaded and verified. Your AppImage is unchanged."));
}

void AppImageUpdate::installPending(int updateLevel)
{
    if (updateLevel < 0 || updateLevel > 2) return;
    const QString manifest = manifestPath();
    if (manifest.isEmpty() || pendingVersion().isEmpty()) return;
    QLockFile lock(manifest + ".lock");
    if (!lock.tryLock(0)) return;
    const QJsonObject object = pending();
    const Version next = Version::fromVimTag(pendingVersion());
    if (!next.isValid() || (next.type == "rc" && updateLevel < 1)
            || (next.type != "rc" && next.type != "stable" && updateLevel < 2)) return;
    QString backup, error;
    const QString candidate = cacheDirectory() + "/" + object.value("file").toString();
    if (QFileInfo(candidate).size() != object.value("size").toDouble()
            || !installVerified(installedImage(), candidate,
                                QByteArray::fromHex(object.value("old_digest").toString().toLatin1()),
                                QByteArray::fromHex(object.value("new_digest").toString().toLatin1()),
                                &backup, &error)) {
        qWarning() << "AppImage update deferred:" << error;
        return;
    }
    QFile::remove(manifest);
    QFile::remove(candidate);
    qInfo() << "AppImage updated. Previous version:" << backup;
}
