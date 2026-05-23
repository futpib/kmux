/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxTreeRowFormat.h"

namespace Konsole
{
namespace TmuxTreeRowFormat
{
namespace
{
// tmux's window-tree.c gates the `: "pane_title"` suffix on
//   pane_title && pane_title != host_short
// — `host_short` is the local hostname's short form, which is what a
// default PS1 like `\u@\h:\w` writes to the OSC title. Without the
// filter every row in the picker would just repeat the hostname.
bool isMeaningfulTitle(const QString &title, const QString &hostShort)
{
    if (title.isEmpty()) {
        return false;
    }
    if (!hostShort.isEmpty() && title == hostShort) {
        return false;
    }
    return true;
}

QString quotedTitleSuffix(const QString &title)
{
    return QStringLiteral(": \"") + title + QLatin1Char('"');
}
} // namespace

QString formatWindow(const WindowRow &row, const QString &hostShort)
{
    Q_UNUSED(row.marked); // marked == selected-in-choose-tree; styling is the view's concern
    QString out = row.name + row.flags;
    // Only single-pane windows promote their lone pane's title to the
    // window row. Multi-pane windows force expansion so the user picks
    // which pane's title they meant — there's no canonical choice.
    if (row.paneCount == 1 && isMeaningfulTitle(row.singlePaneTitle, hostShort)) {
        out += quotedTitleSuffix(row.singlePaneTitle);
    }
    return out;
}

QString formatPane(const PaneRow &row, const QString &hostShort)
{
    QString out = row.command;
    if (row.active) {
        out += QLatin1Char('*');
    }
    if (row.marked) {
        out += QLatin1Char('M');
    }
    if (isMeaningfulTitle(row.title, hostShort)) {
        out += quotedTitleSuffix(row.title);
    }
    return out;
}

} // namespace TmuxTreeRowFormat
} // namespace Konsole
