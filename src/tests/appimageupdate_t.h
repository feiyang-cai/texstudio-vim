#ifndef APPIMAGEUPDATE_T_H
#define APPIMAGEUPDATE_T_H
#ifndef QT_NO_DEBUG
#include "appimageupdate.h"
#include "updatechecker.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class AppImageUpdateTest : public QObject
{
    Q_OBJECT
    static QJsonObject release(const QString &tag) {
        return {{"tag_name", tag}, {"draft", false}, {"published_at", "2026-10-18T00:00:00Z"},
                {"prerelease", Version::fromVimTag(tag).type != "stable"}};
    }
    static QByteArray image(const QByteArray &payload) {
        QByteArray bytes(20, '\0');
        bytes.replace(0, 6, QByteArray("\x7f" "ELF\x02\x01", 6));
        bytes.replace(8, 3, QByteArray("AI\x02", 3));
        bytes.replace(18, 2, QByteArray("\x3e\0", 2));
        return bytes + payload;
    }
    static bool write(const QString &path, const QByteArray &bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
                && file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
private slots:
    void selection_data() {
        QTest::addColumn<QString>("current");
        QTest::addColumn<int>("level");
        QTest::addColumn<QString>("expected");
        QTest::newRow("stable-vim-patch") << QString("4.9.8-r1") << 0 << QString("4.9.8-r1.2");
        QTest::newRow("vim-patch") << QString("4.9.9rc1-r1.1") << 1 << QString("4.9.9rc1-r1.10");
        QTest::newRow("stable-channel") << QString("4.9.9beta2-r1") << 0 << QString();
        QTest::newRow("upstream-and-vim") << QString("4.9.8-r1") << 1 << QString("4.9.9rc1-r1.10");
        QTest::newRow("newer-upstream-beta") << QString("4.9.8-r1") << 2 << QString("4.10.0beta1-r1");
        QTest::newRow("no-upstream-downgrade") << QString("4.10.0beta1-r1") << 1 << QString();
    }
    void selection() {
        QFETCH(QString, current); QFETCH(int, level); QFETCH(QString, expected);
        const auto tag = [](const QString &s) { return "texstudio-vim-" + s; };
        QJsonArray releases;
        // A high Vim revision on an older upstream must not win.
        for (const QString &s : {"4.9.7-r99", "4.9.8-r1.2", "4.9.9rc1-r1.2", "4.9.9rc1-r1.10", "4.10.0beta1-r1"})
            releases.append(release(tag(s)));
        const auto selected = UpdateChecker::newestRelease(QJsonDocument(releases).toJson(), level, Version::fromVimTag(tag(current)));
        QCOMPARE(selected.value("tag_name").toString(), expected.isEmpty() ? QString() : tag(expected));
    }
    void upstreamFinalWins() {
        const QString current = "texstudio-vim-4.9.9rc1-r1.10";
        const QString final = "texstudio-vim-4.9.9-r1.1";
        const QJsonArray releases{release(final), release("texstudio-vim-4.9.9rc2-r2")};
        QCOMPARE(UpdateChecker::newestRelease(QJsonDocument(releases).toJson(), 1,
                 Version::fromVimTag(current)).value("tag_name").toString(), final);
    }
    void invalidReleasesIgnored() {
        auto draft = release("texstudio-vim-9.0.0-r1"); draft["draft"] = true;
        auto unpublished = release("texstudio-vim-8.0.0-r1"); unpublished["published_at"] = QJsonValue();
        auto mislabeled = release("texstudio-vim-7.0.0-r1"); mislabeled["prerelease"] = true;
        const QJsonArray releases{draft, unpublished, mislabeled, release("4.9.9"), release("texstudio-vim-4.9.9-r1-2-gabcdef0")};
        QVERIFY(UpdateChecker::newestRelease(QJsonDocument(releases).toJson(), 2,
                Version::fromVimTag("texstudio-vim-4.9.8-r1")).isEmpty());
        QVERIFY(UpdateChecker::newestRelease("invalid json", 2, Version("4.9.8")).isEmpty());
    }
    void strictAssetMetadata() {
        const QString tag = "texstudio-vim-4.9.9-r1.2";
        const QString name = tag + "-x86_64.AppImage";
        QJsonObject asset{{"name", name}, {"state", "uploaded"}, {"size", 100},
                          {"digest", "sha256:" + QString(64, 'a')},
                          {"browser_download_url", "https://github.com/feiyang-cai/texstudio-vim/releases/download/" + tag + "/" + name}};
        auto metadata = release(tag);
        metadata["assets"] = QJsonArray{asset};
        QVERIFY(AppImageUpdate::releaseAsset(metadata).isValid());
        for (const QString &field : {"digest", "state", "size", "browser_download_url"}) {
            auto broken = asset; broken.remove(field);
            auto bad = metadata; bad["assets"] = QJsonArray{broken};
            QVERIFY2(!AppImageUpdate::releaseAsset(bad).isValid(), qPrintable(field));
        }
        auto bad = metadata; bad["assets"] = QJsonArray{asset, asset};
        QVERIFY(!AppImageUpdate::releaseAsset(bad).isValid());
        auto hostile = asset; hostile["browser_download_url"] = "https://example.org/payload.AppImage";
        bad["assets"] = QJsonArray{hostile};
        QVERIFY(!AppImageUpdate::releaseAsset(bad).isValid());
        hostile = asset; hostile["size"] = 1.5; bad["assets"] = QJsonArray{hostile};
        QVERIFY(!AppImageUpdate::releaseAsset(bad).isValid());
        hostile["size"] = 2.0 * 1024 * 1024 * 1024; bad["assets"] = QJsonArray{hostile};
        QVERIFY(!AppImageUpdate::releaseAsset(bad).isValid());
    }
    void replaceAndBackUp() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString target = dir.filePath("application with spaces.AppImage"), update = dir.filePath("next.AppImage");
        QVERIFY(write(target, image("old"))); QVERIFY(write(update, image("new and larger")));
        const auto oldDigest = AppImageUpdate::fileDigest(target), newDigest = AppImageUpdate::fileDigest(update);
        const auto permissions = QFileInfo(target).permissions();
        QString backup, error;
        QVERIFY2(AppImageUpdate::installVerified(target, update, oldDigest, newDigest, &backup, &error), qPrintable(error));
        QCOMPARE(AppImageUpdate::fileDigest(target), newDigest);
        QCOMPARE(AppImageUpdate::fileDigest(backup), oldDigest);
        QCOMPARE(QFileInfo(target).permissions(), permissions);
        QVERIFY(QFileInfo(update).exists());
        QVERIFY(backup.startsWith(target + ".previous-"));
    }
#ifdef Q_OS_UNIX
    void refusesSymbolicLinks() {
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString target = dir.filePath("current.AppImage"), update = dir.filePath("next.AppImage");
        const QString link = dir.filePath("linked.AppImage");
        QVERIFY(write(target, image("old"))); QVERIFY(write(update, image("new")));
        const auto oldDigest = AppImageUpdate::fileDigest(target), newDigest = AppImageUpdate::fileDigest(update);
        QVERIFY(QFile::link(target, link));
        QString backup, error;
        QVERIFY(!AppImageUpdate::installVerified(link, update, oldDigest, newDigest, &backup, &error));
        QVERIFY(!AppImageUpdate::installVerified(update, link, newDigest, oldDigest, &backup, &error));
        QCOMPARE(AppImageUpdate::fileDigest(target), oldDigest);
        QCOMPARE(AppImageUpdate::fileDigest(update), newDigest);
    }
#endif
    void failedUpdatePreservesOriginal_data() {
        QTest::addColumn<QString>("failure");
        for (const char *failure : {"corrupt-candidate", "changed-target", "wrong-old-digest", "invalid-header", "missing-candidate", "same-file"})
            QTest::newRow(failure) << QString::fromLatin1(failure);
    }
    void failedUpdatePreservesOriginal() {
        QFETCH(QString, failure);
        QTemporaryDir dir; QVERIFY(dir.isValid());
        const QString target = dir.filePath("current.AppImage"), update = dir.filePath("next.AppImage");
        QVERIFY(write(target, image("old"))); QVERIFY(write(update, image("new")));
        QByteArray oldDigest = AppImageUpdate::fileDigest(target), newDigest = AppImageUpdate::fileDigest(update);
        if (failure == "corrupt-candidate") QVERIFY(write(update, image("broken")));
        if (failure == "changed-target") QVERIFY(write(target, image("externally updated")));
        if (failure == "wrong-old-digest") oldDigest = QByteArray(32, 'x');
        if (failure == "invalid-header") { QVERIFY(write(update, "not an AppImage")); newDigest = AppImageUpdate::fileDigest(update); }
        if (failure == "missing-candidate") QVERIFY(QFile::remove(update));
        const auto before = AppImageUpdate::fileDigest(target);
        QString backup, error;
        QVERIFY(!AppImageUpdate::installVerified(target, failure == "same-file" ? target : update, oldDigest, newDigest, &backup, &error));
        QCOMPARE(AppImageUpdate::fileDigest(target), before);
        QVERIFY(backup.isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(QDir(dir.path()).entryList({"*.previous-*"}, QDir::Files).size(), 0);
    }
};
#endif
#endif
