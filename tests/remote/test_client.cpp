#include "remoteclient.h"
#include <QtTest>
#include <cstdio>
#include <cstring>
using namespace RemoteWorkspace;

class RemoteTests : public QObject {
    Q_OBJECT
    static void write(const QString &name, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(name).absolutePath());
        QFile file(name); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), qint64(bytes.size()));
    }
    static Profile makeProfile(const QString &folder) {
        Profile profile; profile.manifest = folder + "/profile.json";
        profile.host = "office-test"; profile.root = "/remote/paper"; profile.ready = true;
        QDir().mkpath(profile.cache()); return profile;
    }
    static void stage(const Profile &profile, const QString &name, const QByteArray &expected, const QByteArray &next) {
        const QString folder = QUuid::createUuid().toString(QUuid::WithoutBraces), token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        write(profile.directory() + "/pulls/" + folder + "/" + token, next);
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(next,QCryptographicHash::Sha256).toHex());
        QVERIFY(writeJson(profile.directory() + "/pull.json", {{"directory", folder}, {"files", QJsonArray{
            QJsonObject{{"path", name}, {"stage", token}, {"expected", QString::fromLatin1(expected)}, {"local", hash}, {"remote", hash}}}}}));
    }
private slots:
    void pathMappings() {
        QTemporaryDir temp; auto profile = makeProfile(temp.path());
        profile.root = "C:/Users/Office/Paper"; profile.platform = "win32";
        QCOMPARE(toRemote(profile, profile.cache()+"/chapter/main.tex"), profile.root+"/chapter/main.tex");
        QCOMPARE(toLocal(profile, "c:\\users\\office\\paper\\chapter\\main.tex"), profile.cache()+"/chapter/main.tex");
        QCOMPARE(toLocal(profile, "C:/Users/Office/Paper-other/main.tex"), QString("C:/Users/Office/Paper-other/main.tex"));
        QCOMPARE(toRemote(profile, temp.path()+"/project-other/paper.tex"), temp.path()+"/project-other/paper.tex");
        QCOMPARE(mapCommandArgument(profile, "-output-directory="+profile.cache()+"/output"), "-output-directory="+profile.root+"/output");
        QCOMPARE(mapCommandArgument(profile, profile.cache()+"-other/output"), profile.cache()+"-other/output");
        QString nativeCache = profile.cache(); nativeCache.replace('/', '\\');
        profile.root = "/office/paper"; profile.platform = "linux";
        QCOMPARE(mapCommandArgument(profile, nativeCache+"\\chapter\\main.tex"), profile.root+"/chapter/main.tex");
    }
    void rejectsNonportableOrEscapingNames() {
        for (const QString &name : {"../x", "/tmp/x", "C:/x", "a/../../x", "a\\..\\x", ".", "aux.tex", "a.", "a/b ", "a?b"}) QVERIFY(!validRelativePath(name));
        QVERIFY(validRelativePath("chapter 'quoted'; $name/中文.tex"));
    }
    void appliesVerifiedDownload() {
        QTemporaryDir temp; auto profile = makeProfile(temp.path());
        write(profile.cache()+"/paper.tex", "old"); const auto old = fileHash(profile.cache()+"/paper.tex");
        QVERIFY(writeJson(profile.directory()+"/state.json", {{"paper.tex", QJsonObject{{"local", QString::fromLatin1(old)}, {"remote", QString::fromLatin1(old)}}}}));
        stage(profile,"paper.tex",old,"new"); QString error;
        QVERIFY2(applyPull(profile,&error),qPrintable(error));
        QCOMPARE(fileHash(profile.cache()+"/paper.tex"),QCryptographicHash::hash("new",QCryptographicHash::Sha256).toHex());
    }
    void preservesSaveThatArrivedDuringDownload() {
        QTemporaryDir temp; auto profile = makeProfile(temp.path());
        write(profile.cache()+"/paper.tex", "old"); const auto old = fileHash(profile.cache()+"/paper.tex");
        stage(profile,"paper.tex",old,"office revision");
        write(profile.cache()+"/paper.tex", "new home revision"); QString error;
        QVERIFY(!applyPull(profile,&error)); QVERIFY(error.contains("conflict"));
        QCOMPARE(fileHash(profile.cache()+"/paper.tex"), QCryptographicHash::hash("new home revision",QCryptographicHash::Sha256).toHex());
        QVERIFY(QFileInfo::exists(profile.directory()+"/conflicts.json"));
    }
    void preservesDirtyEditorBuffer() {
        QTemporaryDir temp; auto profile = makeProfile(temp.path());
        write(profile.cache()+"/paper.tex", "old"); const auto old = fileHash(profile.cache()+"/paper.tex");
        stage(profile,"paper.tex",old,"remote change"); QString error;
        QVERIFY(!applyPull(profile,&error,[](const QString&){return false;}));
        QCOMPARE(fileHash(profile.cache()+"/paper.tex"),old);
    }
    void refusesTamperedDownload() {
        QTemporaryDir temp; auto profile = makeProfile(temp.path());
        stage(profile,"paper.tex",{},"downloaded");
        QFile manifest(profile.directory()+"/pull.json"); QVERIFY(manifest.open(QIODevice::ReadOnly));
        const auto pull=QJsonDocument::fromJson(manifest.readAll()).object();
        const auto token=pull.value("files").toArray().first().toObject().value("stage").toString();
        write(profile.directory()+"/pulls/"+pull.value("directory").toString()+"/"+token,"tampered");
        QString error; QVERIFY(!applyPull(profile,&error)); QVERIFY(!QFileInfo::exists(profile.cache()+"/paper.tex"));
    }
    void sshWorkflow() {
        const QString config=qEnvironmentVariable("TXS_TEST_SSH_CONFIG"), root=qEnvironmentVariable("TXS_TEST_REMOTE_ROOT");
        if(config.isEmpty() || root.isEmpty()) { QSKIP("SSH integration requires the disposable SSH fixture."); }
        QTemporaryDir temp; auto profile=makeProfile(temp.path()); profile.sshConfig=config;profile.root=root;profile.ready=false;
        profile.python=qEnvironmentVariable("TXS_TEST_REMOTE_PYTHON","python3");
        QVERIFY(profile.save());
        auto worker=[&](const QString &operation,const QJsonObject &data=QJsonObject()) {
            QProcess process;process.setProcessChannelMode(QProcess::MergedChannels);
            process.start(QCoreApplication::applicationFilePath(),workerArguments(profile,operation,data));
            if(!process.waitForFinished(120000)) { process.kill();process.waitForFinished(); return QByteArray("timeout"); }
            const auto output=process.readAll();
            return process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0?QByteArray():output;
        };
        QCOMPARE(worker("connect"),QByteArray());profile=Profile::load(profile.manifest);QVERIFY(profile.ready);
        QVERIFY(QFileInfo::exists(profile.cache()+"/main.tex"));
        const QByteArray edited="% remote editing αβ\n\\documentclass{article}\n\\begin{document}Home revision.\\end{document}\n";
        write(profile.cache()+"/main.tex",edited);QCOMPARE(worker("sync"),QByteArray());
        Connection connection;QVERIFY2(connection.open(profile),qPrintable(connection.error()));
        auto received=connection.request("read",{{"path","main.tex"}});QCOMPARE(QByteArray::fromBase64(received.value("data").toString().toLatin1()),edited);
        // A remote tool creates an artifact; downloads are staged until the editor applies them.
        const QJsonArray argv{"python", "-c", "from pathlib import Path;Path('output.bin').write_bytes(b'\\0artifact\\xff')"};
        QCOMPARE(worker("command",{{"argv",argv},{"cwd",profile.cache()}}),QByteArray());
        QString error;QVERIFY2(applyPull(profile,&error),qPrintable(error));QVERIFY(QFileInfo::exists(profile.cache()+"/output.bin"));
        const auto old=received.value("hash");
        auto changed=connection.request("put",{{"path","main.tex"},{"data",QString::fromLatin1(QByteArray("office revision").toBase64())},{"expected",old}});
        QVERIFY2(connection.error().isEmpty(),qPrintable(connection.error()));
        write(profile.cache()+"/main.tex","home conflict");QVERIFY(!worker("sync").isEmpty());
        auto preserved=connection.request("read",{{"path","main.tex"}});QCOMPARE(QByteArray::fromBase64(preserved.value("data").toString().toLatin1()),QByteArray("office revision"));
        QVERIFY(!applyPull(profile,&error));
        const auto observed = preserved.value("hash");
        // An edit after comparison blocks an explicit merged upload, too.
        connection.request("put",{{"path","main.tex"},{"data",QString::fromLatin1(QByteArray("newer office").toBase64())},{"expected",observed}});
        QVERIFY(!worker("resolve",{{"path","main.tex"},{"expected",observed}}).isEmpty());
        QCOMPARE(worker("refresh"),QByteArray());QVERIFY(!applyPull(profile,&error));
        const auto latest=connection.request("read",{{"path","main.tex"}}).value("hash");
        write(profile.cache()+"/main.tex","manually merged revision");
        QCOMPARE(worker("resolve",{{"path","main.tex"},{"expected",latest}}),QByteArray());
        preserved=connection.request("read",{{"path","main.tex"}});
        QCOMPARE(QByteArray::fromBase64(preserved.value("data").toString().toLatin1()),QByteArray("manually merged revision"));
    }
    void unknownHostKeyIsRejected() {
        const QString config = qEnvironmentVariable("TXS_TEST_SSH_CONFIG");
        if (config.isEmpty()) QSKIP("Requires the disposable SSH fixture.");
        QTemporaryDir temp;
        QFile original(config); QVERIFY(original.open(QIODevice::ReadOnly));
        auto contents = original.readAll();
        QVERIFY(contents.contains("HostKeyAlias office-fixture"));
        contents.replace("HostKeyAlias office-fixture", "HostKeyAlias untrusted-fixture");
        const QString untrusted = temp.path() + "/untrusted-config";
        write(untrusted, contents);
        auto profile = makeProfile(temp.path()); profile.sshConfig = untrusted;
        profile.root = qEnvironmentVariable("TXS_TEST_REMOTE_ROOT");
        profile.python = qEnvironmentVariable("TXS_TEST_REMOTE_PYTHON", "python3");
        Connection connection;
        QVERIFY(!connection.open(profile));
        QVERIFY2(connection.error().contains("Host key verification failed"), qPrintable(connection.error()));
    }
};
int main(int argc,char **argv) {
    if(argc>1&&std::strcmp(argv[1],"--remote-workspace-worker")==0)return workerMain(argc,argv);
    QCoreApplication app(argc,argv);QCoreApplication::setApplicationName("TeXstudio");QCoreApplication::setOrganizationName("texstudio");
    RemoteTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "test_client.moc"
