/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXPROCESSBRIDGE_H
#define TMUXPROCESSBRIDGE_H

#include <QObject>
#include <QProcess>
#include <QSocketNotifier>
#include <QString>

#include "konsoleprivate_export.h"

namespace Konsole
{

class TmuxGateway;
class TmuxController;
class ViewManager;

/**
 * Spawns tmux in plain control mode (-C) as a QProcess and wires
 * its stdout/stdin to TmuxGateway and TmuxController.
 *
 * No PTY, no Session, no terminal emulation — the tmux subprocess
 * is completely hidden from the user.
 */
class KONSOLEPRIVATE_EXPORT TmuxProcessBridge : public QObject
{
    Q_OBJECT
public:
    explicit TmuxProcessBridge(ViewManager *viewManager, QObject *parent = nullptr);
    ~TmuxProcessBridge() override;

    /**
     * @param tmuxPath   Path to tmux binary. Empty = find in PATH (local mode),
     *                   or "tmux" (when @p rshCommand is non-empty).
     * @param tmuxArgs   Extra tmux flags before -C (e.g. {"-S", "/path/to/socket"}).
     * @param command    Tmux command + args after -C (e.g. {"new-session", "-A"}).
     *                   Defaults to {"new-session", "-A"}.
     * @param rshCommand Optional remote-shell wrapper (e.g. {"ssh", "user@host"}).
     *                   When non-empty, the first element is the executable and
     *                   the rest are prepended to the tmux invocation.
     */
    bool start(const QString &tmuxPath = QString(),
               const QStringList &tmuxArgs = {},
               const QStringList &command = {QStringLiteral("new-session"), QStringLiteral("-A")},
               const QStringList &rshCommand = {});

    TmuxController *controller() const;

    QString tmuxPath() const;
    QStringList tmuxArgs() const;
    QStringList command() const;
    QStringList rshCommand() const;

Q_SIGNALS:
    void disconnected();

private:
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void teardown();

    ViewManager *_viewManager;
    QProcess *_process = nullptr;
    TmuxGateway *_gateway = nullptr;
    TmuxController *_controller = nullptr;
    QSocketNotifier *_readNotifier = nullptr;
    int _socketFd = -1;
    QByteArray _readBuffer;
    QString _tmuxPath;
    QStringList _tmuxArgs;
    QStringList _command;
    QStringList _rshCommand;
};

} // namespace Konsole

#endif // TMUXPROCESSBRIDGE_H
