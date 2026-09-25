/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXWORKSPACESNAPSHOT_H
#define TMUXWORKSPACESNAPSHOT_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "konsoleprivate_export.h"

namespace Konsole
{

struct KONSOLEPRIVATE_EXPORT TmuxPaneSnapshot {
    int paneId = -1;
    QString workingDirectory;
    bool active = false;
};

struct KONSOLEPRIVATE_EXPORT TmuxWindowSnapshot {
    int windowId = -1;
    int index = -1;
    QString name;
    QString layout;
    bool active = false;
    QList<TmuxPaneSnapshot> panes;
};

/**
 * A restart-safe description of one tmux session.
 *
 * It intentionally contains only structure and shell working directories.
 * Re-running arbitrary foreground commands after a reboot would repeat side
 * effects and is therefore outside the restore contract.
 */
struct KONSOLEPRIVATE_EXPORT TmuxWorkspaceSnapshot {
    static constexpr int CurrentVersion = 1;

    int version = CurrentVersion;
    qint64 serverPid = 0;
    qint64 sessionCreated = 0;
    QString sessionName;
    QList<TmuxWindowSnapshot> windows;

    bool isValid() const;
    QByteArray toJson() const;
    static std::optional<TmuxWorkspaceSnapshot> fromJson(const QByteArray &json);

    /** Remap the saved layout's pane IDs to newly-created pane IDs. */
    static std::optional<QString> remapLayout(const TmuxWindowSnapshot &window, const QList<int> &newPaneIds);
};

/**
 * Per-MainWindow state saved by KDE session management.
 *
 * The workspace is session-wide and can be duplicated across windows. The
 * visible indexes preserve kmux windows created by Detach Tab without relying
 * on tmux object IDs, which do not survive a reboot.
 */
struct KONSOLEPRIVATE_EXPORT TmuxRestoreState {
    static constexpr int CurrentVersion = 1;

    int version = CurrentVersion;
    QString tmuxPath;
    QStringList tmuxArgs;
    QStringList rshCommand;
    QList<int> visibleWindowIndexes;
    int activeWindowIndex = -1;
    TmuxWorkspaceSnapshot workspace;

    bool isValid() const;
    QByteArray toJson() const;
    static std::optional<TmuxRestoreState> fromJson(const QByteArray &json);
};

}

#endif // TMUXWORKSPACESNAPSHOT_H
