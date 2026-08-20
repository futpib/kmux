/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxReconnectPolicy.h"

#include <QLoggingCategory>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(KonsoleTmuxReconnect, "konsole.tmux.reconnect", QtWarningMsg)

namespace Konsole
{

namespace
{
constexpr int maxLocalAutoReconnects = 5;
constexpr int ttyPasswordHintMs = 2000;
}

TmuxReconnectPolicy::TmuxReconnectPolicy(QObject *parent)
    : QObject(parent)
{
    _handshakeTimer = new QTimer(this);
    _handshakeTimer->setSingleShot(true);
    connect(_handshakeTimer, &QTimer::timeout, this, &TmuxReconnectPolicy::onHandshakeTimeout);
    _ttyHintTimer = new QTimer(this);
    _ttyHintTimer->setSingleShot(true);
    connect(_ttyHintTimer, &QTimer::timeout, this, &TmuxReconnectPolicy::onTtyPasswordHint);
}

TmuxReconnectPolicy::State TmuxReconnectPolicy::state() const
{
    return _state;
}

bool TmuxReconnectPolicy::isReady() const
{
    return _state == State::Live || _state == State::Hung;
}

bool TmuxReconnectPolicy::isReconnecting() const
{
    return _state == State::Reconnecting;
}

void TmuxReconnectPolicy::setHasRsh(bool hasRsh)
{
    _hasRsh = hasRsh;
}

void TmuxReconnectPolicy::setHasSessionName(bool hasSessionName)
{
    _hasSessionName = hasSessionName;
}

void TmuxReconnectPolicy::setExplicitDetach(bool explicitDetach)
{
    _explicitDetach = explicitDetach;
}

void TmuxReconnectPolicy::setHandshakeTimeoutMs(int ms)
{
    _handshakeTimeoutMs = ms;
}

void TmuxReconnectPolicy::onSpawnStarted()
{
    if (_state != State::Reconnecting) {
        _state = State::Starting;
    }
    _sawExit = false;
}

void TmuxReconnectPolicy::onReady()
{
    _state = State::Live;
    _autoReconnectAttempts = 0;
    _manualReconnect = false;
    _killThenReconnect = false;
    _sawExit = false;
    _explicitDetach = false;
    stopWatchdogs();
    Q_EMIT bannerChanged(TmuxConnectionBanner::Hidden);
}

void TmuxReconnectPolicy::onExitNotification()
{
    _sawExit = true;
}

void TmuxReconnectPolicy::onUnresponsive()
{
    if (_state != State::Live && _state != State::Hung) {
        return;
    }
    _state = State::Hung;
    Q_EMIT bannerChanged(TmuxConnectionBanner::Unresponsive);
}

void TmuxReconnectPolicy::onResponsive()
{
    if (_state != State::Hung) {
        return;
    }
    _state = State::Live;
    Q_EMIT bannerChanged(TmuxConnectionBanner::Hidden);
}

void TmuxReconnectPolicy::onProcessFinished(const QString &reason)
{
    switch (_state) {
    case State::Reconnecting:
        onHandshakeFailed(reason);
        return;
    case State::Starting:
        Q_EMIT firstLaunchFailed(reason);
        return;
    case State::Live:
    case State::Hung:
        break;
    case State::WaitingForRetry:
    case State::Dead:
        return;
    }

    if (_killThenReconnect) {
        _killThenReconnect = false;
        startReconnect();
        return;
    }

    if (_explicitDetach || _sawExit || !_hasSessionName) {
        _state = State::Dead;
        stopWatchdogs();
        Q_EMIT teardownRequested();
        return;
    }

    if (shouldAutoReconnect()) {
        startReconnect();
        return;
    }

    _state = State::WaitingForRetry;
    Q_EMIT bannerChanged(TmuxConnectionBanner::Disconnected);
}

void TmuxReconnectPolicy::onHandshakeFailed(const QString &reason)
{
    qCWarning(KonsoleTmuxReconnect) << "tmux reconnect failed:" << reason;
    stopWatchdogs();

    if (looksLikeSessionGone(reason)) {
        _state = State::Dead;
        Q_EMIT teardownRequested();
        return;
    }

    if (shouldAutoReconnect()) {
        _state = State::WaitingForRetry;
        Q_EMIT bannerChanged(TmuxConnectionBanner::Reconnecting);
        scheduleAutoReconnect();
        return;
    }

    _state = State::WaitingForRetry;
    Q_EMIT bannerChanged(TmuxConnectionBanner::Disconnected);
}

void TmuxReconnectPolicy::requestReconnect(bool processStillRunning)
{
    if (_state == State::Reconnecting || _state == State::Dead) {
        return;
    }
    if (!_hasSessionName) {
        return;
    }
    _manualReconnect = true;
    _autoReconnectAttempts = 0;
    if (processStillRunning) {
        _killThenReconnect = true;
        Q_EMIT killProcessRequested();
        return;
    }
    startReconnect();
}

void TmuxReconnectPolicy::armReconnectWatchdogs()
{
    if (_state != State::Reconnecting) {
        return;
    }
    // First launch waits forever so --rsh can prompt on the controlling TTY.
    // Reconnect does the same when that TTY still exists. No TTY keeps the
    // deadline so "Reconnecting…" cannot hang forever.
    if (_handshakeTimeoutMs > 0 && !hasControllingTty()) {
        _handshakeTimer->start(_handshakeTimeoutMs);
    }
    _ttyHintTimer->stop();
    if (_hasRsh && hasControllingTty()) {
        _ttyHintTimer->start(ttyPasswordHintMs);
    }
}

void TmuxReconnectPolicy::abort()
{
    stopWatchdogs();
    _killThenReconnect = false;
    _state = State::Dead;
}

void TmuxReconnectPolicy::startReconnect()
{
    _state = State::Reconnecting;
    ++_autoReconnectAttempts;
    Q_EMIT bannerChanged(TmuxConnectionBanner::Reconnecting);
    Q_EMIT reconnectRequested();
}

void TmuxReconnectPolicy::scheduleAutoReconnect()
{
    const int shift = qBound(0, _autoReconnectAttempts - 1, 3);
    const int delayMs = 1000 * (1 << shift);
    QTimer::singleShot(delayMs, this, [this]() {
        if (_state == State::Live || _state == State::Dead || !_hasSessionName) {
            return;
        }
        startReconnect();
    });
}

bool TmuxReconnectPolicy::shouldAutoReconnect() const
{
    if (_manualReconnect || !_hasSessionName) {
        return false;
    }
    if (_hasRsh) {
        return _autoReconnectAttempts == 0;
    }
    return _autoReconnectAttempts < maxLocalAutoReconnects;
}

bool TmuxReconnectPolicy::hasControllingTty()
{
    if (qEnvironmentVariableIntValue("KMUX_ASSUME_CONTROLLING_TTY") > 0) {
        return true;
    }
    const int fd = ::open("/dev/tty", O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        return false;
    }
    ::close(fd);
    return true;
}

bool TmuxReconnectPolicy::looksLikeSessionGone(const QString &reason)
{
    const QString lower = reason.toLower();
    return lower.contains(QLatin1String("no sessions")) || lower.contains(QLatin1String("no current target"))
        || lower.contains(QLatin1String("can't find session")) || lower.contains(QLatin1String("no such session"))
        || lower.contains(QLatin1String("session not found"));
}

void TmuxReconnectPolicy::stopWatchdogs()
{
    _handshakeTimer->stop();
    _ttyHintTimer->stop();
}

void TmuxReconnectPolicy::onHandshakeTimeout()
{
    if (isReady()) {
        return;
    }
    _ttyHintTimer->stop();
    qCWarning(KonsoleTmuxReconnect) << "tmux reconnect handshake timed out after" << _handshakeTimeoutMs << "ms";
    Q_EMIT killProcessRequested();
}

void TmuxReconnectPolicy::onTtyPasswordHint()
{
    if (_state != State::Reconnecting || isReady()) {
        return;
    }
    Q_EMIT bannerChanged(TmuxConnectionBanner::ReconnectingCheckTty);
}

}

#include "moc_TmuxReconnectPolicy.cpp"
