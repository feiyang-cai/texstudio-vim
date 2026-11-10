#ifndef TXS_REMOTE_CLIENT_H
#define TXS_REMOTE_CLIENT_H
#include <QtCore>
#include <functional>

namespace RemoteWorkspace {
struct Profile {
    QString manifest, host, root, python = "python3", sshConfig, platform;
    bool ready = false;
    QString directory() const { return QFileInfo(manifest).absolutePath(); }
    QString cache() const { return directory() + "/project"; }
    bool save(QString *error = nullptr) const;
    static Profile load(const QString &manifest, QString *error = nullptr);
    bool isValid() const;
};
QString cacheBase();
bool validRelativePath(const QString &name);
// Directory entries end in a slash; files do not. Exact repeats are allowed.
bool validateProjectPaths(const QStringList &paths, QString *error = nullptr);
bool applyPull(const Profile &profile, QString *error = nullptr,
               const std::function<bool(const QString &)> &canOverwrite = {});
Profile forFile(const QString &filename);
QString toRemote(const Profile &profile, const QString &local);
QString toLocal(const Profile &profile, const QString &remote);
QString mapCommandArgument(const Profile &profile, const QString &argument);
QString mapSourcePath(const QString &context, const QString &remote);
QString previewDirectory(const QString &master);
QString workerProgram();
QStringList workerArguments(const Profile &profile, const QString &operation, const QJsonObject &data = {});
QByteArray fileHash(const QString &path);
bool writeJson(const QString &path, const QJsonObject &object, QString *error = nullptr);

class Connection {
public:
    using Output = std::function<void(const QString &, const QByteArray &)>;
    bool open(Profile &profile);
    QJsonObject request(const QString &operation, const QJsonObject &data = {}, const Output &output = {}, int timeout = 60000);
    QString error() const { return m_error; }
    ~Connection();
private:
    QProcess m_process;
    QByteArray m_buffer;
    QString m_error;
    int m_id = 0;
};
int workerMain(int argc, char **argv);
}
#endif
