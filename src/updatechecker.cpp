#include "updatechecker.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include "smallUsefulFunctions.h"
#include "utilsVersion.h"
#include "configmanager.h"
#include <QNetworkProxyFactory>
#include <QMutex>
#if QT_VERSION_MAJOR>5
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#endif

UpdateChecker *UpdateChecker::m_Instance = nullptr;
int comboBoxUpdateLevel;

UpdateChecker::UpdateChecker() :
    QObject(nullptr), silent(true)
{
	QNetworkProxyFactory::setUseSystemConfiguration(true);
	//networkManager = new QNetworkAccessManager();
    // activate networkManager only when directly needed as it causes network delays on mac (Bug 1717/1738)
    networkManager=nullptr;
}

UpdateChecker::~UpdateChecker()
{
    m_Instance = nullptr;
}

QString UpdateChecker::lastCheckAsString()
{
	QDateTime lastCheck = ConfigManager::getInstance()->getOption("Update/LastCheck").toDateTime();
	return lastCheck.isValid() ? QLocale().toString(lastCheck, QLocale::ShortFormat) : tr("Never", "last update");
}

void UpdateChecker::autoCheck()
{
	ConfigManagerInterface *cfg  = ConfigManager::getInstance();
	bool autoCheck = cfg->getOption("Update/AutoCheck").toBool();
	if (autoCheck) {
		QDateTime lastCheck = cfg->getOption("Update/LastCheck").toDateTime();
		int checkInterval = cfg->getOption("Update/AutoCheckInvervalDays").toInt();
		if (!lastCheck.isValid() || lastCheck.addDays(checkInterval) < QDateTime::currentDateTime()) {
			check();
		}
	}

}

void UpdateChecker::check(bool silent, int currentComboBoxUpdateLevel)
{
	// catch value if possible, s. comment at start of checkForNewVersion
	comboBoxUpdateLevel = currentComboBoxUpdateLevel;

	this->silent = silent;
    networkManager = new QNetworkAccessManager();
    QNetworkRequest request = QNetworkRequest(QUrl("https://api.github.com/repos/feiyang-cai/texstudio-vim/releases?per_page=100"));
	request.setRawHeader("User-Agent", "TeXstudio Update Checker");
	QNetworkReply *reply = networkManager->get(request);
	connect(reply, SIGNAL(finished()), this, SLOT(onRequestCompleted()));
	if (!silent)
#if QT_VERSION_MAJOR<6
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(onRequestError(QNetworkReply::NetworkError)));
#else
        connect(reply, &QNetworkReply::errorOccurred,this, &UpdateChecker::onRequestError);
#endif
}

void UpdateChecker::onRequestError(QNetworkReply::NetworkError )
{
	QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
	if (!reply) return;
	QString errorMessage = reply->errorString();
#if QT_VERSION_MAJOR>5
	QByteArray data = reply->readAll();
	QJsonParseError err;
	QJsonDocument doc = QJsonDocument::fromJson(data, &err);
	if (err.error == QJsonParseError::NoError && doc.isObject()) {
		QJsonObject obj = doc.object();
		if (obj.contains("message")) {
			errorMessage += "\n" + obj.value("message").toString();
		}
	}
#endif
	UtilsUi::txsCritical(tr("Update check failed with error:\n") + errorMessage);

    networkManager->deleteLater();
    networkManager=nullptr;
}

void UpdateChecker::onRequestCompleted()
{
	QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
	if (!reply || reply->error() != QNetworkReply::NoError) return;

	QByteArray ba = reply->readAll();

	parseData(ba);
	if (comboBoxUpdateLevel > -2)  // if not About dialog
		checkForNewVersion();

    networkManager->deleteLater();
    networkManager=nullptr;
}

QList<Version> UpdateChecker::releaseVersions(const QByteArray &data)
{
    QList<Version> versions;
    const auto document = QJsonDocument::fromJson(data);
    if (!document.isArray()) return versions;
    for (const auto &entry : document.array()) {
        const auto release = entry.toObject();
        if (release.value("draft").toBool() || release.value("published_at").toString().isEmpty()) continue;
        const Version version = Version::fromVimTag(release.value("tag_name").toString());
        if (!version.isValid() || version.commitsAfter != 0) continue;
        if (release.value("prerelease").toBool() && version.type == "stable") continue;
        versions << version;
    }
    return versions;
}

void UpdateChecker::parseData(const QByteArray &data)
{
    latestStableVersion = Version();
    latestReleaseCandidateVersion = Version();
    latestDevVersion = Version();
    for (const Version &version : releaseVersions(data)) {
        Version *latest = version.type == "stable" ? &latestStableVersion
                          : version.type == "rc" ? &latestReleaseCandidateVersion : &latestDevVersion;
        if (!latest->isValid() || version > *latest) *latest = version;
    }
    if (latestStableVersion.isValid()) emit dataParsed(latestStableVersion.versionNumber);
    if (!latestReleaseCandidateVersion.isValid()) latestReleaseCandidateVersion = latestStableVersion;
    if (!latestDevVersion.isValid()) latestDevVersion = latestStableVersion;
}

void UpdateChecker::checkForNewVersion()
{
	// updateLevel values from comboBoxUpdateLevel indices:
	// 0: stable, 1: release candidate, 2: development (alpha, beta)
	// Config dialog (check button) passes correct current index from dialog, so user can check with different settings without closing dialog
	// Auto check uses -1, since we do not have the current gui value. in this case we can stay with config value.
	// About dialog uses -2, so we can suppress Update dialog. But in this case this function isn't called.
	int updateLevel;
	if (comboBoxUpdateLevel > -1)
		updateLevel = comboBoxUpdateLevel;
	else // comboBoxUpdateLevel = -1
		updateLevel = ConfigManager::getInstance()->getOption("Update/UpdateLevel").toInt();

	bool checkReleaseCandidate = updateLevel >= 1;
	bool checkDevVersions = updateLevel >= 2;
	Version currentVersion = Version::current();
	QString downloadAddress = "https://github.com/feiyang-cai/texstudio-vim/releases";
	QString downloadAddressGit = "https://github.com/feiyang-cai/texstudio-vim/releases";

	if (!currentVersion.isValid() && !latestReleaseCandidateVersion.isValid() && !latestDevVersion.isValid()) {
		if (!silent) UtilsUi::txsWarning(tr("Update check failed (invalid update file format)."));
		return;
	}
	while (true) {  // single control loop, used be able to break the control flow when some new version is detected
		if (checkReleaseCandidate && !latestReleaseCandidateVersion.isEmpty()) {
			if (!latestReleaseCandidateVersion.isValid()) {
				if (!silent) UtilsUi::txsWarning(tr("Update check for release candidate failed (invalid update file format)."));
			}
			if (latestReleaseCandidateVersion > currentVersion && latestReleaseCandidateVersion > latestStableVersion) {
                notify(QString(tr("A new release candidate of TeXstudio is available.")+"<br><table><tr><td>"+
                                tr("Current version:")+"</td><td>%1</td></tr>"+
                                "<tr><td>"+tr("Latest stable version:")+"</td><td>%2</td></tr>"+
                               "<tr><td>"+tr("Release candidate:")+"</td><td>%3</td></tr></table><br><br>"+
                                tr("You can download it from the %1 TeXstudio website").arg(QString("<a href='%1'>").arg(downloadAddressGit))+"</a>."
                        )
						.arg(Version::versionToString(currentVersion),
							Version::versionToString(latestStableVersion),
                            Version::versionToString(latestReleaseCandidateVersion))
				);
				break;
			}
		}
		if (checkDevVersions && !latestDevVersion.isEmpty()) {
			if (!latestDevVersion.isValid()) {
				if (!silent) UtilsUi::txsWarning(tr("Update check for development version failed (invalid update file format)."));
			}
			if (latestDevVersion > currentVersion && (latestStableVersion.isEmpty() || latestDevVersion > latestStableVersion)) {
                notify(QString(tr("A new development version of TeXstudio is available.")+"<br><table><tr><td>"+
                                tr("Current version:")+"    </td><td>%1</td></tr>"+
                                "<tr><td>"+tr("Latest stable version:")+"</td><td>%2</td></tr>"+
                                "<tr><td>"+tr("Latest development version:")+"</td><td>%3</td></tr></table><br><br>"+
                                tr("You can download it from the %1 TeXstudio website").arg(QString("<a href='%1'>").arg(downloadAddressGit))+"</a>."
                        )
						.arg(Version::versionToString(currentVersion),
							Version::versionToString(latestStableVersion),
                            Version::versionToString(latestDevVersion))
				);
				break;
			}
		}
		if (!latestStableVersion.isEmpty()) {
			if (!latestStableVersion.isValid()) {
				if (!silent) UtilsUi::txsWarning(tr("Update check for stable version failed (invalid update file format)."));
			}
			if (latestStableVersion > currentVersion) {
                notify(QString(tr("A new stable version of TeXstudio is available.")+"<br><table><tr><td>"+
                                tr("Current version:")+"</td><td>%1</td></tr>"+
                                "<tr><td>"+tr("Latest stable version:")+"</td><td>%2</td></tr></table><br><br>"+
                                tr("You can download it from the %1 TeXstudio website").arg(QString("<a href='%1'>").arg(downloadAddressGit))+"</a>."
                        )
						.arg(Version::versionToString(currentVersion),
                            Version::versionToString(latestStableVersion))
				);
			} else {
				if (!silent) {
					UtilsUi::txsInformation(tr("Your TeXstudio version %1 is up-to-date.").arg(Version::versionToString(currentVersion)));
				}
			}
			break;
		} else {
            if (!silent) UtilsUi::txsInformation(tr("Failure to find current TeXstudio versions."));
		}
		break;
	}

	ConfigManager::getInstance()->setOption("Update/LastCheck", QDateTime::currentDateTime());
	emit checkCompleted();
}

void UpdateChecker::notify(QString message)
{
	QMessageBox msgBox;
	msgBox.setWindowTitle(tr("TeXstudio Update"));
	msgBox.setTextFormat(Qt::RichText);
	msgBox.setText(message);
	msgBox.setStandardButtons(QMessageBox::Ok);
	msgBox.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
	msgBox.exec();
}

UpdateChecker *UpdateChecker::instance()
{
	static QMutex mutex;
	mutex.lock();
	if (!m_Instance)
		m_Instance = new UpdateChecker();
	mutex.unlock();
	return m_Instance;
}
