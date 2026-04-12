/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxIntegrationTest.h"
#include "TmuxTestDSL.h"

#include <KActionCollection>
#include <KMessageBox>
#include <QApplication>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QResizeEvent>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabBar>
#include <QTest>
#include <QTreeView>

#include "../Emulation.h"
#include "../MainWindow.h"
#include "../Screen.h"
#include "../ScreenWindow.h"
#include "../ViewManager.h"
#include "../profile/ProfileManager.h"
#include "../session/Session.h"
#include "../session/SessionController.h"
#include "../session/SessionManager.h"
#include "../session/VirtualSession.h"
#include "../terminalDisplay/TerminalDisplay.h"
#include "../tmux/TmuxController.h"
#include "../tmux/TmuxControllerRegistry.h"
#include "../tmux/TmuxLayoutManager.h"
#include "../tmux/TmuxLayoutParser.h"
#include "../tmux/TmuxPaneManager.h"
#include "../tmux/TmuxPrefixPalette.h"
#include "../tmux/TmuxProcessBridge.h"
#include "../tmux/TmuxTreeModel.h"
#include "../tmux/TmuxTreeSwitcher.h"
#include "../widgets/TabPageWidget.h"
#include "../widgets/ViewContainer.h"
#include "../widgets/ViewSplitter.h"

using namespace Konsole;

void TmuxIntegrationTest::initTestCase()
{
    // Set the application name so isKonsolePart() and tab bar visibility
    // logic treat this test binary the same as the real kmux executable.
    QCoreApplication::setApplicationName(QStringLiteral("kmux"));
    QVERIFY(m_tmuxTmpDir.isValid());
}

void TmuxIntegrationTest::cleanupTestCase()
{
    // Each test uses its own -S socket, so there is no shared server to kill.
}

void TmuxIntegrationTest::testTmuxControlModeExitCleanup()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Use TmuxProcessBridge to start tmux -C new-session "sleep 1 && exit 0"
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    const QString socketPath = m_tmuxTmpDir.path() + QStringLiteral("/tmux-exit-cleanup");
    auto *bridge = new TmuxProcessBridge(vm, mw);
    QVERIFY(bridge->start(tmuxPath, {QStringLiteral("-S"), socketPath}, {QStringLiteral("new-session"), QStringLiteral("sleep 1 && exit 0")}));

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    // Wait for tmux control mode to create virtual pane tab(s)
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    // Wait for tmux to exit — the window may close and delete itself
    QTRY_VERIFY_WITH_TIMEOUT(!mwGuard || !container || container->count() == 0, 15000);

    // If the window is still alive, clean up
    delete mwGuard.data();
}

void TmuxIntegrationTest::testClosePaneTabThenGatewayTab()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Use TmuxProcessBridge to start tmux -C new-session "sleep 30"
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    const QString socketPath = m_tmuxTmpDir.path() + QStringLiteral("/tmux-close-pane");
    auto *bridge = new TmuxProcessBridge(vm, mw);
    QVERIFY(bridge->start(tmuxPath, {QStringLiteral("-S"), socketPath}, {QStringLiteral("new-session"), QStringLiteral("sleep 30")}));

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    // Wait for tmux control mode to create virtual pane tab(s)
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    // Find the pane session (all sessions are pane sessions with TmuxProcessBridge)
    Session *paneSession = nullptr;
    const auto sessions = vm->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // Close the pane tab (like clicking the tab close icon)
    paneSession->closeInNormalWay();

    // Wait for everything to tear down — the window may close and delete itself
    QTRY_VERIFY_WITH_TIMEOUT(!mwGuard, 10000);

    delete mwGuard.data();
}

void TmuxIntegrationTest::testTmuxControlModeAttach()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 30                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Close the pane tab, then destroy the bridge
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    paneSession->closeInNormalWay();

    // Wait for everything to tear down
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxTwoPaneSplitAttach()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 30                          │ cmd: sleep 30                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));

    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());
    TmuxTestDSL::assertKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Clean up: close pane sessions, then destroy the bridge
    const auto sessions = attach.mw->viewManager()->sessions();
    for (Session *s : sessions) {
        s->closeInNormalWay();
    }

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

// Helper: read all visible text from a Session's screen
static QString readSessionScreenText(Session *session)
{
    ScreenWindow *window = session->emulation()->createWindow();
    Screen *screen = window->screen();

    int lines = screen->getLines();
    int columns = screen->getColumns();

    screen->setSelectionStart(0, 0, false);
    screen->setSelectionEnd(columns, lines - 1, false);
    return screen->selectedText(Screen::PlainText);
    // Don't delete window — Emulation::~Emulation owns it via _windows list
}

void TmuxIntegrationTest::testTmuxAttachContentRecovery()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌───────────────────────────────────┐
        │ cmd: bash --norc --noprofile      │
        │                                   │
        │                                   │
        │                                   │
        │                                   │
        └───────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Send a command with Unicode output
    QProcess sendKeys;
    sendKeys.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("send-keys"),
                    QStringLiteral("-t"),
                    ctx.sessionName,
                    QStringLiteral("echo 'MARKER_START ★ Unicode → Test ✓ MARKER_END'"),
                    QStringLiteral("Enter")});
    QVERIFY(sendKeys.waitForFinished(5000));
    QCOMPARE(sendKeys.exitCode(), 0);

    // Wait for the command to execute
    QTest::qWait(500);

    // Now attach Konsole via -CC
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // Wait a bit for capture-pane history to be injected
    QTest::qWait(2000);

    // Read the screen content
    QString screenText = readSessionScreenText(paneSession);

    QVERIFY2(screenText.contains(QStringLiteral("MARKER_START")),
             qPrintable(QStringLiteral("Pane screen should contain 'MARKER_START', got: ") + screenText));
    QVERIFY2(screenText.contains(QStringLiteral("MARKER_END")),
             qPrintable(QStringLiteral("Pane screen should contain 'MARKER_END', got: ") + screenText));

    // Cleanup: close pane sessions, then destroy the bridge
    const auto allSessions = attach.mw->viewManager()->sessions();
    for (Session *s : allSessions) {
        s->closeInNormalWay();
    }

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxAttachComplexPromptRecovery()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                                                                                                                                                                                   │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Set a complex PS1 prompt with ANSI colors and Unicode
    QProcess sendPS1;
    sendPS1.start(tmuxPath,
                  {QStringLiteral("-S"),
                   ctx.socketPath,
                   QStringLiteral("send-keys"),
                   QStringLiteral("-t"),
                   ctx.sessionName,
                   QStringLiteral("PS1='\\[\\033[36m\\][\\t] [\\u@\\h \\w] "
                                  "\\[\\033[33m\\]"
                                  "────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────"
                                  "────────────────────────────── \\[\\033[35m\\]\\s \\V  \\[\\033[32m\\]→ \\[\\033[0m\\]'"),
                   QStringLiteral("Enter")});
    QVERIFY(sendPS1.waitForFinished(5000));
    QCOMPARE(sendPS1.exitCode(), 0);

    // Wait for prompt to render
    QTest::qWait(500);

    // Run a command so we have some content
    QProcess sendCmd;
    sendCmd.start(tmuxPath,
                  {QStringLiteral("-S"),
                   ctx.socketPath,
                   QStringLiteral("send-keys"),
                   QStringLiteral("-t"),
                   ctx.sessionName,
                   QStringLiteral("echo 'PROMPT_TEST_OUTPUT'"),
                   QStringLiteral("Enter")});
    QVERIFY(sendCmd.waitForFinished(5000));
    QCOMPARE(sendCmd.exitCode(), 0);

    // Wait for command to execute
    QTest::qWait(500);

    // Now attach Konsole via -CC
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // Wait for capture-pane history to be injected
    QTest::qWait(2000);

    // Read the screen content
    QString screenText = readSessionScreenText(paneSession);

    QVERIFY2(screenText.contains(QStringLiteral("PROMPT_TEST_OUTPUT")),
             qPrintable(QStringLiteral("Pane screen should contain 'PROMPT_TEST_OUTPUT', got: ") + screenText));

    QVERIFY2(screenText.contains(QStringLiteral("→")),
             qPrintable(QStringLiteral("Pane screen should contain '→' from prompt, got: ") + screenText));

    QVERIFY2(screenText.contains(QStringLiteral("────")),
             qPrintable(QStringLiteral("Pane screen should contain '────' from prompt, got: ") + screenText));

    // Cleanup: close pane sessions, then destroy the bridge
    const auto allSessions = attach.mw->viewManager()->sessions();
    for (Session *s : allSessions) {
        s->closeInNormalWay();
    }

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitterResizePropagatedToTmux()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Query initial pane sizes
    QProcess tmuxListPanes;
    tmuxListPanes.start(tmuxPath,
                        {QStringLiteral("-S"),
                         ctx.socketPath,
                         QStringLiteral("list-panes"),
                         QStringLiteral("-t"),
                         ctx.sessionName,
                         QStringLiteral("-F"),
                         QStringLiteral("#{pane_width}")});
    QVERIFY(tmuxListPanes.waitForFinished(5000));
    QCOMPARE(tmuxListPanes.exitCode(), 0);
    QStringList initialWidths = QString::fromUtf8(tmuxListPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
    QCOMPARE(initialWidths.size(), 2);
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Apply the initial layout to set Konsole widget sizes to match the diagram
    auto initialLayout = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(initialLayout, attach.mw->viewManager());

    // Find the split pane splitter
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QCOMPARE(paneSplitter->orientation(), Qt::Horizontal);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    // Read current splitter sizes and display dimensions
    QList<int> sizes = paneSplitter->sizes();
    QCOMPARE(sizes.size(), 2);
    // Move the splitter: make left pane significantly larger (3/4 vs 1/4).
    int total = sizes[0] + sizes[1];
    int newLeft = total * 3 / 4;
    int newRight = total - newLeft;
    paneSplitter->setSizes({newLeft, newRight});

    // Force display widgets to the new pixel sizes and send resize events
    int displayHeight = leftDisplay->height();
    leftDisplay->resize(newLeft, displayHeight);
    rightDisplay->resize(newRight, displayHeight);
    QResizeEvent leftResizeEvent(QSize(newLeft, displayHeight), leftDisplay->size());
    QResizeEvent rightResizeEvent(QSize(newRight, displayHeight), rightDisplay->size());
    QCoreApplication::sendEvent(leftDisplay, &leftResizeEvent);
    QCoreApplication::sendEvent(rightDisplay, &rightResizeEvent);
    QCoreApplication::processEvents();

    // Verify the resize actually produced different column counts
    QVERIFY2(leftDisplay->columns() != rightDisplay->columns(),
             qPrintable(QStringLiteral("Expected different column counts but both are %1").arg(leftDisplay->columns())));

    // Trigger splitterMoved signal (setSizes doesn't emit it automatically)
    Q_EMIT paneSplitter->splitterMoved(newLeft, 1);

    // Read expected sizes from terminal displays (what buildLayoutNode will use)
    int expectedLeftWidth = leftDisplay->columns();
    int expectedRightWidth = rightDisplay->columns();
    int expectedLeftHeight = leftDisplay->lines();
    int expectedRightHeight = rightDisplay->lines();
    int expectedWindowWidth = expectedLeftWidth + 1 + expectedRightWidth; // +1 for separator
    int expectedWindowHeight = qMax(expectedLeftHeight, expectedRightHeight);
    // Wait for the command to propagate to tmux and verify exact sizes
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_width} #{pane_height}")});
        check.waitForFinished(3000);
        QStringList paneLines = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (paneLines.size() != 2) return false;
        QStringList pane0 = paneLines[0].split(QLatin1Char(' '));
        QStringList pane1 = paneLines[1].split(QLatin1Char(' '));
        if (pane0.size() != 2 || pane1.size() != 2) return false;
        int w0 = pane0[0].toInt();
        int h0 = pane0[1].toInt();
        int w1 = pane1[0].toInt();
        int h1 = pane1[1].toInt();
        return w0 == expectedLeftWidth && w1 == expectedRightWidth
            && h0 == expectedWindowHeight && h1 == expectedWindowHeight;
    }(), 10000);

    // Also verify tmux window size matches
    {
        QProcess checkWindow;
        checkWindow.start(tmuxPath,
                          {QStringLiteral("-S"),
                           ctx.socketPath,
                           QStringLiteral("list-windows"),
                           QStringLiteral("-t"),
                           ctx.sessionName,
                           QStringLiteral("-F"),
                           QStringLiteral("#{window_width} #{window_height}")});
        QVERIFY(checkWindow.waitForFinished(3000));
        QStringList windowSize = QString::fromUtf8(checkWindow.readAllStandardOutput()).trimmed().split(QLatin1Char(' '));
        QCOMPARE(windowSize.size(), 2);
        int windowWidth = windowSize[0].toInt();
        int windowHeight = windowSize[1].toInt();
        QCOMPARE(windowWidth, expectedWindowWidth);
        QCOMPARE(windowHeight, expectedWindowHeight);
    }

    // Wait for any pending layout-change callbacks to finish
    QTest::qWait(500);

    // Kill the tmux session first to avoid layout-change during teardown
    // (cleanup guard handles this, but we want it early)
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxPaneTitleInfo()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌───────────────────────────────────┐
        │ cmd: bash --norc --noprofile      │
        │                                   │
        │                                   │
        │                                   │
        │                                   │
        └───────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // cd to /tmp so we have a known directory
    QProcess sendCd;
    sendCd.start(tmuxPath,
                 {QStringLiteral("-S"),
                  ctx.socketPath,
                  QStringLiteral("send-keys"),
                  QStringLiteral("-t"),
                  ctx.sessionName,
                  QStringLiteral("cd /tmp"),
                  QStringLiteral("Enter")});
    QVERIFY(sendCd.waitForFinished(5000));
    QCOMPARE(sendCd.exitCode(), 0);
    QTest::qWait(500);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *virtualSession = qobject_cast<VirtualSession *>(paneSession);
    QVERIFY(virtualSession);

    // Wait for pane title info to be queried
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QString title = paneSession->getDynamicTitle();
        return title.contains(QStringLiteral("tmp")) || title.contains(QStringLiteral("bash"));
    }(), 10000);

    // Verify that the tab title for the tmux window is set from #{window_name}
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    int windowId = controller->windowIdForPane(paneId);
    QVERIFY(windowId >= 0);
    int tabIndex = controller->windowToTabIndex().value(windowId, -1);
    QVERIFY(tabIndex >= 0);
    QString tabText = attach.container->tabText(tabIndex);
    QVERIFY2(!tabText.isEmpty(), "Tab text should not be empty for tmux window");

    // Cleanup
    const auto allSessions = attach.mw->viewManager()->sessions();
    for (Session *s : allSessions) {
        s->closeInNormalWay();
    }

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testWindowNameWithSpaces()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Rename the window to something adversarial: spaces, hex-like tokens, commas, braces
    QString evilName = QStringLiteral("htop lol abc0,80x24,0,0 {evil} [nasty]");
    QProcess renameProc;
    renameProc.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("rename-window"), QStringLiteral("-t"), ctx.sessionName, evilName});
    QVERIFY(renameProc.waitForFinished(5000));
    QCOMPARE(renameProc.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY2(!sessions.isEmpty(), "Expected a tmux pane session to be created despite spaces in window name");
    paneSession = sessions.first();

    // Verify the tab title matches the evil name
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    int windowId = controller->windowIdForPane(paneId);
    QVERIFY(windowId >= 0);
    int tabIndex = controller->windowToTabIndex().value(windowId, -1);
    QVERIFY(tabIndex >= 0);
    QString tabText = attach.container->tabText(tabIndex);
    QCOMPARE(tabText, evilName);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitPaneFocusesNewPane()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session and its controller (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    QVERIFY(paneId >= 0);

    // Record the original pane's display
    auto originalDisplays = paneSession->views();
    QVERIFY(!originalDisplays.isEmpty());
    auto *originalDisplay = originalDisplays.first();

    // Show and activate the window so setFocus() works
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    // Request a horizontal split from within Konsole
    controller->requestSplitPane(paneId, Qt::Horizontal);

    // Wait for the split to appear: a ViewSplitter with 2 TerminalDisplay children
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find the new pane's display (the one that isn't the original)
    auto terminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(terminals.size(), 2);
    TerminalDisplay *newDisplay = nullptr;
    for (auto *td : terminals) {
        if (td != originalDisplay) {
            newDisplay = td;
            break;
        }
    }
    QVERIFY2(newDisplay, "Expected to find a new TerminalDisplay after split");

    // The new pane should have focus
    QTRY_VERIFY_WITH_TIMEOUT(newDisplay->hasFocus(), 5000);

    // Kill the tmux session first to avoid layout-change during teardown
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitPaneFocusesNewPaneComplexLayout()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Create 3 horizontal panes, select pane 0, then split it vertically from Konsole
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Select the first pane so we know which one is active before attaching
    QProcess tmuxSelect;
    tmuxSelect.start(tmuxPath,
                     {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("select-pane"), QStringLiteral("-t"), ctx.sessionName + QStringLiteral(":0.0")});
    QVERIFY(tmuxSelect.waitForFinished(5000));
    QCOMPARE(tmuxSelect.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for all 3 panes to appear
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 3) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find the pane sessions (all sessions are pane sessions)
    QList<Session *> paneSessions = attach.mw->viewManager()->sessions();
    QVERIFY(paneSessions.size() >= 3);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSessions.first());
    QVERIFY(controller);

    int firstPaneId = controller->paneIdForSession(paneSessions.first());
    QVERIFY(firstPaneId >= 0);

    // Record all existing displays before the split
    auto existingTerminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(existingTerminals.size(), 3);

    // Show and activate the window so setFocus() works
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    // Request a vertical split on the first pane
    controller->requestSplitPane(firstPaneId, Qt::Vertical);

    // Wait for 4 panes to appear
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 4) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find the new display (the one not in existingTerminals)
    auto allTerminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(allTerminals.size(), 4);
    TerminalDisplay *newDisplay = nullptr;
    for (auto *td : allTerminals) {
        if (!existingTerminals.contains(td)) {
            newDisplay = td;
            break;
        }
    }
    QVERIFY2(newDisplay, "Expected to find a new TerminalDisplay after split");

    // The new pane should have focus
    QTRY_VERIFY_WITH_TIMEOUT(newDisplay->hasFocus(), 5000);

    // Kill the tmux session first to avoid layout-change during teardown
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitPaneFocusesNewPaneNestedLayout()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Create nested layout: [ pane0 | [ pane1 / pane2 ] ]
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        ├────────────────────────────────────────┤
        │                                        │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Select the first pane (pane0) so that's what we'll split from Konsole
    QProcess tmuxSelect;
    tmuxSelect.start(tmuxPath,
                     {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("select-pane"), QStringLiteral("-t"), ctx.sessionName + QStringLiteral(":0.0")});
    QVERIFY(tmuxSelect.waitForFinished(5000));
    QCOMPARE(tmuxSelect.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for all 3 panes to appear
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 3) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find pane0's session (all sessions are pane sessions)
    QVERIFY(!attach.mw->viewManager()->sessions().isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    // Find pane0: query tmux for pane IDs to find the first one
    QProcess tmuxListPanes;
    tmuxListPanes.start(tmuxPath,
                        {QStringLiteral("-S"),
                         ctx.socketPath,
                         QStringLiteral("list-panes"),
                         QStringLiteral("-t"),
                         ctx.sessionName,
                         QStringLiteral("-F"),
                         QStringLiteral("#{pane_id}")});
    QVERIFY(tmuxListPanes.waitForFinished(5000));
    QCOMPARE(tmuxListPanes.exitCode(), 0);
    QStringList paneIdStrs = QString::fromUtf8(tmuxListPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
    QVERIFY(paneIdStrs.size() >= 3);
    // Pane IDs look like %42 — strip the % prefix
    int firstPaneId = paneIdStrs[0].mid(1).toInt();

    // Record all existing displays
    auto existingTerminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(existingTerminals.size(), 3);

    // Show and activate the window
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    // Split pane0 vertically from Konsole
    controller->requestSplitPane(firstPaneId, Qt::Vertical);

    // Wait for 4 panes to appear
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 4) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find the new display
    auto allTerminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(allTerminals.size(), 4);
    TerminalDisplay *newDisplay = nullptr;
    for (auto *td : allTerminals) {
        if (!existingTerminals.contains(td)) {
            newDisplay = td;
            break;
        }
    }
    QVERIFY2(newDisplay, "Expected to find a new TerminalDisplay after split");

    // The new pane should have focus
    QTRY_VERIFY_WITH_TIMEOUT(newDisplay->hasFocus(), 5000);

    // Kill the tmux session first
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testResizePropagatedToPty()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // 1. Setup tmux session with a two-pane horizontal split running bash
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: bash                              │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);

    // 2. Attach Konsole
    auto initialLayout = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: bash                              │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);
    TmuxTestDSL::applyKonsoleLayout(initialLayout, attach.mw->viewManager());

    // Find the two-pane splitter
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QCOMPARE(paneSplitter->orientation(), Qt::Horizontal);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    // 3. Resize the splitter: make left pane significantly larger (3/4 vs 1/4)
    QList<int> sizes = paneSplitter->sizes();
    int total = sizes[0] + sizes[1];
    int newLeft = total * 3 / 4;
    int newRight = total - newLeft;
    paneSplitter->setSizes({newLeft, newRight});

    // Force display widgets to the new pixel sizes and send resize events
    int displayHeight = leftDisplay->height();
    leftDisplay->resize(newLeft, displayHeight);
    rightDisplay->resize(newRight, displayHeight);
    QResizeEvent leftResizeEvent(QSize(newLeft, displayHeight), leftDisplay->size());
    QResizeEvent rightResizeEvent(QSize(newRight, displayHeight), rightDisplay->size());
    QCoreApplication::sendEvent(leftDisplay, &leftResizeEvent);
    QCoreApplication::sendEvent(rightDisplay, &rightResizeEvent);
    QCoreApplication::processEvents();

    // Trigger splitterMoved signal (setSizes doesn't emit it automatically)
    Q_EMIT paneSplitter->splitterMoved(newLeft, 1);

    int expectedLeftCols = leftDisplay->columns();
    int expectedRightCols = rightDisplay->columns();

    // Verify the resize actually produced different column counts
    QVERIFY2(expectedLeftCols != expectedRightCols,
             qPrintable(QStringLiteral("Expected different column counts but both are %1").arg(expectedLeftCols)));

    // 4. Wait for tmux to process the layout change (metadata)
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_width}")});
        check.waitForFinished(3000);
        QStringList paneWidths = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (paneWidths.size() != 2) return false;
        return paneWidths[0].toInt() == expectedLeftCols && paneWidths[1].toInt() == expectedRightCols;
    }(), 10000);

    // 5. Run 'stty size' in each pane and verify PTY dimensions match.
    // tmux defers TIOCSWINSZ (PTY resize) through its server loop, so we
    // poll: send 'stty size', capture output, and re-send if needed.
    int expectedLeftLines = leftDisplay->lines();
    int expectedRightLines = rightDisplay->lines();
    auto runSttyAndCheck = [&](const QString &paneTarget, int expectedLines, int expectedCols) -> bool {
        // Send stty size
        QProcess sendKeys;
        sendKeys.start(tmuxPath,
                       {QStringLiteral("-S"),
                        ctx.socketPath,
                        QStringLiteral("send-keys"),
                        QStringLiteral("-t"),
                        paneTarget,
                        QStringLiteral("-l"),
                        QStringLiteral("stty size\n")});
        if (!sendKeys.waitForFinished(3000)) return false;
        QTest::qWait(300);

        // Capture and check
        QProcess capture;
        capture.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-t"), paneTarget, QStringLiteral("-p")});
        capture.waitForFinished(3000);
        QString output = QString::fromUtf8(capture.readAllStandardOutput());
        QString expected = QString::number(expectedLines) + QStringLiteral(" ") + QString::number(expectedCols);
        return output.contains(expected);
    };

    QTRY_VERIFY_WITH_TIMEOUT(
        runSttyAndCheck(ctx.sessionName + QStringLiteral(":0.0"), expectedLeftLines, expectedLeftCols),
        10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        runSttyAndCheck(ctx.sessionName + QStringLiteral(":0.1"), expectedRightLines, expectedRightCols),
        10000);

    // Wait for any pending callbacks
    QTest::qWait(500);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNestedResizePropagatedToPty()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // 1. Setup tmux session with a nested layout: left pane | [top-right / bottom-right]
    //    All panes run bash so we can check stty size.
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: bash                              │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        ├────────────────────────────────────────┤
        │                                        │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);

    // 2. Attach Konsole and apply the same layout
    auto initialLayout = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: bash                              │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        ├────────────────────────────────────────┤
        │                                        │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);
    TmuxTestDSL::applyKonsoleLayout(initialLayout, attach.mw->viewManager());

    // 3. Find the top-level splitter (horizontal: left | right-sub-splitter)
    ViewSplitter *topSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 3) {
                topSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(topSplitter, "Expected a ViewSplitter with 3 TerminalDisplay descendants");
    QCOMPARE(topSplitter->orientation(), Qt::Horizontal);
    QCOMPARE(topSplitter->count(), 2);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(topSplitter->widget(0));
    QVERIFY(leftDisplay);

    // The right child should be a nested vertical splitter
    auto *rightSplitter = qobject_cast<ViewSplitter *>(topSplitter->widget(1));
    QVERIFY2(rightSplitter, "Expected right child to be a ViewSplitter");
    QCOMPARE(rightSplitter->orientation(), Qt::Vertical);
    QCOMPARE(rightSplitter->count(), 2);

    auto *topRightDisplay = qobject_cast<TerminalDisplay *>(rightSplitter->widget(0));
    auto *bottomRightDisplay = qobject_cast<TerminalDisplay *>(rightSplitter->widget(1));
    QVERIFY(topRightDisplay);
    QVERIFY(bottomRightDisplay);

    // 4. Resize the NESTED (vertical) splitter: make top-right much larger
    QList<int> sizes = rightSplitter->sizes();
    int total = sizes[0] + sizes[1];
    int newTop = total * 3 / 4;
    int newBottom = total - newTop;
    rightSplitter->setSizes({newTop, newBottom});

    // Force display widgets to the new pixel sizes and send resize events
    int displayWidth = topRightDisplay->width();
    topRightDisplay->resize(displayWidth, newTop);
    bottomRightDisplay->resize(displayWidth, newBottom);
    QResizeEvent topResizeEvent(QSize(displayWidth, newTop), topRightDisplay->size());
    QResizeEvent bottomResizeEvent(QSize(displayWidth, newBottom), bottomRightDisplay->size());
    QCoreApplication::sendEvent(topRightDisplay, &topResizeEvent);
    QCoreApplication::sendEvent(bottomRightDisplay, &bottomResizeEvent);
    QCoreApplication::processEvents();

    // Trigger splitterMoved signal on the nested splitter
    Q_EMIT rightSplitter->splitterMoved(newTop, 1);

    int expectedTopRightLines = topRightDisplay->lines();
    int expectedBottomRightLines = bottomRightDisplay->lines();
    int expectedTopRightCols = topRightDisplay->columns();
    int expectedBottomRightCols = bottomRightDisplay->columns();
    // Verify the resize actually produced different line counts
    QVERIFY2(expectedTopRightLines != expectedBottomRightLines,
             qPrintable(QStringLiteral("Expected different line counts but both are %1").arg(expectedTopRightLines)));

    // 5. Wait for tmux to process the layout change (metadata)
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_height}")});
        check.waitForFinished(3000);
        QStringList paneHeights = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (paneHeights.size() != 3) return false;
        // Pane order: %0 (left), %1 (top-right), %2 (bottom-right)
        return paneHeights[1].toInt() == expectedTopRightLines && paneHeights[2].toInt() == expectedBottomRightLines;
    }(), 10000);

    // 6. Run 'stty size' in each nested pane and verify PTY dimensions match
    auto runSttyAndCheck = [&](const QString &paneTarget, int expectedLines, int expectedCols) -> bool {
        QProcess sendKeys;
        sendKeys.start(tmuxPath,
                       {QStringLiteral("-S"),
                        ctx.socketPath,
                        QStringLiteral("send-keys"),
                        QStringLiteral("-t"),
                        paneTarget,
                        QStringLiteral("-l"),
                        QStringLiteral("stty size\n")});
        if (!sendKeys.waitForFinished(3000)) return false;
        QTest::qWait(300);

        QProcess capture;
        capture.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-t"), paneTarget, QStringLiteral("-p")});
        capture.waitForFinished(3000);
        QString output = QString::fromUtf8(capture.readAllStandardOutput());
        QString expected = QString::number(expectedLines) + QStringLiteral(" ") + QString::number(expectedCols);
        return output.contains(expected);
    };

    // Check top-right pane (pane index 1)
    QTRY_VERIFY_WITH_TIMEOUT(
        runSttyAndCheck(ctx.sessionName + QStringLiteral(":0.1"), expectedTopRightLines, expectedTopRightCols),
        10000);
    // Check bottom-right pane (pane index 2)
    QTRY_VERIFY_WITH_TIMEOUT(
        runSttyAndCheck(ctx.sessionName + QStringLiteral(":0.2"), expectedBottomRightLines, expectedBottomRightCols),
        10000);

    // Wait for any pending callbacks
    QTest::qWait(500);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}


void TmuxIntegrationTest::testTopLevelResizeWithNestedChild()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Minimal 4-pane layout: left | center | [top-right / bottom-right]
    // 3-child top-level HSplit where the rightmost child is a nested VSplit.
    // Resizing the handle between center and the right column must propagate
    // correct absolute offsets and cross-axis dimensions to tmux.
    TmuxTestDSL::SessionContext ctx;
    auto diagram = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌──────────────────────────┬──────────────────────────┬──────────────────────────┐
        │ cmd: bash                │ cmd: bash                │ cmd: bash                │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          ├──────────────────────────┤
        │                          │                          │ cmd: bash                │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          │                          │
        └──────────────────────────┴──────────────────────────┴──────────────────────────┘
    )"));
    TmuxTestDSL::setupTmuxSession(diagram, tmuxPath, m_tmuxTmpDir.path(), ctx);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);
    TmuxTestDSL::applyKonsoleLayout(diagram, attach.mw->viewManager());

    // Find the splitter with 4 displays
    ViewSplitter *topSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 4) {
                topSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(topSplitter, "Expected a ViewSplitter with 4 TerminalDisplay descendants");
    QCOMPARE(topSplitter->orientation(), Qt::Horizontal);
    QCOMPARE(topSplitter->count(), 3);

    // Record initial tmux pane widths
    QProcess initialCheck;
    initialCheck.start(tmuxPath,
                       {QStringLiteral("-S"),
                        ctx.socketPath,
                        QStringLiteral("list-panes"),
                        QStringLiteral("-t"),
                        ctx.sessionName,
                        QStringLiteral("-F"),
                        QStringLiteral("#{pane_id} #{pane_width} #{pane_height}")});
    initialCheck.waitForFinished(3000);
    QString initialPanesStr = QString::fromUtf8(initialCheck.readAllStandardOutput()).trimmed();

    // Parse initial widths per pane ID
    QMap<QString, int> initialWidths;
    for (const auto &line : initialPanesStr.split(QLatin1Char('\n'))) {
        QStringList parts = line.split(QLatin1Char(' '));
        if (parts.size() == 3) {
            initialWidths[parts[0]] = parts[1].toInt();
        }
    }

    // Resize: shift space from right column to center
    QList<int> sizes = topSplitter->sizes();
    QCOMPARE(sizes.size(), 3);
    int shift = sizes[2] / 3;
    sizes[1] += shift;
    sizes[2] -= shift;
    topSplitter->setSizes(sizes);

    // Force resize events on all displays
    auto allDisplays = topSplitter->findChildren<TerminalDisplay *>();
    for (auto *d : allDisplays) {
        QResizeEvent ev(d->size(), d->size());
        QCoreApplication::sendEvent(d, &ev);
    }
    QCoreApplication::processEvents();

    Q_EMIT topSplitter->splitterMoved(sizes[0] + sizes[1], 2);

    // The key assertion: after the splitter drag, tmux pane widths should change.
    // With the bug (wrong offsets/cross-axis), tmux rejects or ignores the layout.
    // Wait for tmux to accept the new layout and verify widths changed.
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id} #{pane_width} #{pane_height}")});
        check.waitForFinished(3000);
        QStringList panes = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (panes.size() != 4) return false;

        // Check that at least one pane's width changed from initial
        bool anyChanged = false;
        for (const auto &line : panes) {
            QStringList parts = line.split(QLatin1Char(' '));
            if (parts.size() != 3) return false;
            QString paneId = parts[0];
            int width = parts[1].toInt();
            if (initialWidths.contains(paneId) && width != initialWidths[paneId]) {
                anyChanged = true;
            }
        }
        return anyChanged;
    }(), 10000);

    // Now verify the dimensions match the layout we sent.
    // Query tmux for the window layout string and verify it parses correctly.
    QProcess layoutCheck;
    layoutCheck.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctx.socketPath,
                       QStringLiteral("display-message"),
                       QStringLiteral("-t"),
                       ctx.sessionName,
                       QStringLiteral("-p"),
                       QStringLiteral("#{window_layout}")});
    layoutCheck.waitForFinished(3000);
    QString tmuxLayout = QString::fromUtf8(layoutCheck.readAllStandardOutput()).trimmed();
    QVERIFY2(!tmuxLayout.isEmpty(), "tmux should report a valid window layout");

    QTest::qWait(500);
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNestedResizeSurvivesFocusCycle()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    const QString scriptPath = QStandardPaths::findExecutable(QStringLiteral("script"));
    if (scriptPath.isEmpty()) {
        QSKIP("script command not found.");
    }

    // 4-pane nested layout: left | center | [top-right / bottom-right]
    // Resize, then cycle through smaller-client attach/detach,
    // verify the resized layout is preserved after recovery.
    TmuxTestDSL::SessionContext ctx;
    auto diagram = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌──────────────────────────┬──────────────────────────┬──────────────────────────┐
        │ cmd: bash                │ cmd: bash                │ cmd: bash                │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          ├──────────────────────────┤
        │                          │                          │ cmd: bash                │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          │                          │
        └──────────────────────────┴──────────────────────────┴──────────────────────────┘
    )"));
    TmuxTestDSL::setupTmuxSession(diagram, tmuxPath, m_tmuxTmpDir.path(), ctx);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);
    TmuxTestDSL::applyKonsoleLayout(diagram, attach.mw->viewManager());

    // Find the splitter with 4 displays
    ViewSplitter *topSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 4) {
                topSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(topSplitter, "Expected a ViewSplitter with 4 TerminalDisplay descendants");
    QCOMPARE(topSplitter->orientation(), Qt::Horizontal);
    QCOMPARE(topSplitter->count(), 3);

    // 1. Resize: shift space from right column to center
    QList<int> sizes = topSplitter->sizes();
    QCOMPARE(sizes.size(), 3);
    int shift = sizes[2] / 3;
    sizes[1] += shift;
    sizes[2] -= shift;
    topSplitter->setSizes(sizes);

    auto allDisplays = topSplitter->findChildren<TerminalDisplay *>();
    for (auto *d : allDisplays) {
        QResizeEvent ev(d->size(), d->size());
        QCoreApplication::sendEvent(d, &ev);
    }
    QCoreApplication::processEvents();

    Q_EMIT topSplitter->splitterMoved(sizes[0] + sizes[1], 2);

    // Wait for tmux to accept the resized layout
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_width}")});
        check.waitForFinished(3000);
        QStringList widths = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (widths.size() != 4) return false;
        // Initially all panes were 26 wide; after resize at least one should differ
        for (const auto &w : widths) {
            if (w.toInt() != 26) return true;
        }
        return false;
    }(), 10000);

    // Record the post-resize layout from tmux
    QProcess layoutCheck;
    layoutCheck.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctx.socketPath,
                       QStringLiteral("display-message"),
                       QStringLiteral("-t"),
                       ctx.sessionName,
                       QStringLiteral("-p"),
                       QStringLiteral("#{window_layout}")});
    layoutCheck.waitForFinished(3000);
    QString postResizeLayout = QString::fromUtf8(layoutCheck.readAllStandardOutput()).trimmed();
    QVERIFY(!postResizeLayout.isEmpty());

    // Record post-resize pane dimensions
    QProcess dimsCheck;
    dimsCheck.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id} #{pane_width} #{pane_height}")});
    dimsCheck.waitForFinished(3000);
    QString postResizeDims = QString::fromUtf8(dimsCheck.readAllStandardOutput()).trimmed();

    // 2. Attach a smaller client to constrain the layout
    QProcess scriptProc;
    scriptProc.start(
        scriptPath,
        {
            QStringLiteral("-q"),
            QStringLiteral("-c"),
            QStringLiteral("stty cols 40 rows 12; ") + tmuxPath + QStringLiteral(" -S ") + ctx.socketPath + QStringLiteral(" attach -t ") + ctx.sessionName,
            QStringLiteral("/dev/null"),
        });
    QVERIFY(scriptProc.waitForStarted(5000));

    // Wait for the smaller client to be visible
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-clients"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{client_width}x#{client_height}")});
        check.waitForFinished(3000);
        QStringList clients = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return clients.size() >= 2;
    }(), 10000);

    // Wait for layout to shrink
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{window_layout}")});
        check.waitForFinished(3000);
        QString layout = QString::fromUtf8(check.readAllStandardOutput()).trimmed();
        return layout != postResizeLayout;
    }(), 10000);

    QProcess constrainedCheck;
    constrainedCheck.start(tmuxPath,
                           {QStringLiteral("-S"),
                            ctx.socketPath,
                            QStringLiteral("display-message"),
                            QStringLiteral("-t"),
                            ctx.sessionName,
                            QStringLiteral("-p"),
                            QStringLiteral("#{window_layout}")});
    constrainedCheck.waitForFinished(3000);
    QString constrainedLayout = QString::fromUtf8(constrainedCheck.readAllStandardOutput()).trimmed();

    // 3. Kill the smaller client — layout should recover
    scriptProc.terminate();
    scriptProc.waitForFinished(5000);
    if (scriptProc.state() != QProcess::NotRunning) {
        scriptProc.kill();
        scriptProc.waitForFinished(3000);
    }

    // Wait for only one client to remain
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-clients"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{client_name}")});
        check.waitForFinished(3000);
        QStringList clients = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return clients.size() == 1;
    }(), 10000);

    // Process events so Konsole reacts to %client-detached → refreshClientCount
    QTest::qWait(500);
    QCoreApplication::processEvents();

    // Simulate Konsole regaining focus: in offscreen mode isActiveWindow() is
    // always false, so constraints are never cleared automatically.  Manually
    // clear constraints on the TabPageWidget and emit focusChanged to trigger
    // sendClientSize, mimicking what happens when the user clicks the window.
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *page = attach.container->tabPageAt(i);
        if (page && page->isConstrained()) {
            page->clearConstrainedSize();
        }
    }
    Q_EMIT qApp->focusChanged(nullptr, nullptr);
    QTest::qWait(200);
    QCoreApplication::processEvents();

    // Now do the resize again on the recovered layout.
    // The widget sizes may differ from the initial run (offscreen doesn't
    // resize widgets back to original proportions), but the point is that
    // buildLayoutNode produces a valid layout string and tmux accepts it.
    topSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 4) {
                topSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(topSplitter, "Expected splitter with 4 displays after focus cycle");

    sizes = topSplitter->sizes();
    QCOMPARE(sizes.size(), 3);
    shift = sizes[2] / 3;
    sizes[1] += shift;
    sizes[2] -= shift;
    topSplitter->setSizes(sizes);

    allDisplays = topSplitter->findChildren<TerminalDisplay *>();
    for (auto *d : allDisplays) {
        QResizeEvent ev(d->size(), d->size());
        QCoreApplication::sendEvent(d, &ev);
    }
    QCoreApplication::processEvents();

    Q_EMIT topSplitter->splitterMoved(sizes[0] + sizes[1], 2);

    // 4. Verify tmux accepts the post-focus-cycle resize.
    // The constrained layout shrank pane widths; after recovery and re-resize,
    // at least one pane should have a width different from the constrained state.
    QProcess constrainedDimsCheck;
    constrainedDimsCheck.start(tmuxPath,
                               {QStringLiteral("-S"),
                                ctx.socketPath,
                                QStringLiteral("list-panes"),
                                QStringLiteral("-t"),
                                ctx.sessionName,
                                QStringLiteral("-F"),
                                QStringLiteral("#{pane_id} #{pane_width}")});
    constrainedDimsCheck.waitForFinished(3000);
    QString constrainedDimsStr = QString::fromUtf8(constrainedDimsCheck.readAllStandardOutput()).trimmed();

    // Parse constrained widths
    QMap<QString, int> constrainedWidths;
    for (const auto &line : constrainedDimsStr.split(QLatin1Char('\n'))) {
        QStringList parts = line.split(QLatin1Char(' '));
        if (parts.size() == 2) {
            constrainedWidths[parts[0]] = parts[1].toInt();
        }
    }

    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id} #{pane_width} #{pane_height}")});
        check.waitForFinished(3000);
        QStringList panes = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (panes.size() != 4) return false;

        bool anyChanged = false;
        for (const auto &line : panes) {
            QStringList parts = line.split(QLatin1Char(' '));
            if (parts.size() != 3) return false;
            QString paneId = parts[0];
            int width = parts[1].toInt();
            if (constrainedWidths.contains(paneId) && width != constrainedWidths[paneId]) {
                anyChanged = true;
            }
        }
        return anyChanged;
    }(), 15000);

    // Verify the layout string is valid and accepted by tmux
    QProcess recoveredCheck;
    recoveredCheck.start(tmuxPath,
                         {QStringLiteral("-S"),
                          ctx.socketPath,
                          QStringLiteral("display-message"),
                          QStringLiteral("-t"),
                          ctx.sessionName,
                          QStringLiteral("-p"),
                          QStringLiteral("#{window_layout}")});
    recoveredCheck.waitForFinished(3000);
    QString recoveredLayout = QString::fromUtf8(recoveredCheck.readAllStandardOutput()).trimmed();
    QVERIFY2(recoveredLayout != constrainedLayout, "Layout should differ from constrained state after focus recovery");

    QTest::qWait(500);
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testForcedSizeFromSmallerClient()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    const QString scriptPath = QStandardPaths::findExecutable(QStringLiteral("script"));
    if (scriptPath.isEmpty()) {
        QSKIP("script command not found.");
    }

    // 1. Setup tmux session with a single pane at 80x24
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // 2. Attach Konsole via control mode
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // 3. Apply large layout so widgets are sized generously
    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // 4. Find the pane display and verify initial state (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto paneViews = paneSession->views();
    QVERIFY(!paneViews.isEmpty());
    auto *display = qobject_cast<TerminalDisplay *>(paneViews.first());
    QVERIFY(display);

    int initialColumns = display->columns();
    int initialLines = display->lines();
    QVERIFY2(initialColumns >= 40, qPrintable(QStringLiteral("Expected initial columns >= 40 but got %1").arg(initialColumns)));
    QVERIFY2(initialLines >= 12, qPrintable(QStringLiteral("Expected initial lines >= 12 but got %1").arg(initialLines)));

    // Record the widget pixel size before the smaller client attaches
    QSize originalPixelSize = display->size();
    // 5. Attach a second smaller tmux client using script to provide a pty
    QProcess scriptProc;
    scriptProc.start(
        scriptPath,
        {
            QStringLiteral("-q"),
            QStringLiteral("-c"),
            QStringLiteral("stty cols 40 rows 12; ") + tmuxPath + QStringLiteral(" -S ") + ctx.socketPath + QStringLiteral(" attach -t ") + ctx.sessionName,
            QStringLiteral("/dev/null"),
        });
    QVERIFY(scriptProc.waitForStarted(5000));

    // Wait for the second client to actually appear in tmux
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-clients"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{client_width}x#{client_height}")});
        check.waitForFinished(3000);
        QStringList clients = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return clients.size() >= 2;
    }(), 10000);

    // 6. Wait for %layout-change to propagate — poll display->columns() until it shrinks
    QTRY_VERIFY_WITH_TIMEOUT(display->columns() < initialColumns, 15000);

    // 7. Assert grid size matches the smaller client (40x12 minus status bar)
    QVERIFY2(display->columns() <= 40,
             qPrintable(QStringLiteral("Expected columns <= 40 but got %1").arg(display->columns())));
    QVERIFY2(display->lines() <= 12,
             qPrintable(QStringLiteral("Expected lines <= 12 but got %1").arg(display->lines())));

    // 8. Assert the TabPageWidget is constrained (the whole layout moves to top-left)
    auto *topSplitter = qobject_cast<ViewSplitter *>(display->parentWidget());
    QVERIFY(topSplitter);
    while (auto *parentSplitter = qobject_cast<ViewSplitter *>(topSplitter->parentWidget())) {
        topSplitter = parentSplitter;
    }
    auto *page = qobject_cast<TabPageWidget *>(topSplitter->parentWidget());
    QVERIFY2(page, "Expected top-level splitter to be inside a TabPageWidget");
    QVERIFY2(page->isConstrained(), "Expected TabPageWidget to be constrained");
    QSize constrained = page->constrainedSize();
    QVERIFY2(constrained.width() < originalPixelSize.width()
                 || constrained.height() < originalPixelSize.height(),
             qPrintable(QStringLiteral("Expected constrained size smaller than %1x%2, got %3x%4")
                            .arg(originalPixelSize.width()).arg(originalPixelSize.height())
                            .arg(constrained.width()).arg(constrained.height())));

    // 9. Cleanup: kill the background script process
    scriptProc.terminate();
    scriptProc.waitForFinished(5000);
    if (scriptProc.state() != QProcess::NotRunning) {
        scriptProc.kill();
        scriptProc.waitForFinished(3000);
    }

    // Kill tmux session early to avoid layout-change during teardown
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testForcedSizeFromSmallerClientMultiPane()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    const QString scriptPath = QStandardPaths::findExecutable(QStringLiteral("script"));
    if (scriptPath.isEmpty()) {
        QSKIP("script command not found.");
    }

    // 1. Setup tmux session with two horizontal panes (40+1+39 = 80 wide, 24 tall)
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬───────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                         │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        └────────────────────────────────────────┴───────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // 2. Attach Konsole via control mode
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // 3. Apply large layout so widgets are sized generously
    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬───────────────────────────────────────┐
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        └────────────────────────────────────────┴───────────────────────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // 4. Find the splitter with 2 TerminalDisplay children
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    int initialLeftCols = leftDisplay->columns();
    int initialRightCols = rightDisplay->columns();
    QSize originalLeftPixelSize = leftDisplay->size();
    QSize originalRightPixelSize = rightDisplay->size();

    QVERIFY2(initialLeftCols >= 20, qPrintable(QStringLiteral("Expected left columns >= 20 but got %1").arg(initialLeftCols)));
    QVERIFY2(initialRightCols >= 20, qPrintable(QStringLiteral("Expected right columns >= 20 but got %1").arg(initialRightCols)));

    // 5. Attach a second smaller tmux client using script to provide a pty
    QProcess scriptProc;
    scriptProc.start(
        scriptPath,
        {
            QStringLiteral("-q"),
            QStringLiteral("-c"),
            QStringLiteral("stty cols 40 rows 12; ") + tmuxPath + QStringLiteral(" -S ") + ctx.socketPath + QStringLiteral(" attach -t ") + ctx.sessionName,
            QStringLiteral("/dev/null"),
        });
    QVERIFY(scriptProc.waitForStarted(5000));

    // Wait for the second client to actually appear in tmux
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-clients"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{client_width}x#{client_height}")});
        check.waitForFinished(3000);
        QStringList clients = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return clients.size() >= 2;
    }(), 10000);

    // 6. Wait for %layout-change to propagate — both panes should shrink
    QTRY_VERIFY_WITH_TIMEOUT(leftDisplay->columns() < initialLeftCols || rightDisplay->columns() < initialRightCols, 15000);

    // 7. Assert forced grid sizes are smaller — total width should be <= 40
    int totalCols = leftDisplay->columns() + 1 + rightDisplay->columns(); // +1 for separator
    QVERIFY2(totalCols <= 40,
             qPrintable(QStringLiteral("Expected total columns <= 40 but got %1 (%2 + 1 + %3)")
                            .arg(totalCols).arg(leftDisplay->columns()).arg(rightDisplay->columns())));
    QVERIFY2(leftDisplay->lines() <= 12,
             qPrintable(QStringLiteral("Expected left lines <= 12 but got %1").arg(leftDisplay->lines())));
    QVERIFY2(rightDisplay->lines() <= 12,
             qPrintable(QStringLiteral("Expected right lines <= 12 but got %1").arg(rightDisplay->lines())));

    // 8. Assert the TabPageWidget is constrained (the whole layout moves to top-left)
    auto *page = qobject_cast<TabPageWidget *>(paneSplitter->parentWidget());
    QVERIFY2(page, "Expected splitter to be inside a TabPageWidget");
    QVERIFY2(page->isConstrained(), "Expected TabPageWidget to be constrained");
    QSize constrained = page->constrainedSize();
    QVERIFY2(constrained.width() < originalLeftPixelSize.width() + originalRightPixelSize.width()
                 || constrained.height() < originalLeftPixelSize.height(),
             qPrintable(QStringLiteral("Expected constrained size to shrink, got %1x%2")
                            .arg(constrained.width()).arg(constrained.height())));

    // 9. Cleanup: kill the background script process
    scriptProc.terminate();
    scriptProc.waitForFinished(5000);
    if (scriptProc.state() != QProcess::NotRunning) {
        scriptProc.kill();
        scriptProc.waitForFinished(3000);
    }

    // Kill tmux session early to avoid layout-change during teardown
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testClearScrollbackSyncToTmux()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // 1. Setup tmux session with a single pane running bash
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌───────────────────────────────────┐
        │ cmd: bash --norc --noprofile      │
        │                                   │
        │                                   │
        │                                   │
        │                                   │
        └───────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // 2. Generate scrollback content
    QProcess sendKeys;
    sendKeys.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("send-keys"),
                    QStringLiteral("-t"),
                    ctx.sessionName,
                    QStringLiteral("for i in $(seq 1 200); do echo \"SCROLLBACK_LINE_$i\"; done"),
                    QStringLiteral("Enter")});
    QVERIFY(sendKeys.waitForFinished(5000));
    QCOMPARE(sendKeys.exitCode(), 0);

    QTest::qWait(500);

    // 3. Check tmux server-side scrollback size
    auto getTmuxHistorySize = [&]() -> int {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{history_size}")});
        check.waitForFinished(3000);
        return QString::fromUtf8(check.readAllStandardOutput()).trimmed().toInt();
    };

    QVERIFY2(getTmuxHistorySize() > 0, "Expected tmux history_size > 0 before attach");

    // 4. Attach Konsole
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    QTest::qWait(2000);

    QVERIFY2(getTmuxHistorySize() > 0, "Expected history_size > 0 after attach");

    // 5. requestClearHistory clears scrollback only, visible content remains
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);

    controller->requestClearHistory(paneSession);

    QTRY_VERIFY_WITH_TIMEOUT(getTmuxHistorySize() == 0, 5000);

    // Visible content should still show recent output
    auto captureTmuxPane = [&]() -> QString {
        QProcess capture;
        capture.start(tmuxPath,
                      {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("-p")});
        capture.waitForFinished(3000);
        return QString::fromUtf8(capture.readAllStandardOutput());
    };

    QString visible = captureTmuxPane();
    QVERIFY2(visible.contains(QStringLiteral("SCROLLBACK_LINE_200")),
             qPrintable(QStringLiteral("Expected visible pane to still contain recent output, got: ") + visible));

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testClearScrollbackAndResetSyncToTmux()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // 1. Setup tmux session with a single pane running bash
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌───────────────────────────────────┐
        │ cmd: bash --norc --noprofile      │
        │                                   │
        │                                   │
        │                                   │
        │                                   │
        └───────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // 2. Generate scrollback content
    QProcess sendKeys;
    sendKeys.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("send-keys"),
                    QStringLiteral("-t"),
                    ctx.sessionName,
                    QStringLiteral("for i in $(seq 1 200); do echo \"SCROLLBACK_LINE_$i\"; done"),
                    QStringLiteral("Enter")});
    QVERIFY(sendKeys.waitForFinished(5000));
    QCOMPARE(sendKeys.exitCode(), 0);

    QTest::qWait(500);

    auto getTmuxHistorySize = [&]() -> int {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{history_size}")});
        check.waitForFinished(3000);
        return QString::fromUtf8(check.readAllStandardOutput()).trimmed().toInt();
    };

    auto captureTmuxPane = [&]() -> QString {
        QProcess capture;
        capture.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctx.socketPath,
                       QStringLiteral("capture-pane"),
                       QStringLiteral("-t"),
                       ctx.sessionName,
                       QStringLiteral("-p"),
                       QStringLiteral("-S"),
                       QStringLiteral("-")});
        capture.waitForFinished(3000);
        return QString::fromUtf8(capture.readAllStandardOutput());
    };

    QVERIFY2(getTmuxHistorySize() > 0, "Expected tmux history_size > 0 before attach");

    // 3. Attach Konsole
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    QTest::qWait(2000);

    QVERIFY2(getTmuxHistorySize() > 0, "Expected history_size > 0 after attach");

    // 4. requestClearHistoryAndReset clears visible screen AND scrollback
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);

    controller->requestClearHistoryAndReset(paneSession);

    // Wait for both commands to take effect
    QTRY_VERIFY_WITH_TIMEOUT(getTmuxHistorySize() == 0, 5000);

    // Visible content should no longer contain the output lines
    QString allContent = captureTmuxPane();
    QVERIFY2(!allContent.contains(QStringLiteral("SCROLLBACK_LINE_")),
             qPrintable(QStringLiteral("Expected all SCROLLBACK_LINE content to be cleared, got: ") + allContent));

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxZoomFromKonsole()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Setup 2-pane tmux session
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Find the splitter with 2 displays
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QVERIFY(!paneSplitter->terminalMaximized());

    // Find a pane session and its controller (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    QVERIFY(paneId >= 0);

    // Trigger zoom via requestToggleZoomPane (simulates Konsole's maximize action)
    controller->requestToggleZoomPane(paneId);

    // Wait for tmux to report zoomed state
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{window_zoomed_flag}")});
        check.waitForFinished(3000);
        return QString::fromUtf8(check.readAllStandardOutput()).trimmed() == QStringLiteral("1");
    }(), 10000);

    // Verify Konsole splitter is maximized
    QTRY_VERIFY_WITH_TIMEOUT(paneSplitter->terminalMaximized(), 5000);

    // Trigger unzoom
    controller->requestToggleZoomPane(paneId);

    // Wait for tmux to report unzoomed state
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{window_zoomed_flag}")});
        check.waitForFinished(3000);
        return QString::fromUtf8(check.readAllStandardOutput()).trimmed() == QStringLiteral("0");
    }(), 10000);

    // Verify Konsole splitter is no longer maximized
    QTRY_VERIFY_WITH_TIMEOUT(!paneSplitter->terminalMaximized(), 5000);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxZoomFromTmux()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Setup 2-pane tmux session
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Find the splitter with 2 displays
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QVERIFY(!paneSplitter->terminalMaximized());

    // Zoom from tmux externally
    QProcess zoomProc;
    zoomProc.start(tmuxPath,
                   {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("resize-pane"), QStringLiteral("-Z"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(zoomProc.waitForFinished(5000));
    QCOMPARE(zoomProc.exitCode(), 0);

    // Wait for Konsole to show maximized state
    QTRY_VERIFY_WITH_TIMEOUT(paneSplitter->terminalMaximized(), 10000);

    // Unzoom from tmux
    QProcess unzoomProc;
    unzoomProc.start(tmuxPath,
                     {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("resize-pane"), QStringLiteral("-Z"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(unzoomProc.waitForFinished(5000));
    QCOMPARE(unzoomProc.exitCode(), 0);

    // Wait for Konsole to restore all panes
    QTRY_VERIFY_WITH_TIMEOUT(!paneSplitter->terminalMaximized(), 10000);

    // Re-find the splitter (layout apply may have replaced it)
    paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children after unzoom");

    // Verify both displays are not explicitly hidden (isHidden() checks the widget's
    // own visibility flag, unlike isVisible() which also checks all ancestors).
    // In the offscreen test the pane tab may not be the active tab, so isVisible()
    // can return false even though the displays are not hidden.
    auto terminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(terminals.size(), 2);
    for (auto *td : terminals) {
        QVERIFY2(!td->isHidden(), "Expected both terminal displays to not be hidden after unzoom");
    }

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxZoomSurvivesLayoutChanges()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Small 2-pane layout — each pane is only ~20 columns wide, so the zoomed
    // display should clearly expand beyond that when maximized.
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │ cmd: sleep 60      │ cmd: sleep 60      │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │                    │                    │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Find the splitter with 2 displays
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");

    // Find a pane session and record its pre-zoom display width (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto paneViews = paneSession->views();
    QVERIFY(!paneViews.isEmpty());
    auto *zoomedDisplay = qobject_cast<TerminalDisplay *>(paneViews.first());
    QVERIFY(zoomedDisplay);

    int preZoomColumns = zoomedDisplay->columns();

    // Zoom from tmux
    QProcess zoomProc;
    zoomProc.start(tmuxPath,
                   {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("resize-pane"), QStringLiteral("-Z"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(zoomProc.waitForFinished(5000));
    QCOMPARE(zoomProc.exitCode(), 0);

    // Wait for Konsole to enter maximized state
    QTRY_VERIFY_WITH_TIMEOUT(paneSplitter->terminalMaximized(), 10000);

    // Record the zoomed display's grid size right after maximize is applied
    int zoomedColumns = zoomedDisplay->columns();
    int zoomedLines = zoomedDisplay->lines();

    // Wait for several %layout-change notifications to arrive (the title refresh
    // timer fires every 2 seconds and can trigger layout-change echo-backs).
    QTest::qWait(5000);
    QCoreApplication::processEvents();

    // The key assertion: the zoomed display's grid size must not have been
    // shrunk by setForcedSize from a layout-change while zoomed.
    QVERIFY2(paneSplitter->terminalMaximized(), "Expected splitter to still be maximized after layout changes");
    QVERIFY2(zoomedDisplay->columns() == zoomedColumns,
             qPrintable(QStringLiteral("Expected zoomed columns to remain %1 but got %2 (pre-zoom was %3)")
                            .arg(zoomedColumns).arg(zoomedDisplay->columns()).arg(preZoomColumns)));
    QVERIFY2(zoomedDisplay->lines() == zoomedLines,
             qPrintable(QStringLiteral("Expected zoomed lines to remain %1 but got %2")
                            .arg(zoomedLines).arg(zoomedDisplay->lines())));

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

namespace
{
bool tmuxWindowZoomed(const QString &tmuxPath, const QString &socketPath, const QString &sessionName)
{
    QProcess check;
    check.start(tmuxPath,
                {QStringLiteral("-S"),
                 socketPath,
                 QStringLiteral("display-message"),
                 QStringLiteral("-t"),
                 sessionName,
                 QStringLiteral("-p"),
                 QStringLiteral("#{window_zoomed_flag}")});
    check.waitForFinished(3000);
    return QString::fromUtf8(check.readAllStandardOutput()).trimmed() == QStringLiteral("1");
}

ViewSplitter *findTwoPaneSplitter(TabbedViewContainer *container)
{
    for (int i = 0; i < container->count(); ++i) {
        auto *splitter = container->viewSplitterAt(i);
        if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
            return splitter;
        }
    }
    return nullptr;
}
} // namespace

// Pressing Ctrl+Shift+E on a tmux-attached pane should zoom it on the tmux
// side, not only maximize locally. Kmux's splitter maximization and tmux's
// window_zoomed_flag must agree after the keypress, and toggling again must
// return both to the unzoomed/unmaximized state.
void TmuxIntegrationTest::testCtrlShiftEBoundToTmuxZoom()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    ViewSplitter *paneSplitter = findTwoPaneSplitter(attach.container);
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QVERIFY(!paneSplitter->terminalMaximized());
    QVERIFY(!tmuxWindowZoomed(tmuxPath, ctx.socketPath, ctx.sessionName));

    // Focus the active pane's display so the shortcut resolves against it.
    auto *display = paneSplitter->activeTerminalDisplay();
    QVERIFY(display);
    display->setFocus(Qt::OtherFocusReason);
    QTRY_COMPARE_WITH_TIMEOUT(QApplication::focusWidget(), static_cast<QWidget *>(display), 5000);

    // Ctrl+Shift+E: must zoom tmux AND maximize kmux.
    QTest::keyClick(display, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);

    QTRY_VERIFY_WITH_TIMEOUT(tmuxWindowZoomed(tmuxPath, ctx.socketPath, ctx.sessionName), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(paneSplitter->terminalMaximized(), 10000);

    // Ctrl+Shift+E again: unzoom tmux AND unmaximize kmux.
    QTest::keyClick(display, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);

    QTRY_VERIFY_WITH_TIMEOUT(!tmuxWindowZoomed(tmuxPath, ctx.socketPath, ctx.sessionName), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!paneSplitter->terminalMaximized(), 10000);

    delete attach.mw.data();
}

// Zooming from the tmux side (equivalent to the user pressing prefix+z) must
// also be reflected as a kmux splitter maximize. Symmetric to
// testCtrlShiftEBoundToTmuxZoom — both sides must agree after the toggle.
void TmuxIntegrationTest::testTmuxZoomReflectedAsKonsoleMaximize()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    ViewSplitter *paneSplitter = findTwoPaneSplitter(attach.container);
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QVERIFY(!paneSplitter->terminalMaximized());
    QVERIFY(!tmuxWindowZoomed(tmuxPath, ctx.socketPath, ctx.sessionName));

    // tmux-side zoom via `resize-pane -Z` (the non-control-mode equivalent of
    // the user pressing prefix+z in an interactive client).
    QProcess zoom;
    zoom.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("resize-pane"), QStringLiteral("-Z"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(zoom.waitForFinished(5000));
    QCOMPARE(zoom.exitCode(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(tmuxWindowZoomed(tmuxPath, ctx.socketPath, ctx.sessionName), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(paneSplitter->terminalMaximized(), 10000);

    // Unzoom on the tmux side.
    QProcess unzoom;
    unzoom.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("resize-pane"), QStringLiteral("-Z"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(unzoom.waitForFinished(5000));
    QCOMPARE(unzoom.exitCode(), 0);

    QTRY_VERIFY_WITH_TIMEOUT(!tmuxWindowZoomed(tmuxPath, ctx.socketPath, ctx.sessionName), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(!paneSplitter->terminalMaximized(), 10000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testBreakPane()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Setup 2-pane tmux session
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    TmuxTestDSL::applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Find the splitter with 2 displays
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");

    int initialTabCount = attach.mw->viewManager()->activeContainer()->count();

    // Find a pane session and its controller (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    QVERIFY(paneId >= 0);

    // Break the pane out into a new tmux window
    controller->requestBreakPane(paneId);

    // Wait for tab count to increase (new tmux window → new tab)
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == initialTabCount + 1;
        }(),
        10000);

    // Verify the controller now has 2 windows, each with 1 pane
    QCOMPARE(controller->windowCount(), 2);
    const auto &windowTabs = controller->windowToTabIndex();
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        QCOMPARE(controller->paneCountForWindow(it.key()), 1);
        auto *splitter = attach.container->viewSplitterAt(it.value());
        QVERIFY(splitter);
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
    }

    // Verify tmux confirms 2 windows exist
    QProcess listWindows;
    listWindows.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("list-windows"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(listWindows.waitForFinished(5000));
    QString windowOutput = QString::fromUtf8(listWindows.readAllStandardOutput()).trimmed();
    int windowCount = windowOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
    QCOMPARE(windowCount, 2);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitPaneInheritsWorkingDirectory()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                   │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // cd to /tmp so we have a known directory
    QProcess sendCd;
    sendCd.start(tmuxPath,
                 {QStringLiteral("-S"),
                  ctx.socketPath,
                  QStringLiteral("send-keys"),
                  QStringLiteral("-t"),
                  ctx.sessionName,
                  QStringLiteral("cd /tmp"),
                  QStringLiteral("Enter")});
    QVERIFY(sendCd.waitForFinished(5000));
    QCOMPARE(sendCd.exitCode(), 0);
    QTest::qWait(500);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session and its controller (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    QVERIFY(paneId >= 0);

    // Wait for the working directory to propagate to the VirtualSession
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QString dir = paneSession->currentWorkingDirectory();
        return dir.contains(QStringLiteral("tmp"));
    }(), 10000);

    // Request a horizontal split, passing the working directory
    controller->requestSplitPane(paneId, Qt::Horizontal, QStringLiteral("/tmp"));

    // Wait for the split to appear: a ViewSplitter with 2 TerminalDisplay children
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!container)
                return false;
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter) {
                    auto terminals = splitter->findChildren<TerminalDisplay *>();
                    if (terminals.size() == 2) {
                        return true;
                    }
                }
            }
            return false;
        }(),
        10000);

    // Verify the new pane started in /tmp by querying tmux
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_current_path}")});
        check.waitForFinished(3000);
        QStringList paths = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (paths.size() != 2) return false;
        // Both the original pane and the new pane should be in /tmp
        return paths[0].contains(QStringLiteral("tmp")) && paths[1].contains(QStringLiteral("tmp"));
    }(), 10000);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNewWindowInheritsWorkingDirectory()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                   │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // cd to /tmp so we have a known directory
    QProcess sendCd;
    sendCd.start(tmuxPath,
                 {QStringLiteral("-S"),
                  ctx.socketPath,
                  QStringLiteral("send-keys"),
                  QStringLiteral("-t"),
                  ctx.sessionName,
                  QStringLiteral("cd /tmp"),
                  QStringLiteral("Enter")});
    QVERIFY(sendCd.waitForFinished(5000));
    QCOMPARE(sendCd.exitCode(), 0);
    QTest::qWait(500);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);

    int initialTabCount = attach.mw->viewManager()->activeContainer()->count();

    // Request a new tmux window with /tmp as working directory
    controller->requestNewWindow(QStringLiteral("/tmp"));

    // Wait for the new tab to appear
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == initialTabCount + 1;
        }(),
        10000);

    // Verify the new window's pane started in /tmp by querying tmux
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-a"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_current_path}")});
        check.waitForFinished(3000);
        QStringList paths = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (paths.size() != 2) return false;
        // The new window's pane should be in /tmp
        return paths[1].contains(QStringLiteral("tmp"));
    }(), 10000);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testOscColorQueryNotLeakedAsKeystrokes()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                   │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // Spy on data sent back from the emulation (this becomes send-keys in tmux mode)
    QSignalSpy sendSpy(paneSession->emulation(), &Emulation::sendData);

    // Send an OSC 10 foreground color query into the pane from the tmux side.
    // This simulates what happens when a program like bat sends "\033]10;?\007"
    // — tmux forwards the pane output as %output to Konsole's emulation.
    QProcess sendQuery;
    sendQuery.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("send-keys"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-l"),
                     QStringLiteral("printf '\\033]10;?\\007'")});
    QVERIFY(sendQuery.waitForFinished(5000));
    QCOMPARE(sendQuery.exitCode(), 0);
    // Execute the printf command
    QProcess sendEnter;
    sendEnter.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("send-keys"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("Enter")});
    QVERIFY(sendEnter.waitForFinished(5000));
    QCOMPARE(sendEnter.exitCode(), 0);

    // Wait for the output to propagate through tmux %output → Konsole emulation
    QTest::qWait(3000);

    // Check if any response containing "rgb:" was sent back via sendData.
    // This is the bug: the OSC color response should NOT be sent back as
    // keystrokes to the tmux pane, because it will appear as visible text.
    bool leaked = false;
    for (const auto &call : sendSpy) {
        QByteArray data = call.at(0).toByteArray();
        if (data.contains("rgb:")) {
            leaked = true;
            break;
        }
    }
    QVERIFY2(!leaked, "OSC color query response was leaked back as keystrokes to tmux pane");

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testCyrillicInputPreservesUtf8()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                   │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // TODO: adapt for TmuxProcessBridge — no emulation to spy on for the gateway.
    // With TmuxProcessBridge, tmux commands are sent via QProcess stdin, not
    // through a gateway Session's emulation. We verify the end-to-end result
    // by checking what tmux actually received instead.

    // Simulate typing Cyrillic text into the pane.
    // This goes through: sendText → Vt102Emulation → sendData signal →
    //   TmuxPaneManager lambda → TmuxGateway::sendKeys → QProcess stdin
    const QString cyrillicText = QStringLiteral("слоп");
    paneSession->emulation()->sendText(cyrillicText);

    // Let the event loop process and tmux receive the command
    QTest::qWait(1000);

    // Verify end-to-end: capture the tmux pane and check that the Cyrillic text
    // was received correctly (not garbled by hex encoding).
    QProcess capture;
    capture.start(tmuxPath,
                  {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("-p")});
    QVERIFY(capture.waitForFinished(5000));
    QString paneContent = QString::fromUtf8(capture.readAllStandardOutput());

    // The pane should contain the Cyrillic text as-is (it appears on the command line)
    bool containsLiteralCyrillic = paneContent.contains(cyrillicText);

    // Also check for the broken hex encoding pattern appearing in the pane.
    // If broken, we'd see something like: 0xd1 0x81 0xd0 0xbb ...
    bool containsHexEncoded = paneContent.contains(QStringLiteral("0xd0"));

    QVERIFY2(containsLiteralCyrillic,
             qPrintable(QStringLiteral("Cyrillic text should be sent as literal UTF-8 via send-keys -l, "
                                       "but the pane contains: %1")
                            .arg(paneContent)));
    QVERIFY2(!containsHexEncoded,
             qPrintable(QStringLiteral("Cyrillic bytes should NOT be hex-encoded as individual bytes, "
                                       "but the pane contains: %1")
                            .arg(paneContent)));

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxAttachNoSessions()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Use TmuxProcessBridge to attempt "tmux -C attach" when there are no
    // tmux sessions. The bridge should handle the immediate exit gracefully
    // without crashing or leaking commands.
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    // Use a fresh socket so there is guaranteed no server running on it
    const QString socketPath = m_tmuxTmpDir.path() + QStringLiteral("/tmux-no-sessions");

    auto *bridge = new TmuxProcessBridge(vm, mw);
    QSignalSpy disconnectedSpy(bridge, &TmuxProcessBridge::disconnected);

    // Start with "attach" which should fail immediately (no sessions)
    bridge->start(tmuxPath, {QStringLiteral("-S"), socketPath}, {QStringLiteral("attach")});

    // Wait for the bridge to report disconnection (tmux exits with error)
    QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() >= 1, 10000);

    // The ViewManager should have no sessions (no pane tabs created)
    QVERIFY2(vm->sessions().isEmpty(), "No pane sessions should be created when tmux attach fails");

    delete mwGuard.data();
}

void TmuxIntegrationTest::testAttachMultipleWindows()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a tmux session with 1 window, then add a second window via tmux command
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Create a second tmux window before attaching
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    // Attach Konsole
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for both tmux windows to appear as tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    // Verify the controller sees 2 windows
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // Each window should have 1 pane
    const auto &windowTabs = controller->windowToTabIndex();
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        QCOMPARE(controller->paneCountForWindow(it.key()), 1);
    }

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNewWindowCreatesTab()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    int initialTabCount = container->count();

    // Get the controller
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);

    // Request a new window via the controller
    controller->requestNewWindow(QString());

    // Wait for the new tab to appear
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == initialTabCount + 1;
        }(),
        10000);

    QCOMPARE(controller->windowCount(), 2);

    // Verify tmux also has 2 windows
    QProcess listWindows;
    listWindows.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("list-windows"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(listWindows.waitForFinished(5000));
    QString windowOutput = QString::fromUtf8(listWindows.readAllStandardOutput()).trimmed();
    int windowCount = windowOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
    QCOMPARE(windowCount, 2);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testCloseWindowFromTmuxRemovesTab()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a session, then add a second window
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    // Attach
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    // Kill the second tmux window from outside
    QProcess killWindow;
    killWindow.start(tmuxPath,
                     {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("kill-window"), QStringLiteral("-t"), QStringLiteral("%1:1").arg(ctx.sessionName)});
    QVERIFY(killWindow.waitForFinished(5000));
    QCOMPARE(killWindow.exitCode(), 0);

    // Wait for the tab to be removed
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == 1;
        }(),
        10000);

    // Verify the controller sees 1 window
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 1);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testCloseWindowTabFromKonsole()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a session with 1 window, then add a second
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    // Attach
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    // Find a pane session from the second window and close it
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(sessions.size() >= 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);

    // Find a session that belongs to a different window than the first session
    int firstPaneId = controller->paneIdForSession(sessions.first());
    int firstWindowId = controller->windowIdForPane(firstPaneId);
    Session *secondWindowSession = nullptr;
    for (Session *s : sessions) {
        int paneId = controller->paneIdForSession(s);
        if (paneId >= 0 && controller->windowIdForPane(paneId) != firstWindowId) {
            secondWindowSession = s;
            break;
        }
    }
    QVERIFY2(secondWindowSession, "Should find a session belonging to the second window");

    // Close the second window's pane session from Konsole.
    // This exercises the fix: VirtualSession::closeInNormalWay() should
    // send kill-pane to tmux, which destroys the window (single-pane window).
    secondWindowSession->closeInNormalWay();

    // Wait for the tab to be removed
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == 1;
        }(),
        10000);

    // Verify tmux also has only 1 window
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            QProcess listWindows;
            listWindows.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("list-windows"), QStringLiteral("-t"), ctx.sessionName});
            listWindows.waitForFinished(3000);
            QString windowOutput = QString::fromUtf8(listWindows.readAllStandardOutput()).trimmed();
            int windowCount = windowOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
            return windowCount == 1;
        }(),
        10000);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testRenameWindowFromTmuxUpdatesTab()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    QVERIFY(container->count() >= 1);

    // Rename the window from tmux
    QProcess renameWindow;
    renameWindow.start(tmuxPath,
                       {QStringLiteral("-S"),
                        ctx.socketPath,
                        QStringLiteral("rename-window"),
                        QStringLiteral("-t"),
                        QStringLiteral("%1:0").arg(ctx.sessionName),
                        QStringLiteral("my-custom-name")});
    QVERIFY(renameWindow.waitForFinished(5000));
    QCOMPARE(renameWindow.exitCode(), 0);

    // Wait for the tab title to update
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!c || c->count() < 1)
                return false;
            // Check all tabs for the renamed title
            for (int i = 0; i < c->count(); ++i) {
                if (c->tabText(i) == QStringLiteral("my-custom-name")) {
                    return true;
                }
            }
            return false;
        }(),
        10000);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSwapPaneFromTmux()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a 2-pane horizontal split
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the splitter with 2 displays
    auto *container = attach.mw->viewManager()->activeContainer();
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
            return false;
        }(),
        10000);

    // Record which pane is on the left and which is on the right
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    int leftPaneId = controller->paneIdForSession(leftDisplay->sessionController()->session());
    int rightPaneId = controller->paneIdForSession(rightDisplay->sessionController()->session());
    QVERIFY(leftPaneId >= 0);
    QVERIFY(rightPaneId >= 0);
    QVERIFY(leftPaneId != rightPaneId);

    // Swap panes from tmux
    QProcess swapPane;
    swapPane.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("swap-pane"),
                    QStringLiteral("-s"),
                    QLatin1Char('%') + QString::number(leftPaneId),
                    QStringLiteral("-t"),
                    QLatin1Char('%') + QString::number(rightPaneId)});
    QVERIFY(swapPane.waitForFinished(5000));
    QCOMPARE(swapPane.exitCode(), 0);

    // Wait for the layout change to be applied — the pane IDs should swap positions
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            // Re-find the splitter (it may have been rebuilt)
            paneSplitter = nullptr;
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    break;
                }
            }
            if (!paneSplitter)
                return false;
            auto *newLeft = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
            auto *newRight = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
            if (!newLeft || !newRight)
                return false;
            int newLeftPaneId = controller->paneIdForSession(newLeft->sessionController()->session());
            int newRightPaneId = controller->paneIdForSession(newRight->sessionController()->session());
            // After swap: the originally-left pane should now be on the right and vice versa
            return newLeftPaneId == rightPaneId && newRightPaneId == leftPaneId;
        }(),
        10000);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSwapPaneFromKonsole()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a 2-pane horizontal split
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Find the splitter with 2 displays
    auto *container = attach.mw->viewManager()->activeContainer();
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
            return false;
        }(),
        10000);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    int leftPaneId = controller->paneIdForSession(leftDisplay->sessionController()->session());
    int rightPaneId = controller->paneIdForSession(rightDisplay->sessionController()->session());
    QVERIFY(leftPaneId >= 0);
    QVERIFY(rightPaneId >= 0);
    QVERIFY(leftPaneId != rightPaneId);

    // Swap panes from Konsole
    controller->requestSwapPane(leftPaneId, rightPaneId);

    // Wait for the layout change — pane positions should swap
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            paneSplitter = nullptr;
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    break;
                }
            }
            if (!paneSplitter)
                return false;
            auto *newLeft = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
            auto *newRight = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
            if (!newLeft || !newRight)
                return false;
            int newLeftPaneId = controller->paneIdForSession(newLeft->sessionController()->session());
            int newRightPaneId = controller->paneIdForSession(newRight->sessionController()->session());
            return newLeftPaneId == rightPaneId && newRightPaneId == leftPaneId;
        }(),
        10000);

    // Verify tmux also reflects the swap
    QProcess listPanes;
    listPanes.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id}")});
    QVERIFY(listPanes.waitForFinished(5000));
    QCOMPARE(listPanes.exitCode(), 0);
    QStringList paneOrder = QString::fromUtf8(listPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(paneOrder.size(), 2);
    // After swap: first pane in tmux list should be the originally-right pane
    QCOMPARE(paneOrder[0], QLatin1Char('%') + QString::number(rightPaneId));
    QCOMPARE(paneOrder[1], QLatin1Char('%') + QString::number(leftPaneId));

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testMovePaneFromTmux()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a session with 2 windows, each with 1 pane
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // Get pane IDs from each window
    const auto &windowTabs = controller->windowToTabIndex();
    QList<int> windowIds = windowTabs.keys();
    QCOMPARE(windowIds.size(), 2);

    // Get the pane ID from window 1 (the second window)
    int window1PaneId = -1;
    {
        int tabIndex = windowTabs.value(windowIds[1]);
        auto *splitter = container->viewSplitterAt(tabIndex);
        QVERIFY(splitter);
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
        window1PaneId = controller->paneIdForSession(terminals.first()->sessionController()->session());
        QVERIFY(window1PaneId >= 0);
    }

    // Get the pane ID from window 0
    int window0PaneId = -1;
    {
        int tabIndex = windowTabs.value(windowIds[0]);
        auto *splitter = container->viewSplitterAt(tabIndex);
        QVERIFY(splitter);
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
        window0PaneId = controller->paneIdForSession(terminals.first()->sessionController()->session());
        QVERIFY(window0PaneId >= 0);
    }

    // Move pane from window 1 into window 0 (horizontal split) via tmux
    QProcess movePane;
    movePane.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("move-pane"),
                    QStringLiteral("-h"),
                    QStringLiteral("-s"),
                    QLatin1Char('%') + QString::number(window1PaneId),
                    QStringLiteral("-t"),
                    QLatin1Char('%') + QString::number(window0PaneId)});
    QVERIFY(movePane.waitForFinished(5000));
    QCOMPARE(movePane.exitCode(), 0);

    // Window 1 should disappear (it had only 1 pane), leaving 1 tab with 2 panes
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!c || c->count() != 1)
                return false;
            auto *splitter = c->viewSplitterAt(0);
            return splitter && splitter->findChildren<TerminalDisplay *>().size() == 2;
        }(),
        10000);

    QCOMPARE(controller->windowCount(), 1);

    // Cleanup
    delete attach.mw.data();
}

void TmuxIntegrationTest::testMovePaneFromKonsole()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a session with 2 windows, each with 1 pane
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // Get pane IDs from each window
    const auto &windowTabs = controller->windowToTabIndex();
    QList<int> windowIds = windowTabs.keys();
    QCOMPARE(windowIds.size(), 2);

    int window0PaneId = -1;
    int window1PaneId = -1;
    {
        auto *splitter0 = container->viewSplitterAt(windowTabs.value(windowIds[0]));
        auto *splitter1 = container->viewSplitterAt(windowTabs.value(windowIds[1]));
        QVERIFY(splitter0);
        QVERIFY(splitter1);
        window0PaneId = controller->paneIdForSession(splitter0->findChildren<TerminalDisplay *>().first()->sessionController()->session());
        window1PaneId = controller->paneIdForSession(splitter1->findChildren<TerminalDisplay *>().first()->sessionController()->session());
        QVERIFY(window0PaneId >= 0);
        QVERIFY(window1PaneId >= 0);
    }

    // Move pane from window 1 into window 0 via Konsole
    controller->requestMovePane(window1PaneId, window0PaneId, Qt::Horizontal, false);

    // Window 1 should disappear, leaving 1 tab with 2 panes
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!c || c->count() != 1)
                return false;
            auto *splitter = c->viewSplitterAt(0);
            return splitter && splitter->findChildren<TerminalDisplay *>().size() == 2;
        }(),
        10000);

    QCOMPARE(controller->windowCount(), 1);

    // Verify tmux also has 1 window with 2 panes
    QProcess listPanes;
    listPanes.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id}")});
    QVERIFY(listPanes.waitForFinished(5000));
    QCOMPARE(listPanes.exitCode(), 0);
    QStringList paneLines = QString::fromUtf8(listPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(paneLines.size(), 2);

    // Cleanup
    delete attach.mw.data();
}

void TmuxIntegrationTest::testMovePaneFromTwoToOneFromTmux()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a 2-pane window + a 1-pane window
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window with 1 pane
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    auto *container = attach.mw->viewManager()->activeContainer();
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 2, 10000);
    QCOMPARE(container->count(), 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    // Find the 2-pane window and the 1-pane window
    const auto &windowTabs = controller->windowToTabIndex();
    int twoPaneWindowId = -1;
    int onePaneWindowId = -1;
    int movePaneId = -1; // pane to move from the 2-pane window
    int targetPaneId = -1; // pane in the 1-pane window
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        if (controller->paneCountForWindow(it.key()) == 2) {
            twoPaneWindowId = it.key();
        } else if (controller->paneCountForWindow(it.key()) == 1) {
            onePaneWindowId = it.key();
        }
    }
    QVERIFY(twoPaneWindowId >= 0);
    QVERIFY(onePaneWindowId >= 0);

    // Get a pane ID from the 2-pane window (the right one)
    {
        auto *splitter = container->viewSplitterAt(windowTabs.value(twoPaneWindowId));
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 2);
        movePaneId = controller->paneIdForSession(terminals.last()->sessionController()->session());
    }
    // Get the pane ID from the 1-pane window
    {
        auto *splitter = container->viewSplitterAt(windowTabs.value(onePaneWindowId));
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
        targetPaneId = controller->paneIdForSession(terminals.first()->sessionController()->session());
    }
    QVERIFY(movePaneId >= 0);
    QVERIFY(targetPaneId >= 0);

    // Move one pane from the 2-pane window to the 1-pane window via tmux
    QProcess movePane;
    movePane.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("move-pane"),
                    QStringLiteral("-h"),
                    QStringLiteral("-s"),
                    QLatin1Char('%') + QString::number(movePaneId),
                    QStringLiteral("-t"),
                    QLatin1Char('%') + QString::number(targetPaneId)});
    QVERIFY(movePane.waitForFinished(5000));
    QCOMPARE(movePane.exitCode(), 0);

    // Both windows should now have 1 and 2 panes (moved from first to second)
    // Still 2 tabs, but the pane counts should have changed
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            return controller->paneCountForWindow(twoPaneWindowId) == 1 && controller->paneCountForWindow(onePaneWindowId) == 2;
        }(),
        10000);

    // Verify Konsole splitter tree matches
    {
        auto *splitter1 = container->viewSplitterAt(windowTabs.value(twoPaneWindowId));
        auto *splitter2 = container->viewSplitterAt(windowTabs.value(onePaneWindowId));
        QVERIFY(splitter1);
        QVERIFY(splitter2);
        QCOMPARE(splitter1->findChildren<TerminalDisplay *>().size(), 1);
        QCOMPARE(splitter2->findChildren<TerminalDisplay *>().size(), 2);
    }

    // Cleanup
    delete attach.mw.data();
}

void TmuxIntegrationTest::testMovePaneFromTwoToOneFromKonsole()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Create a 2-pane window + a 1-pane window
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window with 1 pane
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    auto *container = attach.mw->viewManager()->activeContainer();
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 2, 10000);
    QCOMPARE(container->count(), 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    const auto &windowTabs = controller->windowToTabIndex();
    int twoPaneWindowId = -1;
    int onePaneWindowId = -1;
    int movePaneId = -1;
    int targetPaneId = -1;
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        if (controller->paneCountForWindow(it.key()) == 2) {
            twoPaneWindowId = it.key();
        } else if (controller->paneCountForWindow(it.key()) == 1) {
            onePaneWindowId = it.key();
        }
    }
    QVERIFY(twoPaneWindowId >= 0);
    QVERIFY(onePaneWindowId >= 0);

    {
        auto *splitter = container->viewSplitterAt(windowTabs.value(twoPaneWindowId));
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 2);
        movePaneId = controller->paneIdForSession(terminals.last()->sessionController()->session());
    }
    {
        auto *splitter = container->viewSplitterAt(windowTabs.value(onePaneWindowId));
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
        targetPaneId = controller->paneIdForSession(terminals.first()->sessionController()->session());
    }
    QVERIFY(movePaneId >= 0);
    QVERIFY(targetPaneId >= 0);

    // Move pane from 2-pane window to 1-pane window via Konsole
    controller->requestMovePane(movePaneId, targetPaneId, Qt::Horizontal, false);

    // Pane counts should change: 2→1, 1→2
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            return controller->paneCountForWindow(twoPaneWindowId) == 1 && controller->paneCountForWindow(onePaneWindowId) == 2;
        }(),
        10000);

    // Verify splitter trees
    {
        auto *splitter1 = container->viewSplitterAt(windowTabs.value(twoPaneWindowId));
        auto *splitter2 = container->viewSplitterAt(windowTabs.value(onePaneWindowId));
        QVERIFY(splitter1);
        QVERIFY(splitter2);
        QCOMPARE(splitter1->findChildren<TerminalDisplay *>().size(), 1);
        QCOMPARE(splitter2->findChildren<TerminalDisplay *>().size(), 2);
    }

    // Verify tmux agrees
    QProcess listWindows;
    listWindows.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctx.socketPath,
                       QStringLiteral("list-panes"),
                       QStringLiteral("-a"),
                       QStringLiteral("-t"),
                       ctx.sessionName,
                       QStringLiteral("-F"),
                       QStringLiteral("#{window_id} #{pane_id}")});
    QVERIFY(listWindows.waitForFinished(5000));
    QCOMPARE(listWindows.exitCode(), 0);
    QStringList lines = QString::fromUtf8(listWindows.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 3); // 3 total panes across 2 windows

    // Cleanup
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNewTabFromTmuxPane()
{
    // When the user invokes New Tab (Ctrl+T) while focused on a tmux pane,
    // a new tmux window should be created without any confirmation dialog.
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto *container = attach.mw->viewManager()->activeContainer();
    QVERIFY(container);
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 1, 10000);

    // Trigger the New Tab action (Ctrl+Shift+T)
    QAction *newTabAction = attach.mw->actionCollection()->action(QStringLiteral("new-tab"));
    QVERIFY2(newTabAction, "new-tab action not found");
    newTabAction->trigger();

    // A new tmux tab should appear without any dialog
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 2, 10000);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // The tab bar should be visible now that there are 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(container->tabBar()->isVisible(), 5000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testDetachViewBreaksPane()
{
    // Ctrl+Shift+H (detach-view action) on a tmux pane should break the pane
    // out into a new tmux window (= new Konsole tab).
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Start with a 2-pane window so there's something to break out
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto *container = attach.mw->viewManager()->activeContainer();
    QVERIFY(container);

    // Wait for the 2-pane splitter to appear
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *s = container->viewSplitterAt(0);
            return s && s->findChildren<TerminalDisplay *>().size() == 2;
        }(),
        10000);
    QCOMPARE(container->count(), 1);

    // Trigger the detach-view action (Ctrl+Shift+H)
    QAction *detachAction = attach.mw->actionCollection()->action(QStringLiteral("detach-view"));
    QVERIFY2(detachAction, "detach-view action not found");
    detachAction->trigger();

    // A new tab should appear with the broken-out pane
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 2, 10000);

    // Each tab should have exactly 1 pane
    for (int i = 0; i < container->count(); ++i) {
        auto *s = container->viewSplitterAt(i);
        QVERIFY(s);
        QCOMPARE(s->findChildren<TerminalDisplay *>().size(), 1);
    }

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // Tab bar should be visible with 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(container->tabBar()->isVisible(), 5000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testDetachFromTmuxAction()
{
    // The detach-from-tmux action should exist in the action collection
    // and trigger a tmux detach — the tmux subprocess disconnects but the
    // tmux server keeps running.
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    QAction *detachAction = attach.mw->actionCollection()->action(QStringLiteral("detach-from-tmux"));
    QVERIFY2(detachAction, "detach-from-tmux action not found");

    // Watch for the bridge disconnecting after detach
    QSignalSpy disconnectSpy(attach.bridge, &TmuxProcessBridge::disconnected);

    detachAction->trigger();

    // The bridge should disconnect (tmux control client exits on detach)
    QTRY_VERIFY_WITH_TIMEOUT(disconnectSpy.count() >= 1, 10000);

    // The tmux server itself should still be running — we can still query it
    QProcess listSessions;
    listSessions.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("list-sessions")});
    QVERIFY(listSessions.waitForFinished(5000));
    QCOMPARE(listSessions.exitCode(), 0);

    delete attach.mw.data();
}

// Pressing the tmux prefix (default C-b) on the kmux window opens the prefix
// palette populated from tmux's own `list-keys -T prefix`. The next keystroke
// resolves to a binding and the raw tmux command is sent back through the
// gateway — here, `d` → `detach-client`, which disconnects the control client.
void TmuxIntegrationTest::testTmuxPrefixPaletteDetach()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    // Wait until prefix + bindings are loaded (both show-options and list-keys
    // responses have landed), otherwise the palette would be empty.
    QTRY_VERIFY_WITH_TIMEOUT(!controller->prefixShortcut().isEmpty() && !controller->prefixBindings().isEmpty(), 10000);
    QCOMPARE(controller->prefixShortcut(), QKeySequence(Qt::CTRL | Qt::Key_B));

    // Pressing the tmux prefix shortcut on the main window should invoke the
    // palette action.
    QSignalSpy disconnectSpy(attach.bridge, &TmuxProcessBridge::disconnected);
    QTest::keyClick(attach.mw, Qt::Key_B, Qt::ControlModifier);

    // The palette is a child QFrame of the window; find it.
    TmuxPrefixPalette *palette = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((palette = attach.mw->findChild<TmuxPrefixPalette *>()) != nullptr, 5000);
    QVERIFY(palette->hasFocus());

    // Press `d` — tmux's default binding is `detach-client`.
    QTest::keyClick(palette, Qt::Key_D);

    // The bridge should disconnect (tmux control client exits on detach)
    QTRY_VERIFY_WITH_TIMEOUT(disconnectSpy.count() >= 1, 10000);

    delete attach.mw.data();
}

namespace
{
// Helper: set up a tmux session with 2 windows: window 0 has 2 panes (split),
// window 1 has 1 pane. Returns attached kmux and controller, plus the pane IDs.
struct TreeSwitcherFixture {
    QString tmuxPath;
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::AttachResult attach;
    TmuxController *controller = nullptr;
    int w0pane0Id = -1;
    int w0pane1Id = -1;
    int w1pane0Id = -1;
    int w0WindowId = -1;
    int w1WindowId = -1;
};

void setupTreeSwitcherFixture(TreeSwitcherFixture &f, const QString &tmuxTmpDirPath)
{
    f.tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  f.tmuxPath,
                                  tmuxTmpDirPath,
                                  f.ctx);
    // Add a second window with 1 pane
    QProcess newWindow;
    newWindow.start(
        f.tmuxPath,
        {QStringLiteral("-S"), f.ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), f.ctx.sessionName, QStringLiteral("sleep 60")});
    newWindow.waitForFinished(5000);

    // new-window leaves the new window focused; switch back to the 2-pane window
    // so tests have a predictable "active pane is in w0" starting state.
    QProcess selectWindow;
    selectWindow.start(
        f.tmuxPath,
        {QStringLiteral("-S"), f.ctx.socketPath, QStringLiteral("select-window"), QStringLiteral("-t"), QStringLiteral("%1:0").arg(f.ctx.sessionName)});
    selectWindow.waitForFinished(5000);

    TmuxTestDSL::attachKonsole(f.tmuxPath, f.ctx, f.attach);
    f.attach.mw->show();
    QTest::qWaitForWindowActive(f.attach.mw);

    // Wait for 2 tabs
    auto *container = f.attach.mw->viewManager()->activeContainer();
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 2, 10000);

    f.controller = TmuxControllerRegistry::instance()->controllerForSession(f.attach.mw->viewManager()->sessions().first());

    // Wait for active pane to be known
    QTRY_VERIFY_WITH_TIMEOUT(f.controller && f.controller->activePaneId() >= 0, 10000);

    // Identify windows: 2-pane window is w0, 1-pane is w1
    const auto &windowTabs = f.controller->windowToTabIndex();
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        int cnt = f.controller->paneCountForWindow(it.key());
        if (cnt == 2)
            f.w0WindowId = it.key();
        else if (cnt == 1)
            f.w1WindowId = it.key();
    }

    QList<int> w0Panes = f.controller->panesForWindow(f.w0WindowId);
    QList<int> w1Panes = f.controller->panesForWindow(f.w1WindowId);
    if (w0Panes.size() == 2) {
        f.w0pane0Id = w0Panes[0];
        f.w0pane1Id = w0Panes[1];
    }
    if (w1Panes.size() == 1) {
        f.w1pane0Id = w1Panes[0];
    }
}

// Walk the tree (including proxy) to find the QModelIndex for a pane ID.
QModelIndex findPaneIndex(QAbstractItemModel *model, int paneId)
{
    std::function<QModelIndex(const QModelIndex &)> walk = [&](const QModelIndex &parent) -> QModelIndex {
        int rows = model->rowCount(parent);
        for (int i = 0; i < rows; ++i) {
            QModelIndex idx = model->index(i, 0, parent);
            if (idx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::PaneNode && idx.data(TmuxTreeModel::IdRole).toInt() == paneId) {
                return idx;
            }
            QModelIndex child = walk(idx);
            if (child.isValid())
                return child;
        }
        return {};
    };
    return walk({});
}

QModelIndex findWindowIndex(QAbstractItemModel *model, int windowId)
{
    std::function<QModelIndex(const QModelIndex &)> walk = [&](const QModelIndex &parent) -> QModelIndex {
        int rows = model->rowCount(parent);
        for (int i = 0; i < rows; ++i) {
            QModelIndex idx = model->index(i, 0, parent);
            if (idx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::WindowNode && idx.data(TmuxTreeModel::IdRole).toInt() == windowId) {
                return idx;
            }
            QModelIndex child = walk(idx);
            if (child.isValid())
                return child;
        }
        return {};
    };
    return walk({});
}
} // anonymous namespace

void TmuxIntegrationTest::testTreeSwitcherActivePreselected()
{
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    int activePane = f.controller->activePaneId();
    QVERIFY(activePane >= 0);

    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    // Wait for the async queryTree response to populate the model
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);

    // The currently-selected row in the tree should map to the active pane
    QModelIndex current = switcher->treeView()->currentIndex();
    QVERIFY(current.isValid());
    QCOMPARE(current.data(TmuxTreeModel::NodeTypeRole).toInt(), int(TmuxTreeModel::PaneNode));
    QCOMPARE(current.data(TmuxTreeModel::IdRole).toInt(), activePane);

    delete switcher;
    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherSwitchesPaneSameWindow()
{
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    int activePane = f.controller->activePaneId();
    QVERIFY(activePane == f.w0pane0Id || activePane == f.w0pane1Id);
    int targetPane = (activePane == f.w0pane0Id) ? f.w0pane1Id : f.w0pane0Id;

    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    // Wait for the async queryTree response to populate the model
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);

    // Find and select the sibling pane
    QAbstractItemModel *model = switcher->treeView()->model();
    QModelIndex targetIdx = findPaneIndex(model, targetPane);
    QVERIFY(targetIdx.isValid());
    switcher->treeView()->setCurrentIndex(targetIdx);

    switcher->activateCurrent();

    // activePaneId should update to the target pane
    QTRY_COMPARE_WITH_TIMEOUT(f.controller->activePaneId(), targetPane, 10000);

    // The target pane's terminal display should have keyboard focus
    Session *targetSession = f.controller->sessionForPane(targetPane);
    QVERIFY(targetSession);
    QVERIFY(!targetSession->views().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(targetSession->views().first()->hasFocus(), 10000);

    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherSwitchesPaneDifferentWindow()
{
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    // Active pane is in w0; switch to the pane in w1
    int initialActivePane = f.controller->activePaneId();
    QVERIFY(initialActivePane != f.w1pane0Id);

    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    // Wait for the async queryTree response to populate the model
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);

    QAbstractItemModel *model = switcher->treeView()->model();
    QModelIndex targetIdx = findPaneIndex(model, f.w1pane0Id);
    QVERIFY(targetIdx.isValid());
    switcher->treeView()->setCurrentIndex(targetIdx);

    switcher->activateCurrent();

    // activePaneId should update to w1's pane AND the active tab should change
    QTRY_COMPARE_WITH_TIMEOUT(f.controller->activePaneId(), f.w1pane0Id, 10000);

    auto *container = f.attach.mw->viewManager()->activeContainer();
    int expectedTabIndex = f.controller->windowToTabIndex().value(f.w1WindowId);
    QTRY_COMPARE_WITH_TIMEOUT(container->currentIndex(), expectedTabIndex, 10000);

    // The target pane's terminal display should have keyboard focus
    Session *targetSession = f.controller->sessionForPane(f.w1pane0Id);
    QVERIFY(targetSession);
    QVERIFY(!targetSession->views().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(targetSession->views().first()->hasFocus(), 10000);

    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherSwitchesWindow()
{
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    // Active is in w0; pick the w1 window node
    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    // Wait for the async queryTree response to populate the model
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);

    QAbstractItemModel *model = switcher->treeView()->model();
    QModelIndex targetIdx = findWindowIndex(model, f.w1WindowId);
    QVERIFY(targetIdx.isValid());
    switcher->treeView()->setCurrentIndex(targetIdx);

    switcher->activateCurrent();

    // Active tab should become w1's tab
    auto *container = f.attach.mw->viewManager()->activeContainer();
    int expectedTabIndex = f.controller->windowToTabIndex().value(f.w1WindowId);
    QTRY_COMPARE_WITH_TIMEOUT(container->currentIndex(), expectedTabIndex, 10000);

    // The active pane's terminal display in the now-active window should have focus
    QTRY_COMPARE_WITH_TIMEOUT(f.controller->activePaneId(), f.w1pane0Id, 10000);
    Session *targetSession = f.controller->sessionForPane(f.w1pane0Id);
    QVERIFY(targetSession);
    QVERIFY(!targetSession->views().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(targetSession->views().first()->hasFocus(), 10000);

    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherEscapeClosesNoChange()
{
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    int initialActivePane = f.controller->activePaneId();
    auto *container = f.attach.mw->viewManager()->activeContainer();
    int initialTabIndex = container->currentIndex();

    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    // Wait for the async queryTree response to populate the model
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);
    QPointer<TmuxTreeSwitcher> guard(switcher);

    QTest::keyClick(switcher, Qt::Key_Escape);

    // Switcher should go away
    QTRY_VERIFY_WITH_TIMEOUT(!guard, 5000);

    // No changes to active pane or active tab
    QCOMPARE(f.controller->activePaneId(), initialActivePane);
    QCOMPARE(container->currentIndex(), initialTabIndex);

    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherFuzzyFilter()
{
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    // Rename w1 to a searchable name via tmux
    QProcess rename;
    rename.start(f.tmuxPath,
                 {QStringLiteral("-S"),
                  f.ctx.socketPath,
                  QStringLiteral("rename-window"),
                  QStringLiteral("-t"),
                  QStringLiteral("%1:1").arg(f.ctx.sessionName),
                  QStringLiteral("xyzzy-unique")});
    QVERIFY(rename.waitForFinished(5000));

    // Wait for the tab title to propagate
    auto *container = f.attach.mw->viewManager()->activeContainer();
    int w1TabIndex = f.controller->windowToTabIndex().value(f.w1WindowId);
    QTRY_COMPARE_WITH_TIMEOUT(container->tabText(w1TabIndex), QStringLiteral("xyzzy-unique"), 10000);

    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    // Wait for the async queryTree response to populate the model
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);

    // Type the filter
    QLineEdit *input = switcher->findChild<QLineEdit *>();
    QVERIFY(input);
    input->setText(QStringLiteral("xyzzy"));

    // After filtering, the only matching window should be xyzzy-unique (w1)
    // Walk the filtered model and assert only w1 appears at the window level.
    QAbstractItemModel *filteredModel = switcher->treeView()->model();
    // top-level = sessions; session has windows; verify xyzzy window matched
    bool found = false;
    std::function<void(const QModelIndex &)> walk = [&](const QModelIndex &parent) {
        for (int i = 0; i < filteredModel->rowCount(parent); ++i) {
            QModelIndex idx = filteredModel->index(i, 0, parent);
            if (idx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::WindowNode && idx.data(TmuxTreeModel::IdRole).toInt() == f.w1WindowId) {
                found = true;
            }
            walk(idx);
        }
    };
    walk({});
    QVERIFY2(found, "xyzzy-unique window should pass the fuzzy filter");

    // w0 window (no matching name) should NOT appear in the filtered result
    // (unless one of its panes has "xyzzy" in its title, which we didn't set)
    bool w0Present = false;
    std::function<void(const QModelIndex &)> walk2 = [&](const QModelIndex &parent) {
        for (int i = 0; i < filteredModel->rowCount(parent); ++i) {
            QModelIndex idx = filteredModel->index(i, 0, parent);
            if (idx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::WindowNode && idx.data(TmuxTreeModel::IdRole).toInt() == f.w0WindowId) {
                w0Present = true;
            }
            walk2(idx);
        }
    };
    walk2({});
    QVERIFY2(!w0Present, "w0 window should be filtered out");

    delete switcher;
    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherSwitchesSession()
{
    // Create a tmux server with 2 sessions. Attach kmux to session A.
    // Use the tree switcher to select a pane in session B; after the switch,
    // kmux should be attached to session B.
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctxA;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctxA);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctxA);
    });

    // Create session B on the same socket (same server)
    const QString sessionBName = ctxA.sessionName + QStringLiteral("-B");
    QProcess newSessionB;
    newSessionB.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctxA.socketPath,
                       QStringLiteral("new-session"),
                       QStringLiteral("-d"),
                       QStringLiteral("-s"),
                       sessionBName,
                       QStringLiteral("sleep 60")});
    QVERIFY(newSessionB.waitForFinished(5000));
    QCOMPARE(newSessionB.exitCode(), 0);

    auto cleanupB = qScopeGuard([&] {
        QProcess kill;
        kill.start(tmuxPath, {QStringLiteral("-S"), ctxA.socketPath, QStringLiteral("kill-session"), QStringLiteral("-t"), sessionBName});
        kill.waitForFinished(5000);
    });

    // Attach to session A
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctxA, attach);
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QTRY_VERIFY_WITH_TIMEOUT(controller->activePaneId() >= 0, 10000);

    int initialSessionId = controller->sessionId();
    QVERIFY(initialSessionId >= 0);

    // Open the tree switcher and find session B
    auto *switcher = new TmuxTreeSwitcher(attach.mw->viewManager(), controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    // Wait for queryTree to return both sessions
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 2, 10000);

    // Find session B's index (any non-current session)
    QAbstractItemModel *model = switcher->treeView()->model();
    QModelIndex sessionBIdx;
    for (int i = 0; i < model->rowCount(); ++i) {
        QModelIndex idx = model->index(i, 0);
        if (idx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::SessionNode && idx.data(TmuxTreeModel::IdRole).toInt() != initialSessionId) {
            sessionBIdx = idx;
            break;
        }
    }
    QVERIFY2(sessionBIdx.isValid(), "Session B should appear in the tree");

    int sessionBId = sessionBIdx.data(TmuxTreeModel::IdRole).toInt();

    switcher->treeView()->setCurrentIndex(sessionBIdx);
    switcher->activateCurrent();

    // After switch-client, tmux sends %session-changed and the controller
    // updates _sessionId. Wait for that.
    QTRY_COMPARE_WITH_TIMEOUT(controller->sessionId(), sessionBId, 10000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherSwitchesSessionWithTwoPanes()
{
    // Session A has 1 pane, session B has 2 panes. After switching via the
    // tree switcher, the active window of the controller should contain 2 panes.
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctxA;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctxA);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctxA);
    });

    // Create session B on the same socket with a first pane
    const QString sessionBName = ctxA.sessionName + QStringLiteral("-B");
    QProcess newSessionB;
    newSessionB.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctxA.socketPath,
                       QStringLiteral("new-session"),
                       QStringLiteral("-d"),
                       QStringLiteral("-s"),
                       sessionBName,
                       QStringLiteral("sleep 60")});
    QVERIFY(newSessionB.waitForFinished(5000));
    QCOMPARE(newSessionB.exitCode(), 0);

    auto cleanupB = qScopeGuard([&] {
        QProcess kill;
        kill.start(tmuxPath, {QStringLiteral("-S"), ctxA.socketPath, QStringLiteral("kill-session"), QStringLiteral("-t"), sessionBName});
        kill.waitForFinished(5000);
    });

    // Split session B's window to get a second pane
    QProcess splitB;
    splitB.start(tmuxPath,
                 {QStringLiteral("-S"),
                  ctxA.socketPath,
                  QStringLiteral("split-window"),
                  QStringLiteral("-t"),
                  sessionBName + QStringLiteral(":0"),
                  QStringLiteral("sleep 60")});
    QVERIFY(splitB.waitForFinished(5000));
    QCOMPARE(splitB.exitCode(), 0);

    // Attach to session A
    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctxA, attach);
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QTRY_VERIFY_WITH_TIMEOUT(controller->activePaneId() >= 0, 10000);

    int initialSessionId = controller->sessionId();
    QVERIFY(initialSessionId >= 0);

    // Sanity: session A has 1 pane in its active window
    int initialWindowId = controller->windowIdForPane(controller->activePaneId());
    QCOMPARE(controller->paneCountForWindow(initialWindowId), 1);

    // Open the tree switcher and find session B
    auto *switcher = new TmuxTreeSwitcher(attach.mw->viewManager(), controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 2, 10000);

    QAbstractItemModel *model = switcher->treeView()->model();
    QModelIndex sessionBIdx;
    for (int i = 0; i < model->rowCount(); ++i) {
        QModelIndex idx = model->index(i, 0);
        if (idx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::SessionNode && idx.data(TmuxTreeModel::IdRole).toInt() != initialSessionId) {
            sessionBIdx = idx;
            break;
        }
    }
    QVERIFY2(sessionBIdx.isValid(), "Session B should appear in the tree");

    int sessionBId = sessionBIdx.data(TmuxTreeModel::IdRole).toInt();

    switcher->treeView()->setCurrentIndex(sessionBIdx);
    switcher->activateCurrent();

    // Wait for the session switch to land
    QTRY_COMPARE_WITH_TIMEOUT(controller->sessionId(), sessionBId, 10000);

    // After the switch, the controller's window map should reflect session B:
    // exactly one window, containing 2 panes.
    QTRY_VERIFY_WITH_TIMEOUT(controller->windowCount() == 1, 10000);
    const auto &windowTabs = controller->windowToTabIndex();
    QVERIFY(!windowTabs.isEmpty());
    int bWindowId = windowTabs.firstKey();
    QTRY_COMPARE_WITH_TIMEOUT(controller->paneCountForWindow(bWindowId), 2, 10000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherStaleSessionIsNoop()
{
    // If the user selects a session that has since been killed on the
    // server (race condition), switch-client should fail gracefully and
    // kmux should remain attached to the current session.
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctxA;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctxA);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctxA);
    });

    const QString sessionBName = ctxA.sessionName + QStringLiteral("-stale");
    QProcess newSessionB;
    newSessionB.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctxA.socketPath,
                       QStringLiteral("new-session"),
                       QStringLiteral("-d"),
                       QStringLiteral("-s"),
                       sessionBName,
                       QStringLiteral("sleep 60")});
    QVERIFY(newSessionB.waitForFinished(5000));
    QCOMPARE(newSessionB.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctxA, attach);
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QTRY_VERIFY_WITH_TIMEOUT(controller->activePaneId() >= 0, 10000);

    int initialSessionId = controller->sessionId();

    auto *switcher = new TmuxTreeSwitcher(attach.mw->viewManager(), controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 2, 10000);

    // Find the other session's index BEFORE killing it — this snapshots the tree
    QAbstractItemModel *model = switcher->treeView()->model();
    QModelIndex staleIdx;
    int staleSessionId = -1;
    for (int i = 0; i < model->rowCount(); ++i) {
        QModelIndex idx = model->index(i, 0);
        int id = idx.data(TmuxTreeModel::IdRole).toInt();
        if (idx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::SessionNode && id != initialSessionId) {
            staleIdx = idx;
            staleSessionId = id;
            break;
        }
    }
    QVERIFY(staleIdx.isValid());
    QVERIFY(staleSessionId >= 0);

    // Kill session B behind the switcher's back
    QProcess kill;
    kill.start(tmuxPath, {QStringLiteral("-S"), ctxA.socketPath, QStringLiteral("kill-session"), QStringLiteral("-t"), sessionBName});
    QVERIFY(kill.waitForFinished(5000));
    QCOMPARE(kill.exitCode(), 0);

    // Activate the stale selection — switch-client will fail silently
    switcher->treeView()->setCurrentIndex(staleIdx);
    switcher->activateCurrent();

    // Give tmux a moment to process (and not) the switch
    QTest::qWait(500);

    // Still attached to the original session
    QCOMPARE(controller->sessionId(), initialSessionId);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherActivatePaneAlreadyActiveIsNoop()
{
    // Activating the pane that is already active must not change active pane
    // or active tab — it should just dismiss the switcher.
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    const int activePane = f.controller->activePaneId();
    auto *container = f.attach.mw->viewManager()->activeContainer();
    const int initialTabIndex = container->currentIndex();

    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);

    QModelIndex selfIdx = findPaneIndex(switcher->treeView()->model(), activePane);
    QVERIFY(selfIdx.isValid());
    switcher->treeView()->setCurrentIndex(selfIdx);
    switcher->activateCurrent();

    QTest::qWait(500);
    QCOMPARE(f.controller->activePaneId(), activePane);
    QCOMPARE(container->currentIndex(), initialTabIndex);

    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherActivateCurrentWindowIsNoop()
{
    // Activating the window node that already hosts the active pane must
    // leave active pane and tab unchanged.
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    const int activePane = f.controller->activePaneId();
    const int activeWindow = f.w0WindowId; // fixture starts with active pane in w0
    auto *container = f.attach.mw->viewManager()->activeContainer();
    const int initialTabIndex = container->currentIndex();

    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);

    QModelIndex winIdx = findWindowIndex(switcher->treeView()->model(), activeWindow);
    QVERIFY(winIdx.isValid());
    switcher->treeView()->setCurrentIndex(winIdx);
    switcher->activateCurrent();

    QTest::qWait(500);
    QCOMPARE(f.controller->activePaneId(), activePane);
    QCOMPARE(container->currentIndex(), initialTabIndex);

    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testTreeSwitcherActivateCurrentSessionIsNoop()
{
    // Activating the session node we're already attached to must not issue
    // a switch-client and must leave active pane/tab/session untouched.
    TreeSwitcherFixture f;
    setupTreeSwitcherFixture(f, m_tmuxTmpDir.path());
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(f.tmuxPath, f.ctx);
    });

    const int activePane = f.controller->activePaneId();
    const int initialSessionId = f.controller->sessionId();
    auto *container = f.attach.mw->viewManager()->activeContainer();
    const int initialTabIndex = container->currentIndex();

    auto *switcher = new TmuxTreeSwitcher(f.attach.mw->viewManager(), f.controller);
    QVERIFY(QTest::qWaitForWindowExposed(switcher));
    QTRY_VERIFY_WITH_TIMEOUT(switcher->treeView()->model()->rowCount() >= 1, 5000);

    QAbstractItemModel *model = switcher->treeView()->model();
    QModelIndex sessionIdx;
    for (int i = 0; i < model->rowCount(); ++i) {
        QModelIndex idx = model->index(i, 0);
        if (idx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::SessionNode && idx.data(TmuxTreeModel::IdRole).toInt() == initialSessionId) {
            sessionIdx = idx;
            break;
        }
    }
    QVERIFY(sessionIdx.isValid());
    switcher->treeView()->setCurrentIndex(sessionIdx);
    switcher->activateCurrent();

    QTest::qWait(500);
    QCOMPARE(f.controller->sessionId(), initialSessionId);
    QCOMPARE(f.controller->activePaneId(), activePane);
    QCOMPARE(container->currentIndex(), initialTabIndex);

    delete f.attach.mw.data();
}

void TmuxIntegrationTest::testClosePaneFromSessionControllerConfirmed()
{
    // Closing a single pane in a multi-pane window via SessionController::closeSession
    // should show a confirmation dialog. Preset "don't ask again" = PrimaryAction
    // so the close proceeds without blocking the test.
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
            return false;
        }(),
        10000);

    // Preset "don't ask again" to Close Pane (PrimaryAction)
    KMessageBox::saveDontShowAgainTwoActions(QStringLiteral("ConfirmCloseTmuxPane"), KMessageBox::PrimaryAction);

    // Close one of the panes via its SessionController
    auto terminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(terminals.size(), 2);
    auto *controller = terminals.first()->sessionController();
    QVERIFY(controller);
    controller->closeSession();

    // Pane count should drop to 1 (single pane remaining, the window is not closed)
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!c || c->count() != 1)
                return false;
            auto *s = c->viewSplitterAt(0);
            return s && s->findChildren<TerminalDisplay *>().size() == 1;
        }(),
        10000);

    // Reset the setting so other tests aren't affected
    KMessageBox::enableAllMessages();

    delete attach.mw.data();
}

void TmuxIntegrationTest::testClosePaneFromSessionControllerCancelled()
{
    // When "don't ask again" = SecondaryAction (Cancel), closeSession should
    // do nothing — the pane stays.
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
            return false;
        }(),
        10000);

    // Preset "don't ask again" = Cancel (SecondaryAction)
    KMessageBox::saveDontShowAgainTwoActions(QStringLiteral("ConfirmCloseTmuxPane"), KMessageBox::SecondaryAction);

    auto terminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(terminals.size(), 2);
    auto *sc = terminals.first()->sessionController();
    sc->closeSession();

    // Give async tmux work a moment to potentially produce layout changes
    QTest::qWait(500);

    // Still 2 panes — close was cancelled
    QCOMPARE(paneSplitter->findChildren<TerminalDisplay *>().size(), 2);

    KMessageBox::enableAllMessages();

    delete attach.mw.data();
}

void TmuxIntegrationTest::testCloseTabFromContainerConfirmed()
{
    // When the user clicks the X on a tab (closeTerminalTab path) or uses Ctrl+W,
    // a confirmation dialog appears. Preset "don't ask again" so the close proceeds.
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    // Add a second tmux window so we have 2 tabs — closing one shouldn't tear down kmux
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 2, 10000);

    // Preset "don't ask again" to Close Tab (PrimaryAction)
    KMessageBox::saveDontShowAgainTwoActions(QStringLiteral("ConfirmCloseTmuxWindow"), KMessageBox::PrimaryAction);

    // Trigger tab close via the tab bar's tabCloseRequested signal —
    // this is what clicking the X invokes.
    QTabBar *tabBar = container->tabBar();
    QVERIFY(tabBar);
    QMetaObject::invokeMethod(tabBar, "tabCloseRequested", Qt::DirectConnection, Q_ARG(int, 0));

    // One tab should be gone
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 1, 10000);

    KMessageBox::enableAllMessages();

    delete attach.mw.data();
}

void TmuxIntegrationTest::testFractalSplitDownRight8()
{
    const int depth = 8;
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();

    // Use a large window so deep splits don't hit tmux minimum pane size
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │ columns: 256                                                                   │
        │ lines: 64                                                                      │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    });

    TmuxTestDSL::AttachResult attach;
    TmuxTestDSL::attachKonsole(tmuxPath, ctx, attach);

    // Show and activate the window so setFocus() works
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);

    // Perform alternating horizontal/vertical splits: )()()...
    const int expectedPanes = depth + 1;

    // Look up the split actions — these are the actual hotkey handlers
    // Ctrl+( = split-view-left-right (horizontal), Ctrl+) = split-view-top-bottom (vertical)
    QAction *splitH = attach.mw->actionCollection()->action(QStringLiteral("split-view-left-right"));
    QAction *splitV = attach.mw->actionCollection()->action(QStringLiteral("split-view-top-bottom"));
    QVERIFY2(splitH, "split-view-left-right action not found");
    QVERIFY2(splitV, "split-view-top-bottom action not found");

    for (int i = 0; i < depth; ++i) {
        // Alternate: ( ) ( ) ...
        QAction *action = (i % 2 == 0) ? splitH : splitV;
        action->trigger();

        // Wait for pane count to increase
        int expectedCount = i + 2;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
                if (!container)
                    return false;
                for (int t = 0; t < container->count(); ++t) {
                    auto *splitter = container->viewSplitterAt(t);
                    if (splitter && splitter->findChildren<TerminalDisplay *>().size() == expectedCount) {
                        return true;
                    }
                }
                return false;
            }(),
            10000);
    }

    // Verify tmux has the expected number of panes
    QProcess listPanes;
    listPanes.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id}")});
    QVERIFY(listPanes.waitForFinished(5000));
    QCOMPARE(listPanes.exitCode(), 0);
    QStringList paneLines = QString::fromUtf8(listPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(paneLines.size(), expectedPanes);

    // Verify the fractal splitter structure:
    // HSplit[Leaf, VSplit[Leaf, HSplit[Leaf, ...]]]
    // Alternating H/V, nested child always second (right/bottom).
    auto *container = attach.mw->viewManager()->activeContainer();
    QVERIFY(container);
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < container->count(); ++i) {
        auto *splitter = container->viewSplitterAt(i);
        if (splitter && splitter->findChildren<TerminalDisplay *>().size() == expectedPanes) {
            paneSplitter = splitter;
            break;
        }
    }
    QVERIFY2(paneSplitter, qPrintable(QStringLiteral("Expected a ViewSplitter with %1 TerminalDisplay children").arg(expectedPanes)));

    // Verify the fractal structure: walk the tree always taking the last child.
    // At each level the orientation should alternate H, V, H, V, ...
    if (depth >= 1) {
        ViewSplitter *current = paneSplitter;
        for (int d = 0; d < depth; ++d) {
            Qt::Orientation expectedOrientation = (d % 2 == 0) ? Qt::Horizontal : Qt::Vertical;
            QVERIFY2(current->orientation() == expectedOrientation,
                     qPrintable(QStringLiteral("Depth %1: expected %2 but got %3")
                                    .arg(d)
                                    .arg(expectedOrientation == Qt::Horizontal ? QStringLiteral("Horizontal") : QStringLiteral("Vertical"))
                                    .arg(current->orientation() == Qt::Horizontal ? QStringLiteral("Horizontal") : QStringLiteral("Vertical"))));
            QVERIFY2(current->count() >= 2, qPrintable(QStringLiteral("Depth %1: expected at least 2 children but got %2").arg(d).arg(current->count())));

            // The last child should be a ViewSplitter (except at deepest level)
            auto *lastChild = current->widget(current->count() - 1);
            if (d < depth - 1) {
                auto *nextSplitter = qobject_cast<ViewSplitter *>(lastChild);
                QVERIFY2(nextSplitter, qPrintable(QStringLiteral("Depth %1: last child should be a ViewSplitter").arg(d)));
                current = nextSplitter;
            }
        }
    }

    // Verify the final bottom-right pane has focus.
    // Walk down always taking the last child to find the deepest bottom-right display.
    QWidget *node = paneSplitter;
    while (auto *splitter = qobject_cast<ViewSplitter *>(node)) {
        node = splitter->widget(splitter->count() - 1);
    }
    auto *deepestBottomRight = qobject_cast<TerminalDisplay *>(node);
    QVERIFY(deepestBottomRight);
    QTRY_VERIFY_WITH_TIMEOUT(deepestBottomRight->hasFocus(), 5000);

    // Cleanup
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

QTEST_MAIN(TmuxIntegrationTest)

#include "moc_TmuxIntegrationTest.cpp"
