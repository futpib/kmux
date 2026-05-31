/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXGATEWAY_H
#define TMUXGATEWAY_H

#include <QByteArray>
#include <QObject>
#include <QQueue>

#include <functional>
#include <optional>

#include "TmuxCommand.h"
#include "TmuxNotification.h"
#include "konsoleprivate_export.h"

class QTimer;

namespace Konsole
{

class KONSOLEPRIVATE_EXPORT TmuxGateway : public QObject
{
    Q_OBJECT
public:
    using WriteCallback = std::function<void(const QByteArray &data)>;

    explicit TmuxGateway(WriteCallback writeCallback, QObject *parent = nullptr);

    void processLine(const QByteArray &line);

    using CommandCallback = std::function<void(bool success, const QString &response)>;
    void sendCommand(const TmuxCommand &command, CommandCallback callback = nullptr);
    void sendKeys(int paneId, const QByteArray &data);
    void detach();

    // Outstanding-command liveness: if a sent command goes unanswered for
    // longer than this many ms, the control link is treated as hung and
    // unresponsive() is emitted (default 5000, matching iTerm2's
    // TmuxUnresponsiveTimeout). 0 disables. Mainly exposed so tests can use a
    // short timeout.
    void setCommandTimeoutMs(int ms);

    static std::optional<TmuxNotification> parseNotification(const QByteArray &line);
    static QByteArray decodeVisEncoded(const QByteArray &encoded);

Q_SIGNALS:
    void outputReceived(int paneId, const QByteArray &data);
    void layoutChanged(int windowId, const QString &layout, const QString &visibleLayout, bool zoomed);
    void windowAdded(int windowId);
    void windowClosed(int windowId);
    void windowRenamed(int windowId, const QString &name);
    void windowPaneChanged(int windowId, int paneId);
    void sessionChanged(int sessionId, const QString &name);
    void sessionRenamed(const QString &name);
    void sessionsChanged();
    void sessionWindowChanged(int sessionId, int windowId);
    void panePaused(int paneId);
    void paneContinued(int paneId);
    void paneModeChanged(int paneId);
    void clientSessionChanged(const QString &clientName, int sessionId, const QString &sessionName);
    void clientDetached(const QString &clientName);
    void exitReceived(const QString &reason);
    void ready();
    // An outstanding control command went unanswered past the command timeout:
    // the link is hung (e.g. ssh silently dropped). The transport may still
    // look open (no EOF), so this is the only signal a silent hang produces.
    void unresponsive();
    // Traffic resumed after unresponsive() — the link recovered; a UI prompt
    // raised on unresponsive() can dismiss itself.
    void responsive();

private:
    static QByteArray decodeOctalEscapes(const QByteArray &encoded);
    static int parsePaneId(const QByteArray &token);
    static int parseWindowId(const QByteArray &token);
    static int parseSessionId(const QByteArray &token);
    void handleNotification(const QByteArray &line);
    void finishCurrentCommand(bool success);
    void writeToGateway(const QByteArray &data);
    // (Re)start the command-timeout timer iff a command is outstanding;
    // otherwise stop it.
    void updateCommandTimeout();
    // Any line from the server proves the link is alive: clear unresponsive
    // state (emitting responsive()) and push the deadline out.
    void noteServerActivity();
    void onCommandTimeout();
    bool hasOutstandingCommand() const
    {
        return _inResponseBlock || !_pendingCommands.isEmpty();
    }

    WriteCallback _writeCallback;
    bool _exited = false;
    bool _ready = false;

    struct PendingCommand {
        QString command;
        CommandCallback callback;
        QString response;
        int commandId = -1;
    };
    QQueue<PendingCommand> _pendingCommands;
    bool _inResponseBlock = false;
    bool _serverOriginated = false;
    PendingCommand _currentCommand;

    QTimer *_commandTimeoutTimer = nullptr;
    int _commandTimeoutMs = 5000;
    bool _unresponsive = false;
};

} // namespace Konsole

#endif // TMUXGATEWAY_H
