/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxTreeRowFormatTest.h"

#include <QTest>

#include "../tmux/TmuxTreeRowFormat.h"

using namespace Konsole;
using namespace Konsole::TmuxTreeRowFormat;

namespace
{
constexpr auto kHostShort = "claude-laptop";
} // namespace

// ---- window rows --------------------------------------------------------

void TmuxTreeRowFormatTest::testWindowRowSinglePaneAppendsMeaningfulTitle()
{
    // tmux/window-tree.c:47 — 1-pane window with a meaningful pane_title
    // gets the title appended after the name+flags.
    WindowRow row;
    row.name = QStringLiteral("claude");
    row.flags = QStringLiteral("*");
    row.paneCount = 1;
    row.singlePaneTitle = QStringLiteral("kmux-launch-prep");

    QCOMPARE(formatWindow(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("claude*: \"kmux-launch-prep\""));
}

void TmuxTreeRowFormatTest::testWindowRowSinglePaneSuppressesEmptyTitle()
{
    // Empty pane_title — no suffix even on a single-pane window.
    WindowRow row;
    row.name = QStringLiteral("claude");
    row.flags = QStringLiteral("*");
    row.paneCount = 1;
    row.singlePaneTitle = QString();

    QCOMPARE(formatWindow(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("claude*"));
}

void TmuxTreeRowFormatTest::testWindowRowSinglePaneSuppressesHostShortTitle()
{
    // tmux's `pane_title != host_short` guard filters out default PS1
    // titles. If a bash pane just set OSC title to the local hostname,
    // we don't want the row to repeat it.
    WindowRow row;
    row.name = QStringLiteral("claude");
    row.flags = QString();
    row.paneCount = 1;
    row.singlePaneTitle = QString::fromLatin1(kHostShort);

    QCOMPARE(formatWindow(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("claude"));
}

void TmuxTreeRowFormatTest::testWindowRowMultiPaneSuppressesTitleEvenIfMeaningful()
{
    // The key design choice we're replicating: with N>1 panes, the
    // window row never carries a title — the user expands to see each
    // pane's title. window-tree.c gates the suffix on `window_panes == 1`.
    WindowRow row;
    row.name = QStringLiteral("claude");
    row.flags = QStringLiteral("*");
    row.paneCount = 3;
    // singlePaneTitle is irrelevant when paneCount != 1, but a caller
    // might fill it in anyway — make sure the formatter ignores it.
    row.singlePaneTitle = QStringLiteral("would-be-meaningful");

    QCOMPARE(formatWindow(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("claude*"));
}

void TmuxTreeRowFormatTest::testWindowRowZeroPanesSuppressesTitle()
{
    // Defensive — a window with paneCount == 0 shouldn't crash and
    // shouldn't somehow append a title.
    WindowRow row;
    row.name = QStringLiteral("orphan");
    row.flags = QString();
    row.paneCount = 0;
    row.singlePaneTitle = QStringLiteral("ignored");

    QCOMPARE(formatWindow(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("orphan"));
}

void TmuxTreeRowFormatTest::testWindowRowRendersFlagsAfterName()
{
    // Concatenation order: tmux puts flags directly after the name with
    // no separator, e.g. "0:bash- " in stock status bars.
    WindowRow row;
    row.name = QStringLiteral("editor");
    row.flags = QStringLiteral("-!#");
    row.paneCount = 2;

    QCOMPARE(formatWindow(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("editor-!#"));
}

void TmuxTreeRowFormatTest::testWindowRowEmptyHostShortStillSuppressesEmptyTitle()
{
    // host_short can legitimately be empty (e.g. a Windows host with no
    // FQDN). The empty-title guard must still fire on its own — we
    // can't accidentally start showing literal empty `: ""` suffixes.
    WindowRow row;
    row.name = QStringLiteral("w");
    row.paneCount = 1;
    row.singlePaneTitle = QString();

    QCOMPARE(formatWindow(row, QString()), QStringLiteral("w"));
}

// ---- pane rows ----------------------------------------------------------

void TmuxTreeRowFormatTest::testPaneRowMeaningfulTitleAppended()
{
    // tmux/window-tree.c:43 — pane row is command + markers + optional
    // `: "pane_title"` when the title clears the same gate.
    PaneRow row;
    row.command = QStringLiteral("claude");
    row.title = QStringLiteral("kmux-launch-prep");

    QCOMPARE(formatPane(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("claude: \"kmux-launch-prep\""));
}

void TmuxTreeRowFormatTest::testPaneRowEmptyTitleSuppressed()
{
    PaneRow row;
    row.command = QStringLiteral("bash");
    row.title = QString();

    QCOMPARE(formatPane(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("bash"));
}

void TmuxTreeRowFormatTest::testPaneRowHostShortTitleSuppressed()
{
    PaneRow row;
    row.command = QStringLiteral("bash");
    row.title = QString::fromLatin1(kHostShort);

    QCOMPARE(formatPane(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("bash"));
}

void TmuxTreeRowFormatTest::testPaneRowActiveMarker()
{
    // Active pane: `*` right after the command name (before the title
    // suffix), matching `#{?pane_active,*,}` in tmux's format.
    PaneRow row;
    row.command = QStringLiteral("vim");
    row.title = QStringLiteral("README.md");
    row.active = true;

    QCOMPARE(formatPane(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("vim*: \"README.md\""));
}

void TmuxTreeRowFormatTest::testPaneRowMarkedMarker()
{
    // Marked pane: `M`. Tmux's format places the `*` before `M` —
    // `#{?pane_active,*,}#{?pane_marked,M,}` — preserve that order.
    PaneRow row;
    row.command = QStringLiteral("vim");
    row.marked = true;

    QCOMPARE(formatPane(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("vimM"));
}

void TmuxTreeRowFormatTest::testPaneRowActiveAndMarkedTogether()
{
    PaneRow row;
    row.command = QStringLiteral("top");
    row.title = QStringLiteral("htop overview");
    row.active = true;
    row.marked = true;

    QCOMPARE(formatPane(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("top*M: \"htop overview\""));
}

void TmuxTreeRowFormatTest::testPaneRowQuotingPreservesTitleCharacters()
{
    // We mirror tmux verbatim — no escaping of quotes or other chars
    // inside the title. If tmux ships an embedded quote, we ship an
    // embedded quote; the picker is a UI, not a parser-bait test.
    PaneRow row;
    row.command = QStringLiteral("claude");
    row.title = QStringLiteral("✳ Build \"the\" plan");

    QCOMPARE(formatPane(row, QString::fromLatin1(kHostShort)),
             QStringLiteral("claude: \"✳ Build \"the\" plan\""));
}

QTEST_GUILESS_MAIN(Konsole::TmuxTreeRowFormatTest)

#include "moc_TmuxTreeRowFormatTest.cpp"
