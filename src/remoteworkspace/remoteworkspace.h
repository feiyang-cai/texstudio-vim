#ifndef TXS_REMOTE_WORKSPACE_UI_H
#define TXS_REMOTE_WORKSPACE_UI_H
#include "remoteclient.h"
#include <QtWidgets>

namespace RemoteWorkspace {
// The editor and Vim engine operate normally on the private project cache.
// This optional controller owns transport, synchronization status and project UI.
class Controller : public QObject {
    Q_OBJECT
public:
    explicit Controller(QMainWindow *window, QMenu *fileMenu);
    static Controller *instance();
    static void saved(const QString &filename);
    static void opened(const QString &filename);
    static bool confirmClose();
    static void commandFinished(const QString &file);
    ~Controller() override;
    static bool prepareCommand(const QString &file, const QString &command, const QStringList &environment,
                               QStringList *arguments);
    void showConnectionDialog();
    void addMenu(QMenu *menu);
signals:
    void openFile(const QString &file);
    void status(const QString &message);
private:
    struct Queue { Profile profile; QProcess *process = nullptr; QStringList jobs; QSet<QString> pending; QHash<QString,QJsonObject> data; int commands = 0; QString error; };
    QHash<QString, Queue *> m_queues;
    QMainWindow *m_window;
    QDockWidget *m_dock;
    QTreeView *m_tree;
    QFileSystemModel *m_model;
    QLabel *m_status;
    Profile m_current;
    void activate(const Profile &profile);
    void enqueue(const Profile &profile, const QString &operation, const QJsonObject &data = {});
    void next(const QString &manifest);
    void refresh();
    void newFile();
    void deleteFile();
    bool apply(const Profile &profile, QString *error);
    void openConflict();
    void resolveConflict();
    static Controller *s_instance;
};
}
#endif
