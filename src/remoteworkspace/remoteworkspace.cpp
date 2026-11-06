#include "remoteworkspace.h"
#include "execprogram.h"
#include "qeditor.h"
#include "qdocument.h"

namespace RemoteWorkspace {
Controller *Controller::s_instance = nullptr;
Controller *Controller::instance() { return s_instance; }
Controller::Controller(QMainWindow *window, QMenu *fileMenu) : QObject(window), m_window(window)
{
    s_instance = this;
    connect(this, &QObject::destroyed, [] { s_instance = nullptr; });
    addMenu(fileMenu);
    m_dock = new QDockWidget(tr("Remote Workspace"), window);
    m_dock->setObjectName("remoteWorkspaceDock");
    auto panel = new QWidget(m_dock); auto layout = new QVBoxLayout(panel);
    m_status = new QLabel(tr("No remote project connected."), panel); m_status->setWordWrap(true);
    m_model = new QFileSystemModel(this); m_model->setReadOnly(true);
    m_tree = new QTreeView(panel); m_tree->setModel(m_model); m_tree->setHeaderHidden(true);
    for (int column = 1; column < m_model->columnCount(); ++column) m_tree->hideColumn(column);
    layout->addWidget(m_status); layout->addWidget(m_tree); m_dock->setWidget(panel);
    window->addDockWidget(Qt::LeftDockWidgetArea, m_dock); m_dock->hide();
    connect(m_tree, &QTreeView::doubleClicked, this, [this](const QModelIndex &index) {
        const auto file = m_model->fileInfo(index);
        if (file.isFile()) emit openFile(file.absoluteFilePath());
    });
}
void Controller::addMenu(QMenu *fileMenu)
{
    auto menu = fileMenu->addMenu(tr("Remote Workspace"));
    menu->addAction(tr("Connect over SSH…"), this, &Controller::showConnectionDialog);
    menu->addAction(tr("Refresh remote project"), this, &Controller::refresh);
    menu->addAction(tr("Synchronize saved changes"), this, [this] { if (m_current.isValid()) enqueue(m_current, "sync"); });
    menu->addAction(tr("Open a downloaded conflict copy…"), this, &Controller::openConflict);
    menu->addAction(tr("Upload a merged local version…"), this, &Controller::resolveConflict);
    menu->addAction(tr("New remote file…"), this, &Controller::newFile);
    menu->addAction(tr("Delete remote file…"), this, &Controller::deleteFile);
    menu->addAction(tr("Hide remote project panel"), this, [this] { m_dock->hide(); });
}
void Controller::activate(const Profile &profile)
{
    m_current = profile;
    m_model->setRootPath(profile.cache());
    m_tree->setRootIndex(m_model->index(profile.cache()));
    m_dock->setWindowTitle(tr("Remote: %1").arg(profile.host));
    m_status->setText(tr("%1:%2\nFiles are cached locally; saves synchronize over SSH.").arg(profile.host, profile.root));
    m_dock->show();
}
void Controller::showConnectionDialog()
{
    QDialog dialog(m_window); dialog.setWindowTitle(tr("Open SSH Workspace"));
    auto layout = new QFormLayout(&dialog);
    auto existing = new QComboBox(&dialog); existing->addItem(tr("New connection"), QString());
    QDir directory(cacheBase());
    for (const auto &name : directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto profile = Profile::load(directory.filePath(name + "/profile.json"));
        if (profile.isValid()) existing->addItem(profile.host + ":" + profile.root, profile.manifest);
    }
    auto host = new QLineEdit(&dialog); host->setPlaceholderText(tr("SSH config alias or user@host"));
    auto root = new QLineEdit(&dialog); root->setPlaceholderText(tr("Absolute folder on the remote machine"));
    auto python = new QComboBox(&dialog); python->addItems({"python3", "python", "py -3"});
    auto config = new QLineEdit(&dialog); config->setPlaceholderText(tr("Optional OpenSSH config file"));
    auto note = new QLabel(tr("Uses OpenSSH and your configured keys or SSH agent. First connect with ssh in a terminal to verify the host key. The remote computer needs Python 3 and a TeX installation. Passwords and private keys are not stored here."), &dialog);
    note->setWordWrap(true); note->setMinimumWidth(430);
    layout->addRow(tr("Saved connection"), existing); layout->addRow(tr("SSH host"), host);
    layout->addRow(tr("Project folder"), root); layout->addRow(tr("Remote Python 3 command"), python);
    layout->addRow(tr("SSH config file"), config); layout->addRow(note);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Open | QDialogButtonBox::Cancel, &dialog);
    layout->addRow(buttons);
    connect(existing, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [=](int) {
        const auto profile = Profile::load(existing->currentData().toString());
        if (!profile.isValid()) return;
        host->setText(profile.host); root->setText(profile.root); python->setCurrentText(profile.python); config->setText(profile.sshConfig);
    });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    Profile profile = Profile::load(existing->currentData().toString());
    if (!profile.isValid() || profile.host != host->text().trimmed() || profile.root != root->text().trimmed()) {
        profile = Profile();
        const QString folder = cacheBase() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!QDir().mkpath(folder)) { QMessageBox::warning(m_window, tr("SSH workspace"), tr("Cannot create a private cache folder.")); return; }
        QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        profile.manifest = folder + "/profile.json";
    }
    profile.host = host->text().trimmed(); profile.root = root->text().trimmed();
    profile.python = python->currentText(); profile.sshConfig = config->text().trimmed();
    QString error;
    if (!profile.save(&error)) { QMessageBox::warning(m_window, tr("SSH workspace"), error); return; }
    m_current = profile; enqueue(profile, "connect");
}
void Controller::enqueue(const Profile &profile, const QString &operation, const QJsonObject &data)
{
    auto queue = m_queues.value(profile.manifest, nullptr);
    if (!queue) { queue = new Queue; queue->profile = profile; m_queues.insert(profile.manifest, queue); }
    const QString key = (operation == "delete" || operation == "resolve") ? operation + ":" + data.value("path").toString() : operation;
    if (!queue->jobs.contains(key)) queue->jobs << key;
    queue->data.insert(key, data);
    queue->pending.insert(key); queue->error.clear();
    next(profile.manifest);
}
void Controller::next(const QString &manifest)
{
    auto queue = m_queues.value(manifest);
    if (queue->process || queue->commands || queue->jobs.isEmpty()) return;
    const QString key = queue->jobs.takeFirst();
    const QString operation = key.section(':', 0, 0);
    const auto request = queue->data.take(key);
    auto process = new QProcess(this); queue->process = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    emit status(tr("SSH %1: %2…").arg(queue->profile.host, operation));
    m_status->setText(tr("Synchronizing %1… You can keep editing.").arg(queue->profile.host));
    connect(process, qOverload<int,QProcess::ExitStatus>(&QProcess::finished), this,
            [this, manifest, operation, key, process](int code, QProcess::ExitStatus exit) {
        auto queue = m_queues.value(manifest); queue->process = nullptr;
        const QString output = QString::fromUtf8(process->readAll()).right(16384); process->deleteLater();
        if (exit != QProcess::NormalExit || code) {
            QString ignored; apply(Profile::load(manifest), &ignored);
            queue->error = output.isEmpty() ? tr("SSH workspace synchronization failed.") : output;
            const bool resolving = !queue->jobs.isEmpty() && queue->jobs.first().startsWith("resolve:");
            if (!resolving) queue->jobs.clear();
            m_status->setText(tr("Remote synchronization failed. Local files are preserved.\n%1").arg(queue->error));
            emit status(m_status->text());
            if (operation == "connect" && queue->profile.ready && QDir(queue->profile.cache()).exists()) {
                if (QMessageBox::question(m_window, tr("SSH workspace"), queue->error + "\n\n" + tr("Open the cached project without refreshing?")) == QMessageBox::Yes)
                    activate(Profile::load(manifest));
            }
            if (resolving) next(manifest);
            if (operation == "connect" && !queue->profile.ready) {
                queue->pending.remove(operation);
                QMessageBox::warning(m_window, tr("SSH workspace"), queue->error);
            }
            return;
        }
        QString transferError;
        if (!apply(Profile::load(manifest), &transferError)) {
            queue->error = transferError; queue->pending.insert("conflict");
            m_status->setText(transferError); emit status(transferError);
            return;
        }
        // A save that arrived while a sync was running queued a second sync.
        if (!queue->jobs.contains(key)) queue->pending.remove(key);
        queue->profile = Profile::load(manifest);
        if (operation == "resolve") {
            QFile conflicts(queue->profile.directory() + "/conflicts.json");
            if (conflicts.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(conflicts.readAll()).object().value("files").toArray().isEmpty()) queue->pending.remove("conflict");
            if (!queue->jobs.contains("sync")) queue->jobs << "sync";
            queue->pending.insert("sync");
        }
        if (operation == "connect") activate(queue->profile);
        m_status->setText(operation == "delete" ? tr("Remote file deleted. The cached copy is retained for recovery.")
                : tr("%1:%2\nSaved changes synchronized.").arg(queue->profile.host, queue->profile.root));
        emit status(tr("Remote changes saved on %1.").arg(queue->profile.host));
        next(manifest);
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, manifest](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        auto queue = m_queues.value(manifest); queue->process = nullptr;
        queue->error = process->errorString(); queue->jobs.clear();
        m_status->setText(queue->error); emit status(queue->error); process->deleteLater();
    });
    process->start(QCoreApplication::applicationFilePath(), workerArguments(queue->profile, operation, request));
}
void Controller::saved(const QString &filename)
{
    if (!s_instance) return;
    const auto profile = forFile(filename);
    if (profile.isValid()) s_instance->enqueue(profile, "sync");
}
bool Controller::confirmClose()
{
    if (!s_instance) return true;
    bool pending = false;
    for (auto queue : s_instance->m_queues) pending |= !queue->pending.isEmpty() || queue->process || queue->commands;
    if (!pending) return true;
    QMessageBox box(QMessageBox::Warning, tr("Pending remote saves"),
                    tr("Some remote changes have not been confirmed. Keep the local cached copies and close, or cancel closing to finish synchronization. Cached files are retained for recovery."),
                    QMessageBox::Cancel, s_instance->m_window);
    auto keep = box.addButton(tr("Keep cached changes and close"), QMessageBox::AcceptRole);
    box.exec();
    if (box.clickedButton() != keep) return false;
    for (auto queue : s_instance->m_queues) if (queue->process) { queue->process->kill(); queue->process->waitForFinished(3000); }
    return true;
}
bool Controller::prepareCommand(const QString &file, const QString &command, const QStringList &environment, QStringList *arguments)
{
    const auto profile = forFile(file);
    if (!profile.isValid()) return false;
    const ExecProgram parsed(command, QString());
    QStringList argv{parsed.m_program}; argv << parsed.m_arguments;
    QJsonObject paths;
    const QStringList names{"TEXINPUTS", "BIBINPUTS", "BSTINPUTS", "MFINPUTS", "MPINPUTS", "TFMFONTS"};
    for (const auto &item : environment) {
        const QString name = item.section('=', 0, 0);
        if (!names.contains(name)) continue;
        QJsonArray values;
        for (const auto &path : item.mid(name.size() + 1).split(QDir::listSeparator())) {
            if (path.isEmpty() || path == "." || path == profile.cache() || path.startsWith(profile.cache() + "/")) values.append(path);
        }
        paths.insert(name, values);
    }
    if (s_instance) {
        auto queue = s_instance->m_queues.value(profile.manifest, nullptr);
        if (!queue) { queue = new Queue; queue->profile = profile; s_instance->m_queues.insert(profile.manifest, queue); }
        ++queue->commands;
    }
    *arguments = workerArguments(profile, "command", {{"argv", QJsonArray::fromStringList(argv)},
                                    {"cwd", QFileInfo(file).absolutePath()}, {"tex_paths", paths}});
    return true;
}
void Controller::refresh() { if (m_current.isValid()) enqueue(m_current, "refresh"); }
void Controller::newFile()
{
    if (!m_current.ready) return;
    const QString name = QInputDialog::getText(m_window, tr("New remote file"), tr("Project-relative filename"));
    if (name.isEmpty()) return;
    if (!validRelativePath(name)) {
        QMessageBox::warning(m_window, tr("New remote file"), tr("Choose a filename inside the remote project.")); return;
    }
    const QString path = m_current.cache() + "/" + name;
    if (QFileInfo(path).exists()) { emit openFile(path); return; }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !file.commit()) { QMessageBox::warning(m_window, tr("New remote file"), file.errorString()); return; }
    saved(path); emit openFile(path);
}
void Controller::deleteFile()
{
    if (!m_current.ready) return;
    const auto file = m_model->fileInfo(m_tree->currentIndex());
    if (!file.isFile()) return;
    const QString name = QDir(m_current.cache()).relativeFilePath(file.absoluteFilePath());
    if (QMessageBox::question(m_window, tr("Delete remote file"),
            tr("Delete %1 from %2? The local cached copy is retained for recovery.").arg(name, m_current.host),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        enqueue(m_current, "delete", {{"path", name}});
}

void Controller::commandFinished(const QString &file)
{
    if (!s_instance) return;
    const auto profile = forFile(file);
    if (!profile.isValid()) return;
    auto queue = s_instance->m_queues.value(profile.manifest, nullptr);
    QString error;
    if (!s_instance->apply(profile, &error)) {
        if (queue) { queue->error = error; queue->pending.insert("conflict"); }
        s_instance->m_status->setText(error); emit s_instance->status(error);
    }
    if (queue) queue->commands = qMax(0, queue->commands - 1);
    if (queue) s_instance->next(profile.manifest);
}
bool Controller::apply(const Profile &profile, QString *error)
{
    return applyPull(profile, error, [this](const QString &filename) {
        for (auto editor : m_window->findChildren<QEditor *>()) {
            if (QFileInfo(editor->fileName()).absoluteFilePath() == filename && !editor->document()->isClean()) return false;
        }
        return true;
    });
}
void Controller::opened(const QString &filename)
{
    if (!s_instance) return;
    const auto profile = forFile(filename);
    if (profile.isValid() && s_instance->m_current.manifest != profile.manifest) s_instance->activate(profile);
}
void Controller::openConflict()
{
    if (!m_current.isValid()) return;
    QFile file(m_current.directory() + "/conflicts.json");
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto entries = QJsonDocument::fromJson(file.readAll()).object().value("files").toArray();
    QStringList names;
    for (const auto &entry : entries) names << entry.toObject().value("path").toString();
    if (names.isEmpty()) return;
    bool ok;
    const QString selected = QInputDialog::getItem(m_window, tr("Remote conflict copies"),
            tr("Local edits were preserved. Open a downloaded remote version to compare:"), names, 0, false, &ok);
    if (!ok) return;
    const auto entry = entries.at(names.indexOf(selected)).toObject();
    const QString source = entry.value("remote_copy").toString();
    if (!validRelativePath(selected) || !source.startsWith(m_current.directory() + "/pulls/")) return;
    const QString destination = QFileInfo(source).absolutePath() + "/compare/" + selected;
    QDir().mkpath(QFileInfo(destination).absolutePath());
    if (!QFileInfo::exists(destination) && !QFile::copy(source, destination)) return;
    QFile::setPermissions(destination, QFile::ReadOwner);
    emit openFile(destination);
}
void Controller::resolveConflict()
{
    if (!m_current.isValid()) return;
    QFile file(m_current.directory() + "/conflicts.json");
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto entries = QJsonDocument::fromJson(file.readAll()).object().value("files").toArray();
    QStringList names;
    for (const auto &entry : entries) names << entry.toObject().value("path").toString();
    if (names.isEmpty()) return;
    bool ok;
    const QString selected = QInputDialog::getItem(m_window, tr("Upload merged local version"),
        tr("Select the file whose remote version you compared and merged:"), names, 0, false, &ok);
    if (!ok || !validRelativePath(selected)) return;
    const QString local = m_current.cache() + "/" + selected;
    for (auto editor : m_window->findChildren<QEditor *>()) {
        if (QFileInfo(editor->fileName()).absoluteFilePath() == local && !editor->document()->isClean()) {
            QMessageBox::information(m_window, tr("Save local changes first"), tr("Save the local document before uploading the merged version."));
            emit openFile(local); return;
        }
    }
    if (QMessageBox::question(m_window, tr("Upload merged local version"),
            tr("Upload your saved local version of %1 to %2, replacing the remote revision you compared? A newer remote change will block the upload.").arg(selected, m_current.host),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const auto entry = entries.at(names.indexOf(selected)).toObject();
    enqueue(m_current, "resolve", {{"path", selected}, {"expected", entry.value("remote_hash")}});
}
Controller::~Controller()
{
    for (auto queue : m_queues) {
        if (queue->process) {
            queue->process->disconnect(this); queue->process->kill(); queue->process->waitForFinished(3000);
        }
        delete queue;
    }
}
}
