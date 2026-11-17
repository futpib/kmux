/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "EscapeSequenceUrlExtractor.h"
#include "Screen.h"
#include "ScreenWindow.h"
#include "Vt102Emulation.h"
#include "filterHotSpots/EscapeSequenceUrlFilterHotSpot.h"
#include "profile/Profile.h"
#include "session/Session.h"
#include "session/SessionManager.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>

using namespace Konsole;

class EscapeSequenceUrlTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<Vt102Emulation> _emulation;
    ScreenWindow *_window = nullptr;

    void receive(const QByteArray &data)
    {
        _emulation->receiveData(data.constData(), data.size());
    }

    EscapeSequenceUrlExtractor *extractor() const
    {
        return _window->screen()->urlExtractor();
    }

private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void init()
    {
        _emulation = std::make_unique<Vt102Emulation>();
        _emulation->reset();
        _emulation->setCodec("UTF-8");
        _emulation->setImageSize(24, 120);
        _window = _emulation->createWindow();
        _window->screen()->setEnableUrlExtractor(true);
        Profile profile;
        profile.useBuiltin();
        extractor()->setAllowedLinkSchema(profile.escapedLinksSchema());
    }

    void testValidation_data()
    {
        QTest::addColumn<QByteArray>("url");
        QTest::addColumn<QString>("expected");
        QTest::newRow("https") << QByteArray("https://example.org/path?q=1#part") << QStringLiteral("https://example.org/path?q=1#part");
        QTest::newRow("http") << QByteArray("http://localhost:8080/") << QStringLiteral("http://localhost:8080/");
        QTest::newRow("ipv6") << QByteArray("http://[::1]:8080/") << QStringLiteral("http://[::1]:8080/");
        QTest::newRow("international-host") << QStringLiteral("https://éxample.org/").toUtf8() << QStringLiteral("https://xn--xample-9ua.org/");
        QTest::newRow("encoded") << QByteArray("https://example.org/a%20b") << QStringLiteral("https://example.org/a%20b");
        QTest::newRow("local-file") << QByteArray("file:///tmp/example.txt") << QStringLiteral("file:///tmp/example.txt");
        QTest::newRow("localhost-file") << QByteArray("file://localhost/tmp/example.txt") << QStringLiteral("file:///tmp/example.txt");
        QTest::newRow("remote-file") << QByteArray("file://remote.example/tmp/example.txt") << QString();
        QTest::newRow("relative-file") << QByteArray("file:relative.txt") << QString();
        QTest::newRow("javascript") << QByteArray("javascript:alert(1)") << QString();
        QTest::newRow("data") << QByteArray("data:text/html,test") << QString();
        QTest::newRow("custom-handler") << QByteArray("ssh://example.org/") << QString();
        QTest::newRow("relative") << QByteArray("//example.org/") << QString();
        QTest::newRow("missing-host") << QByteArray("https:///path") << QString();
        QTest::newRow("credentials") << QByteArray("https://trusted.example@evil.example/") << QString();
        QTest::newRow("malformed-percent") << QByteArray("https://example.org/%zz") << QString();
        QTest::newRow("encoded-control") << QByteArray("https://example.org/%0aHidden") << QString();
        QTest::newRow("encoded-bidi") << QByteArray("https://example.org/%E2%80%AEhidden") << QString();
        QTest::newRow("raw-bidi") << QStringLiteral("https://example.org/\u202Ehidden").toUtf8() << QString();
    }

    void testValidation()
    {
        QFETCH(QByteArray, url);
        QFETCH(QString, expected);
        receive("\033]8;id=test;" + url + "\033\\label\033]8;;\033\\");
        const auto history = extractor()->history();
        QCOMPARE(history.size(), expected.isEmpty() ? 0 : 1);
        if (!history.isEmpty()) {
            QCOMPARE(history.first().url, expected);
            QCOMPARE(history.first().text, QStringLiteral("label"));
        }
        QVERIFY(!extractor()->reading());

        // A rejected link must not suppress the next valid link.
        receive("\033]8;;https://example.org/recovery\alabel\033]8;;\a");
        QCOMPARE(extractor()->history().size(), expected.isEmpty() ? 1 : 2);
        QCOMPARE(extractor()->history().last().url, QStringLiteral("https://example.org/recovery"));
    }

    void testOtherOscDoesNotChangeDestination()
    {
        receive("\033]8;;https://example.org/\alabel\033]2;title;https://other.example/\a\033]8;;\a");
        QCOMPARE(extractor()->history().size(), 1);
        QCOMPARE(extractor()->history().first().url, QStringLiteral("https://example.org/"));
    }

    void testExplicitOpenAndClose()
    {
        receive("\033]8;;\a\033]8;;https://example.org/one\aone\033]8;;https://example.org/two\atwo\033]8;;\a\033]8;;\a");
        QCOMPARE(extractor()->history().size(), 2);
        QCOMPARE(extractor()->history().at(0).url, QStringLiteral("https://example.org/one"));
        QCOMPARE(extractor()->history().at(1).url, QStringLiteral("https://example.org/two"));
        QVERIFY(!extractor()->reading());
    }

    void testEmptyAndMalformedLinks()
    {
        receive("\033]8;;https://example.org/\a\033]8;;\a");
        receive("\033]8;https://example.org/\alabel\033]8;;\a");
        receive("\033]80;;https://example.org/\alabel\033]8;;\a");
        QVERIFY(extractor()->history().isEmpty());
        QVERIFY(!extractor()->reading());
    }

    void testBoundedInput()
    {
        receive("\033]8;;https://example.org/\a" + QByteArray(65536, 'x') + "\033]8;;\a");
        QVERIFY(extractor()->history().isEmpty());
        QVERIFY(!extractor()->reading());
        receive("\033]8;;https://example.org/\alabel\033]8;;\a");
        QCOMPARE(extractor()->history().size(), 1);
    }

    void testClearAndPolicyChange()
    {
        receive("\033]8;;https://example.org/\alabel");
        extractor()->clear();
        QVERIFY(!extractor()->reading());
        receive("\033]8;;https://example.org/\alabel\033]8;;\a");
        extractor()->setAllowedLinkSchema({QStringLiteral("file://")});
        QVERIFY(extractor()->history().isEmpty());
    }

    void testProfileCommandCannotChangeLinkPolicy()
    {
        Profile::Ptr profile(new Profile);
        profile->useBuiltin();
        profile->setProperty(Profile::AllowEscapedLinks, false);
        auto *manager = SessionManager::instance();
        std::unique_ptr<Session> session(manager->createSession(profile));
        const auto cleanup = qScopeGuard([&] {
            Q_EMIT session->finished(session.get());
        });
        Q_EMIT session->profileChangeCommandReceived(QStringLiteral("AllowEscapedLinks=true;EscapedLinksSchema=ssh://;ColorScheme=TestOscPolicy"));
        QCOMPARE(manager->sessionProfile(session.get())->colorScheme(), QStringLiteral("TestOscPolicy"));
        QVERIFY(!manager->sessionProfile(session.get())->allowEscapedLinks());
        QCOMPARE(manager->sessionProfile(session.get())->escapedLinksSchema(), profile->escapedLinksSchema());
    }

    void testHiddenDestinationConfirmation_data()
    {
        QTest::addColumn<int>("action");
        QTest::newRow("click") << 0;
        QTest::newRow("context-menu") << 1;
        QTest::newRow("drag") << 2;
    }

    void testHiddenDestinationConfirmation()
    {
        QFETCH(int, action);
        const QString destination = QStringLiteral("https://example.org/%3Cb%3Eactual%3C/b%3E");
        EscapeSequenceUrlHotSpot hotspot(0, 0, 0, 10, QStringLiteral("https://trusted.example/"), destination);
        bool seen = false;
        bool plainText = false;
        bool safeDefault = false;
        QString shown;
        QTimer closer;
        connect(&closer, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!dialog) {
                return;
            }
            seen = true;
            plainText = dialog->textFormat() == Qt::PlainText;
            safeDefault = dialog->standardButton(dialog->defaultButton()) == QMessageBox::Cancel;
            shown = dialog->text() + dialog->informativeText();
            dialog->reject();
        });
        closer.start(20);
        QAction openAction;
        openAction.setObjectName(QStringLiteral("open-action"));
        if (action == 2) {
            hotspot.startDrag(nullptr, {});
        } else {
            hotspot.activate(action == 1 ? &openAction : nullptr);
        }
        QVERIFY(seen);
        QVERIFY(plainText);
        QVERIFY(safeDefault);
        QVERIFY(shown.contains(destination));
    }

    void testCopyUsesDestination()
    {
        const QString destination = QStringLiteral("https://example.org/actual");
        EscapeSequenceUrlHotSpot hotspot(0, 0, 0, 5, QStringLiteral("label"), destination);
        QAction copyAction;
        copyAction.setObjectName(QStringLiteral("copy-action"));
        hotspot.activate(&copyAction);
        QCOMPARE(QApplication::clipboard()->text(), destination);
        QVERIFY(!QApplication::activeModalWidget());
    }
};

QTEST_MAIN(EscapeSequenceUrlTest)

#include "EscapeSequenceUrlTest.moc"
