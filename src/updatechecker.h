#ifndef Header_UpdateChecker
#define Header_UpdateChecker

#include "mostQtHeaders.h"
#include "utilsVersion.h"
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QLocale>

class AppImageUpdate;
class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    ~UpdateChecker();
    static UpdateChecker *instance();
    static QString lastCheckAsString();
    static QList<Version> releaseVersions(const QByteArray &data);
    static QJsonObject newestRelease(const QByteArray &data, int level, const Version &current);
    static bool automaticUpdatesAvailable();
    QString automaticUpdateStatus() const;
    QString latestVersion() { return latestStableVersion.versionNumber; }
    void autoCheck();
    void check(bool silent = true, int level = -1);
signals:
    void dataParsed(QString versionNumber);
    void checkCompleted();
    void updateStatus(const QString &message);
private:
    UpdateChecker();
    Q_DISABLE_COPY(UpdateChecker)
    static UpdateChecker *m_Instance;
    QNetworkAccessManager *networkManager = nullptr;
    AppImageUpdate *m_appImageUpdate;
    int m_requests = 0;
    QString m_status;
    Version latestStableVersion;
    void parseData(const QByteArray &data);
    void checkForNewVersion(const QByteArray &data, bool silent, int level);
    void notify(const QString &message);
};
#endif
