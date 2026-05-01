/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIRTUALSESSION_H
#define VIRTUALSESSION_H

#include "Session.h"
#include "konsoleprivate_export.h"

namespace Konsole
{

/**
 * Session subclass for tmux panes and other virtual sessions
 * that have an emulation but no PTY.
 *
 * Data is injected programmatically via injectData() rather
 * than coming from a shell process.
 */
class KONSOLEPRIVATE_EXPORT VirtualSession : public Session
{
    Q_OBJECT
public:
    explicit VirtualSession(QObject *parent = nullptr);

    void injectData(const char *data, int length);

    void setExternalProcessName(const QString &name);
    void setExternalCurrentDir(const QString &dir);
    void setExternalPaneTitle(const QString &title);
    // Bind the underlying NullProcessInfo to a real OS pid so the
    // existing %u / %B / SSH-argv expansion paths populate from the
    // kernel. Used by tmux panes, where pane_pid is the shell process
    // running inside the tmux pane.
    void setExternalPid(int pid);

protected:
    void run() override;
    void close() override;
};

} // namespace Konsole

#endif // VIRTUALSESSION_H
