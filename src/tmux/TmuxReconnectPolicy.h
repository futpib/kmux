/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXRECONNECTPOLICY_H
#define TMUXRECONNECTPOLICY_H

#include <QObject>
#include <QString>

#include "TmuxConnectionBanner.h"

class QTimer;

namespace Konsole
{

/**
 * Decides what to do when a tmux control client drops, hangs, or is asked
 * to retry. Owns handshake/TTY-hint timers and the reconnect state machine.
 * Does not spawn processes or touch widgets.
 */
class TmuxReconnectPolicy : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Starting,
        Live,
        Hung,
        Reconnecting,
        WaitingForRetry,
        Dead,
    };

    explicit TmuxReconnectPolicy(QObject *parent = nullptr);

    State state() const;
    bool isReady() const;
    bool isReconnecting() const;

    void setHasRsh(bool hasRsh);
    void setHasSessionName(bool hasSessionName);
    void setExplicitDetach(bool explicitDetach);
    void setHandshakeTimeoutMs(int ms);

    void onSpawnStarted();
    void onReady();
    void onExitNotification();
    void onUnresponsive();
    void onResponsive();
    void onProcessFinished(const QString &reason);
    void onHandshakeFailed(const QString &reason);
    void requestReconnect(bool processStillRunning);
    void armReconnectWatchdogs();
    void abort();

Q_SIGNALS:
    void bannerChanged(TmuxConnectionBanner banner);
    void reconnectRequested();
    void killProcessRequested();
    void teardownRequested();
    void firstLaunchFailed(const QString &reason);

private:
    void startReconnect();
    void scheduleAutoReconnect();
    bool shouldAutoReconnect() const;
    static bool hasControllingTty();
    static bool looksLikeSessionGone(const QString &reason);
    void stopWatchdogs();
    void onHandshakeTimeout();
    void onTtyPasswordHint();

    State _state = State::Starting;
    bool _hasRsh = false;
    bool _hasSessionName = false;
    bool _explicitDetach = false;
    bool _sawExit = false;
    bool _manualReconnect = false;
    bool _killThenReconnect = false;
    int _autoReconnectAttempts = 0;
    int _handshakeTimeoutMs = 8000;
    QTimer *_handshakeTimer = nullptr;
    QTimer *_ttyHintTimer = nullptr;
};

}

#endif // TMUXRECONNECTPOLICY_H
