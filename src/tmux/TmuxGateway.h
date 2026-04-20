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

private:
    static QByteArray decodeOctalEscapes(const QByteArray &encoded);
    static int parsePaneId(const QByteArray &token);
    static int parseWindowId(const QByteArray &token);
    static int parseSessionId(const QByteArray &token);
    void handleNotification(const QByteArray &line);
    void finishCurrentCommand(bool success);
    void writeToGateway(const QByteArray &data);

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
};

} // namespace Konsole

#endif // TMUXGATEWAY_H
