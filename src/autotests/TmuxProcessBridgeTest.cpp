/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxProcessBridgeTest.h"

#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTest>

#include "../MainWindow.h"
#include "../ViewManager.h"
#include "../terminalDisplay/TerminalDisplay.h"
#include "../tmux/TmuxController.h"
#include "../tmux/TmuxControllerRegistry.h"
#include "../tmux/TmuxProcessBridge.h"
#include "../widgets/ViewContainer.h"
#include "TmuxTestDSL.h"

using namespace Konsole;

void TmuxProcessBridgeTest::initTestCase()
{
    QVERIFY(m_tmuxTmpDir.isValid());
    m_tmuxPath = TmuxTestDSL::findTmuxOrSkip();
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

QTEST_MAIN(TmuxProcessBridgeTest)

#include "moc_TmuxProcessBridgeTest.cpp"
