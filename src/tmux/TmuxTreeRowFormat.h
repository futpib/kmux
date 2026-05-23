/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXTREEROWFORMAT_H
#define TMUXTREEROWFORMAT_H

#include <QString>

#include "konsoleprivate_export.h"

namespace Konsole
{

/**
 * Pure formatting helpers that mirror the row text upstream tmux's
 * choose-tree produces — specifically WINDOW_TREE_DEFAULT_FORMAT in
 * tmux's `window-tree.c`. Replicating it so kmux's native tree
 * switcher reads the same as `Prefix + w` in stock tmux:
 *
 *   - Window row: "<window_name><window_flags>", with `: "<pane_title>"`
 *     appended only when the window has exactly one pane and the lone
 *     pane's title is non-empty and != host_short. Multi-pane windows
 *     deliberately suppress the title suffix; the user expands to see
 *     per-pane titles.
 *
 *   - Pane row:   "<pane_current_command><'*' if active><'M' if marked>",
 *     with the same `: "<pane_title>"` suffix gated on the
 *     title-non-empty-and-not-host_short check.
 *
 * The `host_short` filter — replicating upstream tmux — suppresses the
 * suffix when the title is just the local hostname (the value a default
 * bash PS1 writes), which would otherwise repeat the same string on
 * every row.
 */
namespace TmuxTreeRowFormat
{
struct KONSOLEPRIVATE_EXPORT WindowRow {
    QString name;
    QString flags; // tmux's #{window_flags} (e.g. "*", "-", "!")
    int paneCount = 0;
    QString singlePaneTitle; // pane_title of the lone pane when paneCount == 1
    bool marked = false; // window_marked_flag (selection in choose-tree; style-only here)
};

struct KONSOLEPRIVATE_EXPORT PaneRow {
    QString command; // #{pane_current_command}
    QString title; // #{pane_title}
    bool active = false; // #{pane_active}
    bool marked = false; // #{pane_marked}
};

KONSOLEPRIVATE_EXPORT QString formatWindow(const WindowRow &row, const QString &hostShort);
KONSOLEPRIVATE_EXPORT QString formatPane(const PaneRow &row, const QString &hostShort);

} // namespace TmuxTreeRowFormat

} // namespace Konsole

#endif // TMUXTREEROWFORMAT_H
