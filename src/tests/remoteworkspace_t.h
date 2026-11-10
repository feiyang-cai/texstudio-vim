#ifndef REMOTE_WORKSPACE_TEST_H
#define REMOTE_WORKSPACE_TEST_H
#ifndef QT_NO_DEBUG
#include <QtTest>
#include "remoteworkspace/remoteclient.h"
class BuildManager;
class LatexEditorView;
class RemoteWorkspaceGuiTest : public QObject {
    Q_OBJECT
public:
    explicit RemoteWorkspaceGuiTest(BuildManager *manager) : m_manager(manager) {}
private slots:
    void localProjectsKeepTheirNormalExecution();
    void sshEditCompilePreviewAndSyncTeX();
    void cleanup();
private:
    BuildManager *m_manager;
    LatexEditorView *m_view = nullptr;
    RemoteWorkspace::Profile m_profile;
};
#endif
#endif
