/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>

#include "../tmux/TmuxCommand.h"
#include "../tmux/TmuxLayoutParser.h"
#include "../tmux/TmuxWorkspaceSnapshot.h"

using namespace Konsole;

class TmuxWorkspaceSnapshotTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void roundTripRestoreState();
    void rejectsInvalidState();
    void remapsPaneIdsInLayout();
    void quotesControlModeArguments();
};

namespace
{
TmuxWorkspaceSnapshot makeSnapshot()
{
    TmuxLayoutNode left{TmuxLayoutNodeType::Leaf, 39, 24, 0, 0, 10, {}};
    TmuxLayoutNode right{TmuxLayoutNodeType::Leaf, 40, 24, 40, 0, 11, {}};
    TmuxLayoutNode root{TmuxLayoutNodeType::HSplit, 80, 24, 0, 0, -1, {left, right}};

    TmuxWorkspaceSnapshot snapshot;
    snapshot.serverPid = 8675;
    snapshot.sessionCreated = 1789730000;
    snapshot.sessionName = QStringLiteral("work session");

    TmuxWindowSnapshot window;
    window.windowId = 3;
    window.index = 1;
    window.name = QStringLiteral("quote ' and\nnewline");
    window.layout = TmuxLayoutParser::serialize(root);
    window.active = true;
    window.panes = {
        TmuxPaneSnapshot{10, QStringLiteral("/tmp/left pane"), true},
        TmuxPaneSnapshot{11, QStringLiteral("/tmp/right'pane"), false},
    };
    snapshot.windows.append(window);
    return snapshot;
}
}

void TmuxWorkspaceSnapshotTest::roundTripRestoreState()
{
    TmuxRestoreState state;
    state.tmuxPath = QStringLiteral("/opt/tmux custom");
    state.tmuxArgs = {QStringLiteral("-L"), QStringLiteral("socket name")};
    state.rshCommand = {QStringLiteral("ssh"), QStringLiteral("host alias")};
    state.visibleWindowIndexes = {1};
    state.activeWindowIndex = 1;
    state.workspace = makeSnapshot();

    const auto restored = TmuxRestoreState::fromJson(state.toJson());
    QVERIFY(restored.has_value());
    QCOMPARE(restored->tmuxPath, state.tmuxPath);
    QCOMPARE(restored->tmuxArgs, state.tmuxArgs);
    QCOMPARE(restored->rshCommand, state.rshCommand);
    QCOMPARE(restored->visibleWindowIndexes, state.visibleWindowIndexes);
    QCOMPARE(restored->activeWindowIndex, state.activeWindowIndex);
    QCOMPARE(restored->workspace.serverPid, state.workspace.serverPid);
    QCOMPARE(restored->workspace.sessionCreated, state.workspace.sessionCreated);
    QCOMPARE(restored->workspace.sessionName, state.workspace.sessionName);
    QCOMPARE(restored->workspace.windows.size(), 1);
    QCOMPARE(restored->workspace.windows.first().name, state.workspace.windows.first().name);
    QCOMPARE(restored->workspace.windows.first().panes.at(1).workingDirectory, state.workspace.windows.first().panes.at(1).workingDirectory);
}

void TmuxWorkspaceSnapshotTest::rejectsInvalidState()
{
    QVERIFY(!TmuxWorkspaceSnapshot::fromJson(QByteArrayLiteral("{}")));

    TmuxRestoreState state;
    state.workspace = makeSnapshot();
    QByteArray json = state.toJson();
    json.replace("\"version\":1", "\"version\":99");
    QVERIFY(!TmuxRestoreState::fromJson(json));

    state.activeWindowIndex = 99;
    QVERIFY(!TmuxRestoreState::fromJson(state.toJson()));

    state.activeWindowIndex = -1;
    state.visibleWindowIndexes = {99};
    QVERIFY(!TmuxRestoreState::fromJson(state.toJson()));
}

void TmuxWorkspaceSnapshotTest::remapsPaneIdsInLayout()
{
    const TmuxWindowSnapshot window = makeSnapshot().windows.first();
    const auto remapped = TmuxWorkspaceSnapshot::remapLayout(window, {90, 91});
    QVERIFY(remapped.has_value());
    const auto parsed = TmuxLayoutParser::parse(remapped.value());
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->children.at(0).paneId, 90);
    QCOMPARE(parsed->children.at(1).paneId, 91);
    QVERIFY(!TmuxWorkspaceSnapshot::remapLayout(window, {90}));
}

void TmuxWorkspaceSnapshotTest::quotesControlModeArguments()
{
    const QString command = TmuxCommand(QStringLiteral("rename-window")).singleQuotedArg(QStringLiteral("a'b;c\nnext\rline")).build();
    QCOMPARE(command, QStringLiteral("rename-window 'a'\\''b;c'\\n'next'\\r'line'"));
}

QTEST_GUILESS_MAIN(TmuxWorkspaceSnapshotTest)

#include "TmuxWorkspaceSnapshotTest.moc"
