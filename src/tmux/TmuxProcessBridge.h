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
class TmuxReconnectPolicy;
class ViewManager;
enum class TmuxConnectionBanner;

/**
 * Spawns tmux in plain control mode (-C) as a QProcess and wires
 * its stdout/stdin to TmuxGateway and TmuxController.
 *
 * No PTY, no Session, no terminal emulation — the tmux subprocess
 * is completely hidden from the user. Drop/retry policy lives in
 * TmuxReconnectPolicy.
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

    /// Kill a hung/dead client and attach-session to the learned session name.
    void requestReconnect();
    /// Handshake deadline for reconnect attempts when there is no controlling
    /// TTY (not first launch; not interactive password prompts). 0 disables.
    void setHandshakeTimeoutMs(int ms);

Q_SIGNALS:
    void disconnected();
    /// Emitted once the tmux client sends its first %begin line — i.e. the
    /// rsh wrapper (if any) authenticated, tmux started, and the control
    /// protocol is live. Callers use this to hold back UI that would
    /// otherwise steal focus from a still-prompting rsh (ssh password).
    void ready();
    /// Emitted when the subprocess exits before ready() ever fired — i.e.
    /// the rsh wrapper or tmux died during startup (bad ssh host, remote
    /// tmux missing, wrapper exited non-zero, …). @p reason carries the
    /// exit code and any stderr/stdout the process emitted. Without this,
    /// a launch whose deferred-show is gated on ready() would idle forever
    /// with no window. Distinct from disconnected(), which fires on a
    /// teardown of an already-established session.
    void startupFailed(const QString &reason);

private:
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void teardownSession();
    void setViewsConnectionBanner(TmuxConnectionBanner banner);
    void connectGatewayBridgeSignals();
    void createGateway(bool bindController);
    bool spawnProcess(const QStringList &command);
    void teardownTransport();
    void spawnReconnectClient();
    void killControlProcess();
    void syncPolicySessionFacts();
    QString processExitReason(int exitCode, QProcess::ExitStatus exitStatus) const;
    QString learnedSessionName() const;

    ViewManager *_viewManager;
    TmuxReconnectPolicy *_policy = nullptr;
    QProcess *_process = nullptr;
    TmuxGateway *_gateway = nullptr;
    TmuxController *_controller = nullptr;
    QSocketNotifier *_readNotifier = nullptr;
    int _socketFd = -1;
    QByteArray _readBuffer;
    // Raw stdout accumulated while still pre-handshake. _readBuffer is
    // drained line-by-line into the gateway, so it can't be relied on for
    // a startup-failure diagnostic; this keeps an untouched copy (e.g. the
    // wrapper's error message) to report. Cleared once ready() fires.
    QByteArray _startupOutput;
    QString _tmuxPath;
    QStringList _tmuxArgs;
    QStringList _command;
    QStringList _rshCommand;
    bool _ignoringProcessFinished = false;
};

} // namespace Konsole

#endif // TMUXPROCESSBRIDGE_H
