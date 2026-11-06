#include "remoteclient.h"
#include <cstdio>
#include <stdexcept>

namespace RemoteWorkspace {
namespace {
class Failure : public std::runtime_error { public: explicit Failure(const QString &text) : std::runtime_error(text.toUtf8().constData()) {} };
void require(bool ok, const QString &error) { if (!ok) throw Failure(error); }
QJsonObject call(Connection &connection, const QString &op, const QJsonObject &data = {}, const Connection::Output &output = {}, int timeout = 60000)
{
    const auto result = connection.request(op, data, output, timeout);
    require(connection.error().isEmpty(), connection.error()); return result;
}
QString relativeName(const QString &name)
{
    require(validRelativePath(name), "The remote project contains a filename that is unsafe or incompatible with this client filesystem.");
    return name;
}
QString cachePath(const Profile &profile, const QString &relative)
{
    const QString name = relativeName(relative);
    QString path = profile.cache();
    for (const auto &part : name.split('/')) {
        path += "/" + part;
        require(!QFileInfo(path).isSymLink(), "A local symbolic link cannot be synchronized.");
    }
    return path;
}
QByteArray bytesAt(const QString &path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 64 * 1024 * 1024, "Cannot read project file, or file exceeds 64 MiB: " + path);
    const auto bytes = file.readAll(); require(file.error() == QFile::NoError, file.errorString()); return bytes;
}
class Mirror {
public:
    Profile &profile; Connection &connection; QJsonObject files;
    Mirror(Profile &p, Connection &c) : profile(p), connection(c) {
        QFile state(profile.directory() + "/state.json");
        if (state.open(QIODevice::ReadOnly)) {
            require(state.size() <= 64 * 1024 * 1024, "Workspace state is too large.");
            const auto document = QJsonDocument::fromJson(state.readAll());
            require(document.isObject(), "Workspace synchronization state is damaged. Local files were preserved.");
            files = document.object();
        }
    }
    void save() {
        QString error;
        require(writeJson(profile.directory() + "/state.json", files, &error), "Cannot save workspace synchronization state: " + error);
    }
    void push() {
        QDirIterator iterator(profile.cache(), QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            require(!iterator.fileInfo().isSymLink(), "A local symbolic link cannot be synchronized: " + path);
            if (iterator.fileInfo().isDir()) continue;
            require(iterator.fileInfo().isFile(), "Unsupported local project file: " + path);
            const QString name = relativeName(QDir(profile.cache()).relativeFilePath(path));
            const auto hash = fileHash(path);
            require(!hash.isEmpty(), "Cannot hash local project file: " + name);
            const auto previous = files.value(name).toObject();
            if (previous.value("local").toString().toLatin1() == hash) continue;
            auto data = bytesAt(cachePath(profile, name));
            const auto localHash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
            if (name.startsWith(".texstudio-remote-preview/") && name.endsWith(".tex"))
                data.replace(profile.cache().toUtf8(), profile.root.toUtf8());
            const auto sentHash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
            QJsonValue expected = previous.contains("remote") ? previous.value("remote") : QJsonValue(QJsonValue::Null);
            const auto result = call(connection, "put", {{"path", name}, {"data", QString::fromLatin1(data.toBase64())}, {"expected", expected}});
            require(result.value("hash").toString().toLatin1() == sentHash, "The remote save acknowledgement was invalid.");
            files.insert(name, QJsonObject{{"remote", QString::fromLatin1(sentHash)}, {"local", QString::fromLatin1(localHash)}});
            save();
        }
        // TeXstudio removes its generated preview files when a preview expires.
        // Synchronize that cleanup only in our reserved transient directory;
        // ordinary project deletions always retain their recovery copies.
        for (const auto &name : files.keys()) {
            if (!name.startsWith(".texstudio-remote-preview/") || QFileInfo::exists(cachePath(profile, name))) continue;
            const auto previous = files.value(name).toObject();
            if (!previous.value("remote").isString()) continue;
            call(connection, "delete", {{"path", name}, {"expected", previous.value("remote")}});
            files.remove(name); save();
        }
    }
    void pull() {
        const auto listing = call(connection, "list").value("files");
        require(listing.isArray(), "Invalid remote project listing.");
        QSet<QString> seen;
        QJsonArray updates;
        const QString folder = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString staging = profile.directory() + "/pulls/" + folder;
        if (profile.ready) require(QDir().mkpath(staging), "Cannot create a download staging folder.");
        for (const auto &value : listing.toArray()) {
            const auto item = value.toObject(); const QString name = relativeName(item.value("path").toString());
            require(!seen.contains(name.toCaseFolded()), "Project filenames collide on a case-insensitive client filesystem."); seen.insert(name.toCaseFolded());
            const QString path = cachePath(profile, name);
            const auto previous = files.value(name).toObject();
            const auto oldLocal = previous.value("local").toString().toLatin1();
            const auto local = QFileInfo(path).isFile() ? fileHash(path) : QByteArray();
            const auto remote = item.value("hash").toString();
            if (previous.value("remote").toString() == remote && !local.isEmpty()) continue;
            // Existing projects are applied by the GUI thread, never by this
            // network worker, so a concurrent editor save cannot be overwritten.
            if (!profile.ready) require(!QFileInfo(path).isSymLink(), "Unsafe initial cache path.");
            const auto result = call(connection, "read", {{"path", name}});
            auto data = QByteArray::fromBase64(result.value("data").toString().toLatin1());
            const auto receivedHash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
            require(receivedHash == result.value("hash").toString().toLatin1(), "Remote file checksum did not match: " + name);
            // Logs are a local view; source and SyncTeX files remain byte-for-byte copies.
            if (name.endsWith(".log")) {
                data.replace(profile.root.toUtf8(), profile.cache().toUtf8());
                QString windowsRoot = profile.root; windowsRoot.replace('/', '\\');
                data.replace(windowsRoot.toUtf8(), profile.cache().toUtf8());
            }
            const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
            const QString destination = profile.ready ? staging + "/" + token : path;
            require(QDir().mkpath(QFileInfo(destination).absolutePath()), "Cannot create the project cache folder.");
            require(!QFileInfo(destination).isSymLink(), "Local cache path changed.");
            QSaveFile output(destination);
            require(output.open(QIODevice::WriteOnly) && output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
                    && output.write(data) == data.size() && output.commit(), "Cannot store project file: " + name);
            const QString localHash = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
            if (profile.ready) updates.append(QJsonObject{{"path", name}, {"stage", token},
                    {"expected", QString::fromLatin1(oldLocal)}, {"remote", QString::fromLatin1(receivedHash)}, {"local", localHash}});
            else files.insert(name, QJsonObject{{"remote", QString::fromLatin1(receivedHash)}, {"local", localHash}});
        }
        // Never silently delete a cached source if another editor removed it remotely.
        save();
        if (profile.ready) {
            QString error;
            require(writeJson(profile.directory() + "/pull.json", {{"directory", folder}, {"files", updates}}, &error), error);
        }
    }
};
}

int workerMain(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("texstudio");
    QCoreApplication::setApplicationName("TeXstudio");
    const QStringList args = app.arguments();
    try {
        require(args.size() == 5, "Invalid SSH workspace worker arguments.");
        QString error; Profile profile = Profile::load(args.at(2), &error);
        require(profile.isValid(), error);
        const QString op = args.at(3);
        const auto document = QJsonDocument::fromJson(QByteArray::fromBase64(args.at(4).toLatin1()));
        require(document.isObject(), "Invalid SSH workspace request."); const auto request = document.object();
        QLockFile lock(profile.directory() + "/sync.lock");
        lock.setStaleLockTime(0);
        require(lock.tryLock(300000), "Another workspace synchronization is still running. Local edits are preserved.");
        // The GUI must apply a completed download before another worker can
        // publish a new one. Release the lock while it handles that completion.
        QElapsedTimer pending; pending.start();
        while (QFileInfo::exists(profile.directory() + "/pull.json")) {
            lock.unlock();
            require(pending.elapsed() < 30000, "Apply the previous download before starting another transfer. Local files are preserved.");
            QThread::msleep(50);
            require(lock.tryLock(300000), "Another workspace synchronization is still running.");
        }
        require(QDir().mkpath(profile.cache()), "Cannot create the private project cache.");
        QFile::setPermissions(profile.directory(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        QFile::setPermissions(profile.cache(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        Connection connection;
        const bool connected = connection.open(profile);
        require(connected, connection.error());
        require(profile.save(&error), error);
        Mirror mirror(profile, connection);
        if (op == "connect" || op == "refresh") {
            mirror.pull(); profile.ready = true; require(profile.save(&error), error);
        } else if (op == "sync") {
            try { mirror.push(); }
            catch (const Failure &) { if (connection.error().startsWith("Conflict:")) mirror.pull(); throw; }
        } else if (op == "resolve") {
            const QString name = relativeName(request.value("path").toString());
            QFile conflicts(profile.directory() + "/conflicts.json");
            require(conflicts.open(QIODevice::ReadOnly), "No downloaded conflict revision is available. Refresh the remote project first.");
            const auto entries = QJsonDocument::fromJson(conflicts.readAll()).object().value("files").toArray(); conflicts.close();
            const QString expected = request.value("expected").toString();
            bool found = false;
            for (const auto &value : entries) {
                const auto entry = value.toObject();
                found |= entry.value("path").toString() == name && entry.value("remote_hash").toString() == expected;
            }
            require(found && expected.size() == 64, "The compared remote revision is no longer available. Refresh and compare again.");
            const auto data = bytesAt(cachePath(profile, name));
            const auto hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
            const auto result = call(connection, "put", {{"path", name}, {"data", QString::fromLatin1(data.toBase64())}, {"expected", expected}});
            require(result.value("hash").toString().toLatin1() == hash, "Invalid remote save acknowledgement.");
            mirror.files.insert(name, QJsonObject{{"remote", QString::fromLatin1(hash)}, {"local", QString::fromLatin1(hash)}}); mirror.save();
            QJsonArray remaining;
            for (const auto &entry : entries) if (entry.toObject().value("path").toString() != name) remaining.append(entry);
            require(writeJson(profile.directory() + "/conflicts.json", {{"files", remaining}}, &error), error);
        } else if (op == "delete") {
            const QString name = relativeName(request.value("path").toString());
            const auto previous = mirror.files.value(name).toObject();
            require(previous.contains("remote"), "The remote file has no verified revision.");
            const auto local = fileHash(cachePath(profile, name));
            require(local == previous.value("local").toString().toLatin1(), "Save or preserve local edits before deleting this file.");
            call(connection, "delete", {{"path", name}, {"expected", previous.value("remote")}});
            // A save can arrive while the network request is in flight. Keep
            // the cache as a recovery copy instead of deleting a newer edit.
            mirror.files.insert(name, QJsonObject{{"remote", QJsonValue(QJsonValue::Null)}, {"local", previous.value("local")}});
            mirror.save();
        } else if (op == "command") {
            mirror.push();
            QJsonArray argv;
            for (const auto &argument : request.value("argv").toArray()) {
                argv.append(mapCommandArgument(profile, argument.toString()));
            }
            const QString cwd = QDir(profile.cache()).relativeFilePath(request.value("cwd").toString());
            require(cwd == "." || (!cwd.startsWith("../") && !QDir::isAbsolutePath(cwd)), "Remote working directory escaped the project.");
            QByteArray stdoutPending, stderrPending;
            auto output = [&](const QString &channel, const QByteArray &bytes) {
                auto &pending = channel == "stderr" ? stderrPending : stdoutPending;
                pending += bytes;
                int end;
                while ((end = pending.indexOf('\n')) >= 0 || pending.size() > 1024 * 1024) {
                    if (end < 0) end = pending.size() - 4096;
                    auto line = pending.left(end + 1); pending.remove(0, end + 1);
                    line.replace(profile.root.toUtf8(), profile.cache().toUtf8());
                    QString windowsRoot = profile.root; windowsRoot.replace('/', '\\');
                    line.replace(windowsRoot.toUtf8(), profile.cache().toUtf8());
                    FILE *stream = channel == "stderr" ? stderr : stdout;
                    fwrite(line.constData(), 1, size_t(line.size()), stream); fflush(stream);
                }
            };
            QJsonObject texPaths;
            const auto supplied = request.value("tex_paths").toObject();
            for (auto it = supplied.begin(); it != supplied.end(); ++it) {
                QJsonArray paths;
                for (const auto &value : it.value().toArray()) {
                    paths.append(mapCommandArgument(profile, value.toString()));
                }
                texPaths.insert(it.key(), paths);
            }
            const auto result = call(connection, "command", {{"argv", argv}, {"cwd", cwd == "." ? "" : cwd}, {"tex_paths", texPaths}}, output, -1);
            output("stdout", "\n"); output("stderr", "\n");
            mirror.pull();
            return result.value("exit_code").toInt(1);
        } else throw Failure("Unknown SSH workspace operation.");
        fputs("Remote workspace synchronized.\n", stdout);
        return 0;
    } catch (const std::exception &error) {
        fprintf(stderr, "Remote workspace: %s\n", error.what());
        return 1;
    }
}
}
