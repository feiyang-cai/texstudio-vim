#include "updatechecker.h"
#include "appimageupdate.h"
#include "smallUsefulFunctions.h"
#include "configmanager.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkReply>
#include <QNetworkProxyFactory>
#include <QMutex>
#include <QTimer>

namespace {
bool validRelease(const QJsonObject &release)
{
    const Version version = Version::fromVimTag(release.value("tag_name").toString());
    return !release.value("draft").toBool() && !release.value("published_at").toString().isEmpty()
            && version.isValid() && version.commitsAfter == 0
            && !(release.value("prerelease").toBool() && version.type == "stable");
}
}

UpdateChecker *UpdateChecker::m_Instance = nullptr;

UpdateChecker::UpdateChecker() : QObject(qApp), m_appImageUpdate(new AppImageUpdate(this))
{
    QNetworkProxyFactory::setUseSystemConfiguration(true);
    auto report = [this](const QString &message) { m_status = message; emit updateStatus(message); };
    connect(m_appImageUpdate, &AppImageUpdate::statusChanged, this, report);
    connect(m_appImageUpdate, &AppImageUpdate::failed, this, report);
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
        const auto cfg = ConfigManager::getInstance();
        if (cfg->getOption("Update/AutoInstallAppImage").toBool())
            m_appImageUpdate->installPending(cfg->getOption("Update/UpdateLevel").toInt());
    });
    auto timer = new QTimer(this);
    timer->setInterval(60 * 60 * 1000);
    connect(timer, &QTimer::timeout, this, &UpdateChecker::autoCheck);
    timer->start();
}

UpdateChecker::~UpdateChecker() { m_Instance = nullptr; }

QString UpdateChecker::lastCheckAsString()
{
    const QDateTime lastCheck = ConfigManager::getInstance()->getOption("Update/LastCheck").toDateTime();
    return lastCheck.isValid() ? QLocale().toString(lastCheck, QLocale::ShortFormat) : tr("Never", "last update");
}

bool UpdateChecker::automaticUpdatesAvailable() { return !AppImageUpdate::installedImage().isEmpty(); }

QString UpdateChecker::automaticUpdateStatus() const
{
    if (!m_status.isEmpty()) return m_status;
    const QString pending = m_appImageUpdate->pendingVersion();
    if (!pending.isEmpty()) return tr("Update %1 is downloaded. Close normally to install it; the next launch uses the new version.").arg(pending);
    return automaticUpdatesAvailable()
            ? tr("Automatic installation is optional. Updates include TeXstudio and Vim and follow your Update Level.")
            : tr("Automatic installation requires a writable Linux x86_64 AppImage run as your normal user. Other packages use download notifications.");
}

void UpdateChecker::autoCheck()
{
    const auto cfg = ConfigManager::getInstance();
    if (m_requests || !cfg->getOption("Update/AutoCheck").toBool()) return;
    const QDateTime lastCheck = cfg->getOption("Update/LastCheck").toDateTime();
    const int interval = qMax(0, cfg->getOption("Update/AutoCheckInvervalDays").toInt());
    if (!lastCheck.isValid() || lastCheck.addDays(interval) < QDateTime::currentDateTime()) check();
}

void UpdateChecker::check(bool silent, int level)
{
    if (level == -1) level = ConfigManager::getInstance()->getOption("Update/UpdateLevel").toInt();
    // Capture each request's settings: About and Check Now may overlap.
    if (!networkManager) networkManager = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl("https://api.github.com/repos/feiyang-cai/texstudio-vim/releases?per_page=100"));
    request.setRawHeader("User-Agent", "texstudio-vim Update Checker");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(30000);
#endif
    QNetworkReply *reply = networkManager->get(request);
    ++m_requests;
    connect(reply, &QNetworkReply::finished, this, [this, reply, silent, level] {
        --m_requests;
        const QByteArray data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            if (!silent) UtilsUi::txsCritical(tr("Update check failed with error:\n") + reply->errorString());
        } else if (!QJsonDocument::fromJson(data).isArray()) {
            if (!silent) UtilsUi::txsWarning(tr("Update check failed (invalid update file format)."));
        } else {
            parseData(data);
            if (level > -2) {
                ConfigManager::getInstance()->setOption("Update/LastCheck", QDateTime::currentDateTime());
                checkForNewVersion(data, silent, level);
            }
        }
        reply->deleteLater();
        emit checkCompleted();
    });
}

QList<Version> UpdateChecker::releaseVersions(const QByteArray &data)
{
    QList<Version> versions;
    for (const auto &entry : QJsonDocument::fromJson(data).array()) {
        const auto release = entry.toObject();
        if (validRelease(release)) versions << Version::fromVimTag(release.value("tag_name").toString());
    }
    return versions;
}

QJsonObject UpdateChecker::newestRelease(const QByteArray &data, int level, const Version &current)
{
    if (!current.isValid() || level < 0 || level > 2) return {};
    Version newest = current;
    QJsonObject result;
    for (const auto &entry : QJsonDocument::fromJson(data).array()) {
        const auto release = entry.toObject();
        if (!validRelease(release)) continue;
        const Version version = Version::fromVimTag(release.value("tag_name").toString());
        if ((version.type == "rc" && level < 1)
                || (version.type != "rc" && version.type != "stable" && level < 2)) continue;
        if (version > newest) { newest = version; result = release; }
    }
    return result;
}

void UpdateChecker::parseData(const QByteArray &data)
{
    latestStableVersion = Version();
    for (const Version &version : releaseVersions(data)) {
        if (version.type == "stable" && (!latestStableVersion.isValid() || version > latestStableVersion))
            latestStableVersion = version;
    }
    if (latestStableVersion.isValid()) emit dataParsed(latestStableVersion.versionNumber);
}

void UpdateChecker::checkForNewVersion(const QByteArray &data, bool silent, int level)
{
    const Version current = Version::current();
    if (!current.isValid()) {
        if (!silent) UtilsUi::txsWarning(tr("The current version could not be determined."));
        return;
    }
    const auto next = newestRelease(data, level, current);
    if (next.isEmpty()) {
        if (!silent) UtilsUi::txsInformation(tr("No newer texstudio-vim release is available for your Update Level."));
        return;
    }
    const QString tag = next.value("tag_name").toString();
    if (ConfigManager::getInstance()->getOption("Update/AutoInstallAppImage").toBool() && automaticUpdatesAvailable()) {
        QJsonObject currentRelease;
        for (const auto &entry : QJsonDocument::fromJson(data).array()) {
            const auto release = entry.toObject();
            if (validRelease(release) && release.value("tag_name").toString() == Version::versionToString(current)) {
                currentRelease = release;
                break;
            }
        }
        if (m_appImageUpdate->download(currentRelease, next)) return;
        m_status = tr("Automatic update is unavailable for this package. Download the new version from our releases.");
        emit updateStatus(m_status);
    }
    const QString address = "https://github.com/feiyang-cai/texstudio-vim/releases/tag/" + tag;
    notify(tr("A new texstudio-vim release is available.<br>Current: %1<br>New: %2<br><br><a href='%3'>Download from our releases</a>")
           .arg(Version::versionToString(current).toHtmlEscaped(), tag.toHtmlEscaped(), address.toHtmlEscaped()));
}

void UpdateChecker::notify(const QString &message)
{
    QMessageBox box;
    box.setWindowTitle(tr("texstudio-vim Update"));
    box.setTextFormat(Qt::RichText);
    box.setText(message);
    box.setStandardButtons(QMessageBox::Ok);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    box.exec();
}

UpdateChecker *UpdateChecker::instance()
{
    static QMutex mutex;
    QMutexLocker locker(&mutex);
    if (!m_Instance) m_Instance = new UpdateChecker();
    return m_Instance;
}
