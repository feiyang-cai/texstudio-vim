#include "remoteclient.h"

namespace RemoteWorkspace {
namespace {
QString clean(QString path) { return QDir::cleanPath(QDir::fromNativeSeparators(path)); }
bool within(const QString &path, const QString &root) {
#ifdef Q_OS_WIN
    const auto sensitivity = Qt::CaseInsensitive;
#else
    const auto sensitivity = Qt::CaseSensitive;
#endif
    return path.startsWith(root + "/", sensitivity) || path.compare(root, sensitivity) == 0;
}
}
bool validRelativePath(const QString &name)
{
    if (name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(QChar(0))) return false;
    const QRegularExpression reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])([.]|$)", QRegularExpression::CaseInsensitiveOption);
    for (const auto &part : name.split('/')) {
        if (part.isEmpty() || part == "." || part == ".." || part.endsWith('.') || part.endsWith(' ')
                || part.contains(QRegularExpression("[<>:\"|?*\\x00-\\x1f]")) || reserved.match(part).hasMatch()) return false;
    }
    return true;
}
bool validateProjectPaths(const QStringList &paths, QString *error)
{
    QMap<QString, QString> spellings;
    QMap<QString, bool> directories;
    for (QString name : paths) {
        const bool isDirectory = name.endsWith('/');
        if (isDirectory) name.chop(1);
        if (!validRelativePath(name)) {
            if (error) *error = "Unsafe project path.";
            return false;
        }
        const auto parts = name.split('/');
        QString prefix;
        for (int i = 0; i < parts.size(); ++i) {
            prefix += (i ? "/" : "") + parts.at(i);
            const QString folded = prefix.normalized(QString::NormalizationForm_C).toCaseFolded();
            const bool directory = isDirectory || i + 1 < parts.size();
            if (spellings.contains(folded) && (spellings.value(folded) != prefix
                    || directories.value(folded) != directory)) {
                if (error) *error = "Project paths collide on a case-insensitive client filesystem.";
                return false;
            }
            spellings.insert(folded, prefix); directories.insert(folded, directory);
        }
    }
    return true;
}
QString cacheBase() { return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/remote-workspaces"; }

bool writeJson(const QString &path, const QJsonObject &object, QString *error)
{
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    const bool ok = file.open(QIODevice::WriteOnly) && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner)
            && file.write(bytes) == bytes.size() && file.commit();
    if (!ok && error) *error = file.errorString();
    return ok;
}
bool applyPull(const Profile &profile, QString *error, const std::function<bool(const QString &)> &canOverwrite)
{
    const QString manifest = profile.directory() + "/pull.json";
    if (!QFileInfo::exists(manifest)) return true;
    QLockFile lock(profile.directory() + "/sync.lock");
    lock.setStaleLockTime(0); // Long builds remain locked; dead owners are still detected by PID.
    auto fail = [&](const QString &reason) { if (error) *error = reason; return false; };
    if (!lock.tryLock(100)) return fail("Another workspace transfer is running; downloaded files are preserved.");
    QFile file(manifest), state(profile.directory() + "/state.json");
    if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) return fail("Cannot read downloaded project changes.");
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return fail("Invalid downloaded project changes.");
    const auto pull = doc.object(); const QString folder = pull.value("directory").toString();
    const QRegularExpression uuid("^[a-f0-9-]{36}$");
    if (!uuid.match(folder).hasMatch() || !pull.value("files").isArray()) return fail("Invalid downloaded project paths.");
    QJsonObject records;
    if (state.open(QIODevice::ReadOnly)) {
        if (state.size() > 64 * 1024 * 1024) return fail("Workspace state is too large.");
        const auto stateDoc = QJsonDocument::fromJson(state.readAll());
        if (!stateDoc.isObject()) return fail("Damaged synchronization state; local files are preserved.");
        records = stateDoc.object();
        state.close();
    }
    QMap<QString, QJsonObject> conflictCopies;
    QFile previousConflicts(profile.directory() + "/conflicts.json");
    if (previousConflicts.open(QIODevice::ReadOnly)) {
        const auto entries = QJsonDocument::fromJson(previousConflicts.readAll()).object().value("files").toArray();
        for (const auto &entry : entries) conflictCopies.insert(entry.toObject().value("path").toString(), entry.toObject());
        previousConflicts.close();
    }
    for (const auto &value : pull.value("files").toArray()) {
        const auto change = value.toObject(); const QString name = change.value("path").toString();
        const QString token = change.value("stage").toString();
        if (!validRelativePath(name)) return fail("Unsafe downloaded filename.");
        if (change.value("deleted").toBool()) {
            if (!change.value("remote").isNull()) return fail("Invalid remote deletion revision.");
            conflictCopies.insert(name, QJsonObject{{"path", name}, {"deleted", true},
                    {"remote_hash", QJsonValue(QJsonValue::Null)}});
            continue;
        }
        if (!uuid.match(token).hasMatch()) return fail("Unsafe downloaded filename.");
        QString target = profile.cache();
        for (const auto &part : name.split('/')) {
            target += "/" + part;
            if (QFileInfo(target).isSymLink()) return fail("A cache path changed into a symbolic link.");
        }
        const QString source = profile.directory() + "/pulls/" + folder + "/" + token;
        if (QFileInfo(source).isSymLink() || fileHash(source) != change.value("local").toString().toLatin1()) return fail("A downloaded file failed checksum verification.");
        const bool exists = QFileInfo::exists(target);
        const auto expected = change.value("expected").toString().toLatin1();
        const auto record = records.value(name).toObject();
        if (exists && record.value("remote") == change.value("remote")
                && fileHash(target) == change.value("local").toString().toLatin1()) {
            conflictCopies.remove(name);
            continue;
        }
        if ((canOverwrite && !canOverwrite(target)) || (exists && (expected.isEmpty() || fileHash(target) != expected)) || (!exists && !expected.isEmpty())) {
            conflictCopies.insert(name, QJsonObject{{"path", name}, {"remote_copy", source}, {"remote_hash", change.value("remote")}});
            continue;
        }
        QFile input(source); QSaveFile output(target);
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !input.open(QIODevice::ReadOnly)
                || !output.open(QIODevice::WriteOnly) || !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner)) return fail("Cannot apply downloaded project file: " + name);
        while (!input.atEnd()) {
            const auto bytes = input.read(1024 * 1024);
            if (bytes.isEmpty() || output.write(bytes) != bytes.size()) return fail("Cannot write downloaded project file: " + name);
        }
        if (!output.commit()) return fail("Cannot commit downloaded project file: " + name);
        records.insert(name, QJsonObject{{"remote", change.value("remote")}, {"local", change.value("local")}});
        conflictCopies.remove(name);
        // Persist each applied revision so an interrupted multi-file download
        // does not mistake our own earlier writes for a concurrent local edit.
        if (!writeJson(profile.directory() + "/state.json", records, error)) return false;
    }
    if (!writeJson(profile.directory() + "/state.json", records, error)) return false;
    QJsonArray entries;
    for (const auto &entry : conflictCopies) entries.append(entry);
    if (!writeJson(profile.directory() + "/conflicts.json", {{"files", entries}}, error)) return false;
    file.close(); QFile::remove(manifest);
    if (conflictCopies.isEmpty()) {
        QDir(profile.directory() + "/pulls/" + folder).removeRecursively();
        return true;
    }
    return fail("Local edits were preserved for conflicting files: " + conflictCopies.keys().join(", ")
                + ". Downloaded remote copies are retained in " + profile.directory() + "/pulls/" + folder);
}
bool Profile::isValid() const
{
    static const QRegularExpression hostPattern("^[A-Za-z0-9][A-Za-z0-9_.@-]*$");
    return hostPattern.match(host).hasMatch() && !root.isEmpty() && !root.contains(QChar(0))
            && (python == "python3" || python == "python" || python == "py -3")
            && (sshConfig.isEmpty() || QFileInfo(sshConfig).isFile())
            && !manifest.isEmpty() && QFileInfo(manifest).isAbsolute();
}
bool Profile::save(QString *error) const
{
    if (!isValid()) { if (error) *error = "Invalid SSH workspace profile."; return false; }
    return writeJson(manifest, {{"version", 1}, {"host", host}, {"root", root}, {"python", python},
                               {"ssh_config", sshConfig}, {"platform", platform}, {"ready", ready}}, error);
}
Profile Profile::load(const QString &manifest, QString *error)
{
    Profile result;
    QFile file(manifest);
    if (QFileInfo(manifest).isSymLink() || !file.open(QIODevice::ReadOnly) || file.size() > 32768) {
        if (error) *error = "Cannot read the private SSH workspace profile.";
        return result;
    }
    const auto obj = QJsonDocument::fromJson(file.readAll()).object();
    result.manifest = QFileInfo(manifest).absoluteFilePath();
    result.host = obj.value("host").toString(); result.root = obj.value("root").toString();
    result.python = obj.value("python").toString(); result.sshConfig = obj.value("ssh_config").toString();
    result.platform = obj.value("platform").toString(); result.ready = obj.value("ready").toBool();
    if (obj.value("version").toInt() != 1 || !result.isValid()) {
        if (error) *error = "Invalid SSH workspace profile.";
        return {};
    }
    return result;
}
Profile forFile(const QString &filename)
{
    const QString path = clean(QFileInfo(filename).absoluteFilePath()), base = clean(cacheBase());
    if (!within(path, base)) return {};
    const QString relative = QDir(base).relativeFilePath(path);
    const QString id = relative.section('/', 0, 0);
    if (!QRegularExpression("^[a-f0-9-]{36}$").match(id).hasMatch()) return {};
    const auto profile = Profile::load(base + "/" + id + "/profile.json");
    return profile.ready && within(path, clean(profile.cache())) ? profile : Profile();
}
QString toRemote(const Profile &profile, const QString &local)
{
    const QString path = clean(QFileInfo(local).absoluteFilePath()), root = clean(profile.cache());
    if (!within(path, root)) return local;
    const QString relative = QDir(root).relativeFilePath(path);
    return profile.root + (relative == "." ? "" : "/" + relative);
}
QString toLocal(const Profile &profile, const QString &remote)
{
    QString path = remote; path.replace('\\', '/');
    QString root = profile.root; root.replace('\\', '/');
    const auto cs = profile.platform == "win32" ? Qt::CaseInsensitive : Qt::CaseSensitive;
    if (path.compare(root, cs) == 0) return profile.cache();
    if (!path.startsWith(root + "/", cs)) return remote;
    const QString relative = path.mid(root.size() + 1);
    if (relative.split('/').contains("..")) return remote;
    return profile.cache() + "/" + relative;
}
QString mapSourcePath(const QString &context, const QString &remote)
{
    const auto profile = forFile(context);
    return profile.isValid() ? toLocal(profile, remote) : remote;
}
QString previewDirectory(const QString &master)
{
    const auto profile = forFile(master);
    if (!profile.isValid()) return QDir::tempPath();
    const QString path = profile.cache() + "/.texstudio-remote-preview";
    return QDir().mkpath(path) ? path : QString();
}
QString mapCommandArgument(const Profile &profile, const QString &argument)
{
    QString mapped = argument;
    const auto replaceRoot = [&](const QString &root) {
        // BuildManager has already parsed quoting. A root may follow a tool
        // option such as -output-directory=, but never match project-other.
        const QRegularExpression pattern(QRegularExpression::escape(root) + "(?=[/\\\\]|$)");
        auto matches = pattern.globalMatch(mapped);
        QList<QPair<int,int>> positions;
        while (matches.hasNext()) {
            const auto match = matches.next(); positions.prepend({match.capturedStart(), match.capturedLength()});
        }
        for (const auto &position : positions) {
            const int start = position.first + position.second;
            int end = start;
            while (end < mapped.size() && !QString("\"'{}").contains(mapped.at(end))) ++end;
            QString suffix = mapped.mid(start, end - start);
            suffix.replace('\\', '/');
            mapped.replace(start, end - start, suffix);
            mapped.replace(position.first, position.second, profile.root);
        }
    };
    replaceRoot(profile.cache());
    QString windowsCache = profile.cache(); windowsCache.replace('/', '\\');
    replaceRoot(windowsCache);
    return mapped;
}
QString workerProgram()
{
#ifdef Q_OS_LINUX
    // Standalone AppImages invoke their bundled ELF loader directly. Qt then
    // reports that loader as applicationFilePath(), which cannot accept our
    // worker options. Re-enter the bundle through its normal AppRun launcher.
    const QFileInfo directory(qEnvironmentVariable("APPDIR"));
    const QString root = directory.canonicalFilePath();
    const QFileInfo executable(QCoreApplication::arguments().value(0));
    const QString binary = executable.canonicalFilePath();
    const QString running = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QFileInfo launcher(root + "/AppRun");
    if (directory.isAbsolute() && !root.isEmpty()
            && (binary.startsWith(root + "/") || running.startsWith(root + "/"))
            && launcher.isFile() && launcher.isExecutable()) return launcher.absoluteFilePath();
#endif
    return QCoreApplication::applicationFilePath();
}
QStringList workerArguments(const Profile &profile, const QString &operation, const QJsonObject &data)
{
    return {"--remote-workspace-worker", profile.manifest, operation,
            QString::fromLatin1(QJsonDocument(data).toJson(QJsonDocument::Compact).toBase64())};
}
QByteArray fileHash(const QString &path)
{
    QFile file(path); QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) return {};
    return hash.result().toHex();
}
bool Connection::open(Profile &profile)
{
    if (!profile.isValid()) { m_error = "Invalid SSH profile."; return false; }
    const QString ssh = QStandardPaths::findExecutable("ssh");
    if (ssh.isEmpty()) { m_error = "OpenSSH is not installed or is not on PATH."; return false; }
    QFile helper(":/remote-workspace/agent.py");
    if (!helper.open(QIODevice::ReadOnly)) { m_error = "The SSH workspace helper is missing."; return false; }
    const QByteArray code = helper.readAll();
    const QString bootstrap = profile.python + " -u -c \"import sys;exec(sys.stdin.buffer.read(int(sys.stdin.buffer.readline())))\"";
    QStringList args{"-T", "-o", "BatchMode=yes", "-o", "RemoteCommand=none", "-o", "StrictHostKeyChecking=yes", "-o", "ConnectTimeout=15",
                     "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=2"};
    if (!profile.sshConfig.isEmpty()) args << "-F" << profile.sshConfig;
    args << profile.host << bootstrap;
    // SSH uses the operating system's libraries, including in an AppImage.
    auto environment = QProcessEnvironment::systemEnvironment();
#ifdef Q_OS_LINUX
    if (environment.contains("APPIMAGE") || environment.contains("APPDIR")) environment.remove("LD_LIBRARY_PATH");
#endif
    m_process.setProcessEnvironment(environment);
    m_process.start(ssh, args);
    if (!m_process.waitForStarted(15000)) { m_error = m_process.errorString(); return false; }
    const QByteArray boot = QByteArray::number(code.size()) + '\n' + code;
    if (m_process.write(boot) != boot.size()) { m_error = "Could not send the SSH helper."; return false; }
    const auto hello = request("hello", {{"root", profile.root}}, {}, 30000);
    if (!m_error.isEmpty()) return false;
    if (hello.value("version").toInt() != 1) { m_error = "Unsupported SSH helper version."; return false; }
    profile.root = hello.value("root").toString(); profile.platform = hello.value("platform").toString();
    return true;
}
QJsonObject Connection::request(const QString &operation, const QJsonObject &data, const Output &output, int timeout)
{
    m_error.clear();
    QJsonObject request = data; const int id = ++m_id;
    request.insert("op", operation); request.insert("id", id);
    const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    if (m_process.state() != QProcess::Running || m_process.write(bytes) != bytes.size()) {
        m_error = "The SSH connection is not running."; return {};
    }
    QElapsedTimer timer; timer.start();
    while (timeout < 0 || timer.elapsed() < timeout) {
        m_buffer += m_process.readAllStandardOutput();
        if (m_buffer.size() > 96 * 1024 * 1024) { m_error = "SSH response exceeds the transfer limit."; break; }
        int newline;
        while ((newline = m_buffer.indexOf('\n')) >= 0) {
            const auto line = m_buffer.left(newline); m_buffer.remove(0, newline + 1);
            const auto doc = QJsonDocument::fromJson(line);
            if (!doc.isObject() || doc.object().value("protocol").toInt() != 1) {
                m_error = "Unexpected SSH output. The remote login must allow a clean command session."; return {};
            }
            const auto object = doc.object();
            if (object.contains("event")) {
                if (output) output(object.value("event").toString(), QByteArray::fromBase64(object.value("data").toString().toLatin1()));
                continue;
            }
            if (object.value("id").toInt() != id) { m_error = "SSH response did not match the request."; return {}; }
            if (!object.value("ok").toBool()) { m_error = object.value("error").toString(); return {}; }
            return object.value("result").toObject();
        }
        if (m_process.state() == QProcess::NotRunning) {
            m_error = "SSH connection closed: " + QString::fromUtf8(m_process.readAllStandardError()).left(16384);
            return {};
        }
        m_process.waitForReadyRead(50);
    }
    if (m_error.isEmpty()) m_error = "SSH request timed out. Local files are preserved.";
    m_process.kill(); m_process.waitForFinished(5000);
    return {};
}
Connection::~Connection()
{
    m_process.closeWriteChannel();
    if (!m_process.waitForFinished(1500)) { m_process.kill(); m_process.waitForFinished(3000); }
}
}
