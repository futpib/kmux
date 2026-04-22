/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxPaneStateRecoveryTest.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include "../Emulation.h"
#include "../Screen.h"
#include "../ScreenWindow.h"
#include "../session/VirtualSession.h"
#include "TmuxTestDSL.h"

using namespace Konsole;

// Helper: simulate what handleCapturePaneResponse does (plain text, no escape sequences)
static void injectCapturePaneResponse(VirtualSession *session, const QString &response)
{
    const QStringList lines = response.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        QByteArray lineData = lines[i].toUtf8();
        if (i < lines.size() - 1) {
            lineData.append("\r\n");
        }
        session->injectData(lineData.constData(), lineData.size());
    }
}

// Helper: read all visible text from a VirtualSession's screen
static QString readScreenText(VirtualSession *session)
{
    ScreenWindow *window = session->emulation()->createWindow();
    Screen *screen = window->screen();

    int lines = screen->getLines();
    int columns = screen->getColumns();

    screen->setSelectionStart(0, 0, false);
    screen->setSelectionEnd(columns, lines - 1, false);
    QString text = screen->selectedText(Screen::PlainText);
    // Don't delete window — Emulation::~Emulation owns it via _windows list
    return text;
}

void TmuxPaneStateRecoveryTest::testCapturePaneContentRecovery()
{
    auto *session = new VirtualSession();

    // capture-pane without -e produces plain text
    QString response = QStringLiteral("$ echo hello\nhello\n$");

    injectCapturePaneResponse(session, response);

    QString screenText = readScreenText(session);

    QVERIFY2(screenText.contains(QStringLiteral("$ echo hello")),
             qPrintable(QStringLiteral("Screen should contain '$ echo hello', got: ") + screenText));
    QVERIFY2(screenText.contains(QStringLiteral("hello")),
             qPrintable(QStringLiteral("Screen should contain 'hello', got: ") + screenText));

    delete session;
}

void TmuxPaneStateRecoveryTest::testCapturePaneWithEscapeSequences()
{
    // capture-pane without -e strips escape sequences. The text content
    // (user@host, ~/code, etc.) is preserved as plain text.
    auto *session = new VirtualSession();

    QString response = QStringLiteral("user@host ~/code $ echo hello\nhello\nuser@host ~/code $");

    injectCapturePaneResponse(session, response);

    QString screenText = readScreenText(session);

    QVERIFY2(screenText.contains(QStringLiteral("user@host")),
             qPrintable(QStringLiteral("Screen should contain 'user@host', got: ") + screenText));
    QVERIFY2(screenText.contains(QStringLiteral("~/code")),
             qPrintable(QStringLiteral("Screen should contain '~/code', got: ") + screenText));
    QVERIFY2(screenText.contains(QStringLiteral("echo hello")),
             qPrintable(QStringLiteral("Screen should contain 'echo hello', got: ") + screenText));

    delete session;
}

void TmuxPaneStateRecoveryTest::testCapturePaneRealisticPrompt()
{
    // Simulate a realistic multi-line prompt as captured without -e
    auto *session = new VirtualSession();

    QString response = QStringLiteral("[15:25:11] [user@host ~/code/project] bash 5.3.9(1)-release  \xe2\x86\x92\ncommand output here\n[15:25:15] [user@host ~/code/project] bash 5.3.9(1)-release  \xe2\x86\x92");

    injectCapturePaneResponse(session, response);

    QString screenText = readScreenText(session);

    QVERIFY2(screenText.contains(QStringLiteral("user@host")),
             qPrintable(QStringLiteral("Screen should contain 'user@host', got: ") + screenText));
    QVERIFY2(screenText.contains(QStringLiteral("command output here")),
             qPrintable(QStringLiteral("Screen should contain 'command output here', got: ") + screenText));
    // Note: the → (U+2192) character may render differently depending on
    // the emulation's character width handling, so we just verify the
    // surrounding text is present.

    delete session;
}

void TmuxPaneStateRecoveryTest::testCapturePaneWideMismatch()
{
    // Default screen is 80x40; inject content from a wider (200-col) pane
    auto *session = new VirtualSession();

    QString longLine = QStringLiteral("START_MARKER ");
    while (longLine.size() < 200) {
        longLine += QLatin1Char('x');
    }
    longLine += QStringLiteral(" END_MARKER");

    QString response = longLine + QStringLiteral("\nsecond_line");

    injectCapturePaneResponse(session, response);

    QString screenText = readScreenText(session);

    QVERIFY2(screenText.contains(QStringLiteral("START_MARKER")),
             qPrintable(QStringLiteral("Screen should contain 'START_MARKER', got: ") + screenText));
    QVERIFY2(screenText.contains(QStringLiteral("second_line")),
             qPrintable(QStringLiteral("Screen should contain 'second_line', got: ") + screenText));

    delete session;
}

void TmuxPaneStateRecoveryTest::testCapturePaneFromRealTmux()
{
    const QString tmuxPath = TmuxTestDSL::findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    QTemporaryDir socketDir;
    QVERIFY(socketDir.isValid());

    // Create a detached tmux session with known dimensions using the DSL
    TmuxTestDSL::SessionContext ctx;
    TmuxTestDSL::setupTmuxSession(TmuxTestDSL::parse(QStringLiteral(R"(
        ┌───────────┐
        │cmd: cat   │
        │columns: 80│
        │lines: 24  │
        └───────────┘
    )")),
                                  tmuxPath,
                                  socketDir.path(),
                                  ctx);

    // Send known text to the pane via send-keys
    QProcess sendKeys;
    sendKeys.start(tmuxPath,
                   {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("send-keys"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("CAPTURE_TEST_MARKER"), QStringLiteral("Enter")});
    QVERIFY(sendKeys.waitForFinished(5000));
    QCOMPARE(sendKeys.exitCode(), 0);

    // Small delay to let the text appear
    QTest::qWait(200);

    // Capture pane content WITHOUT -e (matching our code: capture-pane -p -J -S -)
    QProcess capture;
    capture.start(tmuxPath,
                  {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-p"), QStringLiteral("-J"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("-S"), QStringLiteral("-")});
    QVERIFY(capture.waitForFinished(5000));
    QCOMPARE(capture.exitCode(), 0);

    QByteArray captureOutput = capture.readAllStandardOutput();
    QString captureText = QString::fromUtf8(captureOutput);

    // Inject into a VirtualSession
    auto *session = new VirtualSession();
    session->emulation()->setImageSize(24, 80);

    injectCapturePaneResponse(session, captureText);

    QString screenText = readScreenText(session);
    QVERIFY2(screenText.contains(QStringLiteral("CAPTURE_TEST_MARKER")),
             qPrintable(QStringLiteral("Screen should contain 'CAPTURE_TEST_MARKER', got: ") + screenText));

    delete session;

    // Cleanup tmux session
    TmuxTestDSL::killTmuxSession(tmuxPath, ctx);
}

QTEST_GUILESS_MAIN(TmuxPaneStateRecoveryTest)

#include "moc_TmuxPaneStateRecoveryTest.cpp"
