/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxGatewayTest.h"

#include <QSignalSpy>
#include <QTest>

#include "../tmux/TmuxGateway.h"

using namespace Konsole;

void TmuxGatewayTest::testNotificationInsideServerOriginatedBlockIsRouted()
{
    // Repro for the dropped-notification bug. When tmux starts (or in any
    // other server-originated %begin/%end where no kmux command is queued),
    // tmux may interleave notifications inside the block — e.g. when
    // `new-session -A` triggers `%session-changed` as a side effect of the
    // server creating the session, and that arrives between %begin and
    // %end of an unrelated server-originated block.
    //
    // The gateway used to silently drop every non-%end line in a
    // server-originated block. That meant %session-changed never reached
    // the controller, _sessionId stayed -1, and downstream
    // %session-window-changed handling early-returned on the sessionId
    // mismatch — producing the "tab switch silently no-ops" symptom.
    TmuxGateway gateway([](const QByteArray &) {});

    QSignalSpy sessionChangedSpy(&gateway, &TmuxGateway::sessionChanged);
    QVERIFY(sessionChangedSpy.isValid());

    // Server-originated block (no pending commands when %begin arrives →
    // gateway flags it server-originated). Flag field "0" makes it
    // explicit; the no-pending-command path would also do it.
    gateway.processLine("%begin 1779000000 1 0");
    gateway.processLine("%session-changed $7 main");
    gateway.processLine("%end 1779000000 1 0");

    QCOMPARE(sessionChangedSpy.count(), 1);
    QCOMPARE(sessionChangedSpy.first().at(0).toInt(), 7);
    QCOMPARE(sessionChangedSpy.first().at(1).toString(), QStringLiteral("main"));
}

void TmuxGatewayTest::testNotificationOutsideAnyBlockIsRouted()
{
    // Baseline: notifications outside any %begin/%end block must reach
    // their signal. This is the always-worked path; we assert it so the
    // fix to the inside-block path doesn't accidentally break it.
    TmuxGateway gateway([](const QByteArray &) {});

    QSignalSpy spy(&gateway, &TmuxGateway::sessionChanged);
    gateway.processLine("%session-changed $3 other");

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), 3);
    QCOMPARE(spy.first().at(1).toString(), QStringLiteral("other"));
}

void TmuxGatewayTest::testExtendedOutputParsedAsOutput()
{
    // Once a control client sets the pause-after flag, tmux replaces %output
    // with %extended-output (inserting an "age" field before a " : " delimiter).
    // The gateway must treat it as pane output and decode the value after the
    // delimiter — otherwise enabling flow control makes kmux blind to all pane
    // output. The value is octal-escaped exactly like %output.
    TmuxGateway gateway([](const QByteArray &) {});

    QSignalSpy spy(&gateway, &TmuxGateway::outputReceived);
    QVERIFY(spy.isValid());

    gateway.processLine("%extended-output %3 0 : hello\\015\\012");

    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toInt(), 3);
    QCOMPARE(spy.first().at(1).toByteArray(), QByteArray("hello\r\n"));
}

void TmuxGatewayTest::testResponseInsideClientOriginatedBlockIsCaptured()
{
    // Make sure routing notifications inside server-originated blocks
    // didn't break the client-originated body capture: command responses
    // (non-%-prefixed lines and even %-prefixed text that's part of a
    // response payload) must still accumulate into the command response.
    TmuxGateway gateway([](const QByteArray &) {});

    bool fired = false;
    QString got;
    TmuxCommand cmd(QStringLiteral("display-message"));
    gateway.sendCommand(cmd, [&](bool ok, const QString &response) {
        fired = true;
        got = response;
        Q_UNUSED(ok);
    });

    // tmux replies with a client-originated block (flags=1). Body is plain
    // data — even though it starts with `%` it's the literal pane title,
    // not a notification. The gateway must not reroute it to
    // handleNotification.
    gateway.processLine("%begin 1779000000 1 1");
    gateway.processLine("hello world");
    gateway.processLine("%end 1779000000 1 1");

    QVERIFY(fired);
    QCOMPARE(got, QStringLiteral("hello world"));
}

void TmuxGatewayTest::testUnresponsiveFiresWhenCommandUnanswered()
{
    // A command that never gets its %begin/%end within the timeout marks the
    // control link unresponsive — the "ssh silently hung" case that produces
    // no EOF and so isn't caught by the bridge's process-exit teardown.
    TmuxGateway gateway([](const QByteArray &) {});
    gateway.setCommandTimeoutMs(50);
    QSignalSpy spy(&gateway, &TmuxGateway::unresponsive);
    QVERIFY(spy.isValid());

    gateway.sendCommand(TmuxCommand(QStringLiteral("display-message")));
    QVERIFY(spy.wait(2000));
    QCOMPARE(spy.count(), 1);
}

void TmuxGatewayTest::testReplyBeforeTimeoutStaysResponsive()
{
    // A command answered before the deadline must NOT trip the detector.
    TmuxGateway gateway([](const QByteArray &) {});
    gateway.setCommandTimeoutMs(300);
    QSignalSpy spy(&gateway, &TmuxGateway::unresponsive);

    gateway.sendCommand(TmuxCommand(QStringLiteral("display-message")));
    // Client-originated reply (flags bit 0 set) for command id 7.
    gateway.processLine("%begin 1700000000 7 1");
    gateway.processLine("hello");
    gateway.processLine("%end 1700000000 7 1");

    QVERIFY(!spy.wait(600));
    QCOMPARE(spy.count(), 0);
}

void TmuxGatewayTest::testIdleLinkNeverUnresponsive()
{
    // With no command outstanding the detector is disarmed — an idle link is
    // never flagged (matching iTerm2: only outstanding commands are timed).
    TmuxGateway gateway([](const QByteArray &) {});
    gateway.setCommandTimeoutMs(50);
    QSignalSpy spy(&gateway, &TmuxGateway::unresponsive);

    QVERIFY(!spy.wait(300));
    QCOMPARE(spy.count(), 0);
}

void TmuxGatewayTest::testActivityRecoversFromUnresponsive()
{
    // After going unresponsive, any line from the server resumes the link and
    // emits responsive() (so a UI prompt can dismiss itself).
    TmuxGateway gateway([](const QByteArray &) {});
    gateway.setCommandTimeoutMs(50);
    QSignalSpy unspy(&gateway, &TmuxGateway::unresponsive);
    QSignalSpy respy(&gateway, &TmuxGateway::responsive);

    gateway.sendCommand(TmuxCommand(QStringLiteral("display-message")));
    QVERIFY(unspy.wait(2000));
    QCOMPARE(unspy.count(), 1);

    // A reply block arrives late — the link is alive again.
    gateway.processLine("%begin 1700000000 7 1");
    QCOMPARE(respy.count(), 1);
}

void TmuxGatewayTest::testUnresponsiveFiresDespitePeriodicResends()
{
    // Regression: the deadline must measure time since the last *server reply*,
    // not since our last *send*. kmux re-sends a pane-title refresh every ~2s;
    // the production timeout is 5s. If sending a command restarted the deadline,
    // each refresh (interval < timeout) would push it back forever and a dead
    // link would never be flagged — which is exactly what happened when wifi was
    // turned off and no banner appeared.
    //
    // Reproduce with a timeout SHORTER than the test would otherwise need but
    // LONGER than the resend cadence: resend every 100ms with NO server reply,
    // and a 300ms timeout. With the restart-on-send bug, unresponsive() never
    // fires; correctly, it fires ~300ms after the first send because nothing
    // from the server has reset the deadline.
    TmuxGateway gateway([](const QByteArray &) {});
    gateway.setCommandTimeoutMs(300);
    QSignalSpy spy(&gateway, &TmuxGateway::unresponsive);
    QVERIFY(spy.isValid());

    gateway.sendCommand(TmuxCommand(QStringLiteral("list-panes")));
    for (int i = 0; i < 8 && spy.count() == 0; ++i) {
        QTest::qWait(100); // < 300ms timeout: a resend-restart would defeat detection
        gateway.sendCommand(TmuxCommand(QStringLiteral("list-panes")));
    }
    QVERIFY2(spy.count() >= 1, "unresponsive() never fired while commands kept being re-sent without replies");
}

QTEST_MAIN(TmuxGatewayTest)

#include "moc_TmuxGatewayTest.cpp"
