#ifndef QT_NO_DEBUG
#include "remoteworkspace_t.h"
#include "remoteworkspace/remoteworkspace.h"
#include "buildmanager.h"
#include "configmanager.h"
#include "latexdocument.h"
#include "latexeditorview.h"
#include "latexeditorview_config.h"
#include "qdocumentcursor.h"
#include "qeditor.h"
#include "texstudio.h"
#ifndef NO_POPPLER_PREVIEW
#include "pdfviewer/PDFDocument.h"
#include "pdfviewer/qsynctex.h"
#endif
extern Texstudio *txsInstance;
using namespace RemoteWorkspace;

void RemoteWorkspaceGuiTest::localProjectsKeepTheirNormalExecution()
{
    QTemporaryDir local;
    const QString file = local.path() + "/main.tex";
    QStringList arguments;
    QVERIFY(!Controller::prepareCommand(file, "pdflatex main.tex", {}, &arguments));
    QCOMPARE(previewDirectory(file), QDir::tempPath());
    QVERIFY(!forFile(file).isValid());
}

void RemoteWorkspaceGuiTest::sshEditCompilePreviewAndSyncTeX()
{
    const QString root = qEnvironmentVariable("TXS_TEST_REMOTE_ROOT");
    QVERIFY2(!root.isEmpty(), "Run through tests/remote/run_gui_tests.py with its disposable SSH server.");
    const QString directory = cacheBase() + "/" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(QDir().mkpath(directory));
    m_profile.manifest = directory + "/profile.json";
    m_profile.host = "office-test"; m_profile.root = root;
    m_profile.python = qEnvironmentVariable("TXS_TEST_REMOTE_PYTHON", "python3");
    m_profile.sshConfig = qEnvironmentVariable("TXS_TEST_SSH_CONFIG");
    QString error; QVERIFY2(m_profile.save(&error), qPrintable(error));
    QProcess connect;
    connect.start(QCoreApplication::applicationFilePath(), workerArguments(m_profile, "connect"));
    QVERIFY(connect.waitForFinished(60000));
    QCOMPARE(connect.exitCode(), 0);
    m_profile = Profile::load(m_profile.manifest);
    const QString source = m_profile.cache() + "/main.tex";
    m_view = txsInstance->load(source);
    QVERIFY(m_view);
    m_view->getConfig()->editingMode = LatexEditorViewConfig::VimEditing;
    m_view->updateSettings();
    txsInstance->show();
    QVERIFY(QTest::qWaitForWindowExposed(txsInstance));
    m_view->editor->setFocus();
    QTest::keyClicks(m_view->editor, "ggO");
    QCOMPARE(m_view->editor->inputModeLabel(), QString("INSERT"));
    QTest::keyClicks(m_view->editor, "% Edited over SSH using Vim");
    QTest::keyClick(m_view->editor, Qt::Key_Escape);
    QCOMPARE(m_view->editor->inputModeLabel(), QString("NORMAL"));
    m_view->editor->save();
    QVERIFY(m_view->document->isClean());
    const auto saved = fileHash(source);
    // This fixture is a separate SSH process on the same CI machine; inspecting
    // its folder proves that the native editor's save observer uploaded it.
    QTRY_COMPARE_WITH_TIMEOUT(fileHash(root + "/main.tex"), saved, 60000);

    QString output;
    ProcessX compiler(m_manager, "pdflatex -interaction=nonstopmode -halt-on-error -synctex=1 \"" + source + "\"", source);
    compiler.setStdoutBuffer(&output); compiler.setStderrBuffer(&output);
    QSignalSpy finished(&compiler, &ProcessX::processFinished);
    compiler.startCommand();
    QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, 120000);
    QVERIFY2(compiler.exitStatus() == QProcess::NormalExit && compiler.exitCode() == 0, qPrintable(output));
    const QString pdf = m_profile.cache() + "/main.pdf";
    QVERIFY(QFileInfo::exists(pdf));
    QVERIFY(QFileInfo::exists(m_profile.cache() + "/main.synctex.gz"));
#ifndef NO_POPPLER_PREVIEW
    auto configuration = dynamic_cast<ConfigManager *>(ConfigManagerInterface::getInstance());
    QVERIFY(configuration);
    PDFDocument viewer(configuration->pdfDocumentConfig);
    viewer.loadFile(pdf, QFileInfo(source)); viewer.show();
    QTRY_VERIFY_WITH_TIMEOUT(viewer.popplerDoc(), 15000);
    QCOMPARE(viewer.popplerDoc()->numPages(), 1);
    const auto page = viewer.popplerDoc()->page(0);
    QVERIFY(page);
    const QImage rendered = page->renderToImage(96, 96);
    QVERIFY(!rendered.isNull());
    QVERIFY(page->text(QRectF()).contains("Office revision"));
    QSynctex::Scanner scanner(pdf);
    QVERIFY(scanner.isValid());
    const auto target = scanner.syncFromTeX(QSynctex::TeXSyncPoint(source, 3, 1), pdf);
    QCOMPARE(target.page, 1); QVERIFY(!target.rects.isEmpty());
    const auto point = target.rects.first().center();
    auto back = scanner.editQuery(target.page, point.x(), point.y());
    QVERIFY(back.hasNext());
    const auto backwardSource = scanner.getNameFileInfo(QDir(m_profile.cache()), back.next());
    QCOMPARE(backwardSource.absoluteFilePath(), source);
    const QString diagnostics = qEnvironmentVariable("TEXSTUDIO_TEST_SCREENSHOT_DIR");
    QVERIFY(!diagnostics.isEmpty());
    QVERIFY(txsInstance->grab().save(diagnostics + "/remote-editor.png"));
    QVERIFY(viewer.grab().save(diagnostics + "/remote-pdf-viewer.png"));
    QVERIFY(rendered.save(diagnostics + "/remote-rendered-page.png"));
    viewer.close();
#else
    QFAIL("The remote desktop test requires the internal PDF viewer.");
#endif
    // Generated preview files stay inside the workspace and therefore use the
    // same transport, rather than trying to compile a client-only temp path.
    QVERIFY(previewDirectory(source).startsWith(m_profile.cache() + "/"));
}
void RemoteWorkspaceGuiTest::cleanup()
{
    if (m_view) {
        m_view->document->setClean();
        QMetaObject::invokeMethod(txsInstance, "fileClose", Qt::DirectConnection);
        m_view = nullptr;
    }
    if (m_profile.isValid()) QDir(m_profile.directory()).removeRecursively();
}
#endif
