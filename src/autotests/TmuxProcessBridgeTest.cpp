/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxProcessBridgeTest.h"

#include <QDialog>
#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>

#include "../MainWindow.h"
#include "../Screen.h"
#include "../ScreenWindow.h"
#include "../ViewManager.h"
#include "../terminalDisplay/TerminalDisplay.h"
#include "../tmux/TmuxController.h"
#include "../tmux/TmuxControllerRegistry.h"
#include "../tmux/TmuxGateway.h"
#include "../tmux/TmuxProcessBridge.h"
#include "../widgets/ViewContainer.h"
#include "TmuxTestFixture.h"

#include <fcntl.h>
#include <unistd.h>

using namespace Konsole;

namespace
{
QDialog *rshPrompt()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (widget->objectName() == QLatin1String("tmuxRshPrompt") && widget->isVisible()) {
            return qobject_cast<QDialog *>(widget);
        }
    }
    return nullptr;
}

QString terminalText(TerminalDisplay *display)
{
    const auto *screen = display->screenWindow()->screen();
    return screen->text(0, screen->getLines() * screen->getColumns(), Screen::PlainText);
}

bool hasControllingTerminal()
{
    const int fd = ::open("/dev/tty", O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
}
}

void TmuxProcessBridgeTest::initTestCase()
{
    QVERIFY(m_tmuxTmpDir.isValid());
    m_tmuxPath = TmuxTestFixture::findTmuxOrSkip();
    if (m_tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }
}

void TmuxProcessBridgeTest::cleanup()
{
    killTmuxServer();
}

QString TmuxProcessBridgeTest::tmuxSocketPath() const
{
    return m_tmuxTmpDir.path() + QStringLiteral("/process-test");
}

void TmuxProcessBridgeTest::killTmuxServer()
{
    QProcess kill;
    kill.start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("kill-server")});
    kill.waitForFinished(5000);
    // `kill-server` is async: the client returns after queueing the request,
    // but the server takes a moment to exit and unlink its socket. If the
    // next test races in with `new-session -d` while the socket is still on
    // disk, it connects to the dying server and hits "server exited
    // unexpectedly". Poll until the socket is gone (cap at 2s).
    QDeadlineTimer deadline(2000);
    while (QFile::exists(tmuxSocketPath()) && !deadline.hasExpired()) {
        QTest::qWait(20);
    }
}

void TmuxProcessBridgeTest::testConnectNoServer()
{
    // No tmux server running — tmux -C new-session -A should start one
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    auto *bridge = new TmuxProcessBridge(vm, mw);
    bool started = bridge->start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath()});
    QVERIFY(started);

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    // Wait for tmux to create a session and the controller to create pane tabs
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    auto *controller = bridge->controller();
    QVERIFY(controller);
    QVERIFY(TmuxControllerRegistry::instance()->controllers().contains(controller));

    delete mwGuard.data();
}

void TmuxProcessBridgeTest::testConnectAdvertisesTerminalType()
{
    const bool termWasSet = qEnvironmentVariableIsSet("TERM");
    const QByteArray originalTerm = qgetenv("TERM");
    qputenv("TERM", "dumb");

    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    auto *bridge = new TmuxProcessBridge(vm, mw);
    const bool started = bridge->start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath()});

    if (termWasSet) {
        qputenv("TERM", originalTerm);
    } else {
        qunsetenv("TERM");
    }
    QVERIFY(started);

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    QProcess listClients;
    listClients.start(m_tmuxPath,
                      {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("list-clients"), QStringLiteral("-F"), QStringLiteral("#{client_termname}")});
    QVERIFY(listClients.waitForFinished(5000));
    QCOMPARE(listClients.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(listClients.readAllStandardOutput()).trimmed(), QStringLiteral("xterm-256color"));

    delete mwGuard.data();
}

void TmuxProcessBridgeTest::testConnectServerNoSessions()
{
    // Start a tmux server, set exit-empty off, kill all sessions,
    // then connect — new-session -A should create a fresh session.

    QProcess tmuxStart;
    tmuxStart.start(m_tmuxPath,
                    {QStringLiteral("-S"),
                     tmuxSocketPath(),
                     QStringLiteral("new-session"),
                     QStringLiteral("-d"),
                     QStringLiteral("-s"),
                     QStringLiteral("bootstrap"),
                     QStringLiteral("sleep 30")});
    QVERIFY(tmuxStart.waitForFinished(5000));
    QCOMPARE(tmuxStart.exitCode(), 0);

    QProcess tmuxSetOption;
    tmuxSetOption.start(
        m_tmuxPath,
        {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("set-option"), QStringLiteral("-g"), QStringLiteral("exit-empty"), QStringLiteral("off")});
    QVERIFY(tmuxSetOption.waitForFinished(5000));

    QProcess tmuxKillSession;
    tmuxKillSession.start(m_tmuxPath,
                          {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("kill-session"), QStringLiteral("-t"), QStringLiteral("bootstrap")});
    QVERIFY(tmuxKillSession.waitForFinished(5000));
    QCOMPARE(tmuxKillSession.exitCode(), 0);

    // Connect — should create a new session
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    auto *bridge = new TmuxProcessBridge(vm, mw);
    bool started = bridge->start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath()});
    QVERIFY(started);

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    auto *controller = bridge->controller();
    QVERIFY(controller);

    // Verify tmux now has a session
    QProcess tmuxListAfter;
    tmuxListAfter.start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("list-sessions")});
    QVERIFY(tmuxListAfter.waitForFinished(5000));
    QCOMPARE(tmuxListAfter.exitCode(), 0);

    delete mwGuard.data();
}

void TmuxProcessBridgeTest::testConnectServerPreexistingSession()
{
    // Start a tmux server with a detached session, then connect —
    // new-session -A should attach to the existing session.

    QProcess tmuxStart;
    tmuxStart.start(m_tmuxPath,
                    {QStringLiteral("-S"),
                     tmuxSocketPath(),
                     QStringLiteral("new-session"),
                     QStringLiteral("-d"),
                     QStringLiteral("-s"),
                     QStringLiteral("existing"),
                     QStringLiteral("-x"),
                     QStringLiteral("80"),
                     QStringLiteral("-y"),
                     QStringLiteral("24"),
                     QStringLiteral("sleep 300")});
    QVERIFY(tmuxStart.waitForFinished(5000));
    QCOMPARE(tmuxStart.exitCode(), 0);

    // Connect — should attach to "existing"
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    auto *bridge = new TmuxProcessBridge(vm, mw);
    bool started = bridge->start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath()});
    QVERIFY(started);

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    auto *controller = bridge->controller();
    QVERIFY(controller);
    QVERIFY(TmuxControllerRegistry::instance()->controllers().contains(controller));

    // Verify tmux still has exactly one session
    QProcess tmuxListAfter;
    tmuxListAfter.start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("list-sessions")});
    QVERIFY(tmuxListAfter.waitForFinished(5000));
    QCOMPARE(tmuxListAfter.exitCode(), 0);
    QString output = QString::fromUtf8(tmuxListAfter.readAllStandardOutput());
    int sessionCount = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
    QCOMPARE(sessionCount, 1);

    delete mwGuard.data();
}

void TmuxProcessBridgeTest::testRshSingleTokenWrapper()
{
    // rsh="env" is a local no-op wrapper: `env <tmux> -S <socket> -C ...`
    // behaves identically to running tmux directly. Verifies the bridge
    // wraps argv correctly when rsh is a single token and tmuxPath is an
    // absolute path.
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    auto *bridge = new TmuxProcessBridge(vm, mw);
    bool started =
        bridge->start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath()}, {QStringLiteral("new-session"), QStringLiteral("-A")}, {QStringLiteral("env")});
    QVERIFY(started);
    QCOMPARE(bridge->rshCommand(), QStringList{QStringLiteral("env")});

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    auto *controller = bridge->controller();
    QVERIFY(controller);
    QVERIFY(TmuxControllerRegistry::instance()->controllers().contains(controller));

    delete mwGuard.data();
}

void TmuxProcessBridgeTest::testRshMultiTokenWrapperAndDefaultTmuxPath()
{
    // rsh="env KMUX_RSH_TEST=1" is a two-token wrapper that also sets an
    // env var on the wrapped tmux. tmuxPath is left empty, which in rsh
    // mode defaults to the literal string "tmux" — env finds it in PATH.
    // Verifies: (a) leading args beyond argv[0] are threaded through,
    //           (b) rsh mode skips local findExecutable on the tmux path,
    //           (c) the env var actually reaches the tmux server.
    const QStringList rshCommand = {QStringLiteral("env"), QStringLiteral("KMUX_RSH_TEST=1")};
    const QString sessionName = QStringLiteral("rshtest");

    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    auto *bridge = new TmuxProcessBridge(vm, mw);
    bool started = bridge->start(QString(),
                                 {QStringLiteral("-S"), tmuxSocketPath()},
                                 {QStringLiteral("new-session"), QStringLiteral("-A"), QStringLiteral("-s"), sessionName},
                                 rshCommand);
    QVERIFY(started);
    QCOMPARE(bridge->tmuxPath(), QStringLiteral("tmux"));
    QCOMPARE(bridge->rshCommand(), rshCommand);

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    // Verify KMUX_RSH_TEST reached the tmux server's process environment.
    // show-environment only reports vars tmux explicitly copies into session
    // envs (update-environment filter), so we query the server's own env via
    // run-shell, which execs the given shell command with the server's env.
    QProcess queryEnv;
    queryEnv.start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("run-shell"), QStringLiteral("printenv KMUX_RSH_TEST")});
    QVERIFY(queryEnv.waitForFinished(5000));
    QCOMPARE(queryEnv.exitCode(), 0);
    const QString output = QString::fromUtf8(queryEnv.readAllStandardOutput()).trimmed();
    QCOMPARE(output, QStringLiteral("1"));

    delete mwGuard.data();
}

void TmuxProcessBridgeTest::testRshAdvertisesTerminalTypeWithoutForwardedTerm()
{
    // Model an SSH transport without a PTY: the wrapper removes TERM before
    // running the remote command appended by kmux.
    const QStringList rshCommand = {QStringLiteral("env"), QStringLiteral("-u"), QStringLiteral("TERM")};

    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    auto *bridge = new TmuxProcessBridge(vm, mw);
    const bool started = bridge->start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath()}, {QStringLiteral("new-session"), QStringLiteral("-A")}, rshCommand);
    QVERIFY(started);
    QCOMPARE(bridge->rshCommand(), rshCommand);

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    QProcess listClients;
    listClients.start(m_tmuxPath,
                      {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("list-clients"), QStringLiteral("-F"), QStringLiteral("#{client_termname}")});
    QVERIFY(listClients.waitForFinished(5000));
    QCOMPARE(listClients.exitCode(), 0);
    QCOMPARE(QString::fromUtf8(listClients.readAllStandardOutput()).trimmed(), QStringLiteral("xterm-256color"));

    delete mwGuard.data();
}

void TmuxProcessBridgeTest::testRshGuiPrompt()
{
    if (hasControllingTerminal()) {
        QSKIP("Run with setsid to exercise a launch without a controlling terminal.");
    }
    MainWindow mw;
    auto *bridge = new TmuxProcessBridge(mw.viewManager(), &mw);
    const QString script = QStringLiteral(
        "printf 'Confirm host: '\n"
        "read -r answer\n"
        "[ \"$answer\" = yes ] || exit 3\n"
        "exec 9<>/dev/tty\n"
        "stty -echo <&9\n"
        "printf 'Password: ' >&9\n"
        "read -r secret <&9\n"
        "stty echo <&9\n"
        "[ \"$secret\" = GUI-secret-42 ] || exit 4\n"
        "exec 9>&-\n"
        "exec \"$@\"\n");
    QSignalSpy ready(bridge, &TmuxProcessBridge::ready);
    QVERIFY(bridge->start(m_tmuxPath,
                          {QStringLiteral("-S"), tmuxSocketPath()},
                          {QStringLiteral("new-session"), QStringLiteral("-A")},
                          {QStringLiteral("bash"), QStringLiteral("-c"), script, QStringLiteral("rsh-test")}));

    for (int attempt = 0; attempt < 2; ++attempt) {
        QTRY_VERIFY_WITH_TIMEOUT(rshPrompt(), 5000);
        QPointer<QDialog> prompt(rshPrompt());
        auto *display = prompt->findChild<TerminalDisplay *>();
        QVERIFY(display);
        bool earlyReply = false;
        bridge->controller()->gateway()->sendCommand(TmuxCommand(QStringLiteral("display-message")).flag(QStringLiteral("-p")), [&](bool, const QString &) {
            earlyReply = true;
        });
        QTRY_VERIFY(terminalText(display).contains(QLatin1String("Confirm host:")));
        QTest::keyClicks(display, QStringLiteral("yes"));
        QTest::keyClick(display, Qt::Key_Return);
        QTRY_VERIFY(terminalText(display).contains(QLatin1String("Password:")));
        QTest::keyClicks(display, QStringLiteral("GUI-secret-42"));
        QTest::qWait(150);
        QVERIFY(!terminalText(display).contains(QLatin1String("GUI-secret-42")));
        // On reconnect, a visible prompt must suspend the handshake deadline.
        QTest::qWait(300);
        QCOMPARE(ready.count(), attempt);
        QVERIFY(!earlyReply);
        QVERIFY(prompt && prompt->isVisible());
        QTest::keyClick(display, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), attempt + 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(earlyReply, 5000);
        QVERIFY(!prompt->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(bridge->controller()->sessionId() >= 0, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!mw.viewManager()->sessions().isEmpty(), 5000);

        // A PTY left in canonical mode truncates long control commands at 4K.
        const QString payload(10000, QLatin1Char('x'));
        bool replied = false;
        bool success = false;
        QString response;
        bridge->controller()->gateway()->sendCommand(TmuxCommand(QStringLiteral("display-message")).flag(QStringLiteral("-p")).format(payload),
                                                     [&](bool ok, const QString &output) {
                                                         success = ok;
                                                         response = output;
                                                         replied = true;
                                                     });
        QTRY_VERIFY_WITH_TIMEOUT(replied, 5000);
        QVERIFY(success);
        QCOMPARE(response, payload);
        if (attempt == 0) {
            bridge->setHandshakeTimeoutMs(200);
            bridge->requestReconnect();
        }
    }
}

void TmuxProcessBridgeTest::testRshGuiCancel()
{
    if (hasControllingTerminal()) {
        QSKIP("Run with setsid to exercise a launch without a controlling terminal.");
    }
    MainWindow mw;
    auto *bridge = new TmuxProcessBridge(mw.viewManager(), &mw);
    QSignalSpy failed(bridge, &TmuxProcessBridge::startupFailed);
    // No output: the delayed terminal must still make silent input possible.
    QVERIFY(bridge->start(m_tmuxPath,
                          {QStringLiteral("-S"), tmuxSocketPath()},
                          {QStringLiteral("new-session"), QStringLiteral("-A")},
                          {QStringLiteral("bash"), QStringLiteral("-c"), QStringLiteral("read -r answer; exec \"$@\""), QStringLiteral("rsh-test")}));
    QTRY_VERIFY_WITH_TIMEOUT(rshPrompt(), 5000);
    rshPrompt()->reject();
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 5000);
    QVERIFY(!rshPrompt());
    QProcess probe;
    probe.start(m_tmuxPath, {QStringLiteral("-S"), tmuxSocketPath(), QStringLiteral("list-sessions")});
    QVERIFY(probe.waitForFinished(5000));
    QVERIFY(probe.exitCode() != 0);
}

QTEST_MAIN(TmuxProcessBridgeTest)

#include "moc_TmuxProcessBridgeTest.cpp"
