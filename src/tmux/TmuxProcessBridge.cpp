/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxProcessBridge.h"

#include "TmuxController.h"
#include "TmuxControllerRegistry.h"
#include "TmuxGateway.h"

#include "ViewManager.h"
#include "session/Session.h"
#include "terminalDisplay/TerminalDisplay.h"

#include <QDir>
#include <QLoggingCategory>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(KonsoleTmuxBridge, "konsole.tmux.bridge", QtWarningMsg)

namespace Konsole
{

namespace
{
constexpr int maxLocalAutoReconnects = 5;
}

TmuxProcessBridge::TmuxProcessBridge(ViewManager *viewManager, QObject *parent)
    : QObject(parent)
    , _viewManager(viewManager)
{
    _handshakeTimer = new QTimer(this);
    _handshakeTimer->setSingleShot(true);
    connect(_handshakeTimer, &QTimer::timeout, this, &TmuxProcessBridge::onHandshakeTimeout);
    _ttyHintTimer = new QTimer(this);
    _ttyHintTimer->setSingleShot(true);
    connect(_ttyHintTimer, &QTimer::timeout, this, &TmuxProcessBridge::onTtyPasswordHint);
}

TmuxProcessBridge::~TmuxProcessBridge()
{
    _handshakeTimer->stop();
    _ttyHintTimer->stop();
    _ignoringProcessFinished = true;
    _reconnectInProgress = false;
    if (_process) {
        disconnect(_process, nullptr, this, nullptr);
        if (_process->state() != QProcess::NotRunning) {
            qWarning() << "TmuxProcessBridge: destructor terminating tmux process";
            _process->terminate();
            _process->waitForFinished(3000);
        }
    }
    if (_controller) {
        _controller->cleanup();
        TmuxControllerRegistry::instance()->unregisterController(_controller);
    }
    if (_socketFd >= 0) {
        close(_socketFd);
        _socketFd = -1;
    }
}

bool TmuxProcessBridge::start(const QString &tmuxPath, const QStringList &tmuxArgs, const QStringList &command, const QStringList &rshCommand)
{
    _tmuxPath = tmuxPath;
    _tmuxArgs = tmuxArgs;
    _command = command;
    _rshCommand = rshCommand;

    if (!spawnProcess(command)) {
        return false;
    }
    createGateway(true);
    TmuxControllerRegistry::instance()->registerController(_controller);
    return true;
}

bool TmuxProcessBridge::spawnProcess(const QStringList &command)
{
    QString resolvedTmuxPath = _tmuxPath;
    QString executable;
    QStringList leadingArgs;

    if (_rshCommand.isEmpty()) {
        if (resolvedTmuxPath.isEmpty()) {
            resolvedTmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
        }
        if (resolvedTmuxPath.isEmpty()) {
            return false;
        }
        executable = resolvedTmuxPath;
    } else {
        // tmuxPath refers to the binary on the remote host; resolve only to a
        // bare default, never probe PATH (that would check the local host).
        if (resolvedTmuxPath.isEmpty()) {
            resolvedTmuxPath = QStringLiteral("tmux");
        }
        executable = _rshCommand.first();
        leadingArgs = _rshCommand.mid(1);
        leadingArgs << resolvedTmuxPath;
    }

    _tmuxPath = resolvedTmuxPath;

    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    _socketFd = fds[0];
    int childFd = fds[1];

    fcntl(_socketFd, F_SETFL, fcntl(_socketFd, F_GETFL) | O_NONBLOCK);

    _process = new QProcess(this);
    _process->setProcessChannelMode(QProcess::ForwardedOutputChannel);

    _process->setChildProcessModifier([childFd, fds]() {
        dup2(childFd, STDOUT_FILENO);
        ::close(childFd);
        ::close(fds[0]);
    });

    _readNotifier = new QSocketNotifier(_socketFd, QSocketNotifier::Read, this);
    connect(_readNotifier, &QSocketNotifier::activated, this, &TmuxProcessBridge::onReadyRead);
    connect(_process, &QProcess::finished, this, &TmuxProcessBridge::onProcessFinished);

    QStringList args = leadingArgs;
    args << _tmuxArgs;
    if (KonsoleTmuxBridge().isDebugEnabled()) {
        args << QStringLiteral("-vvvv");
        const QString logDir = QStandardPaths::writableLocation(QStandardPaths::GenericStateLocation) + QStringLiteral("/kmux/tmux-logs");
        if (QDir().mkpath(logDir)) {
            _process->setWorkingDirectory(logDir);
        }
    }
    args << QStringLiteral("-u");
    args << QStringLiteral("-C");
    args << command;
    _process->start(executable, args);

    ::close(childFd);

    if (!_process->waitForStarted(5000)) {
        return false;
    }

    _readBuffer.clear();
    _startupOutput.clear();
    _ready = false;
    _gotExitNotification = false;
    return true;
}

void TmuxProcessBridge::createGateway(bool bindController)
{
    _gateway = new TmuxGateway(TmuxGateway::WriteCallback([this](const QByteArray &data) {
                                   if (_process && _process->state() == QProcess::Running) {
                                       _process->write(data);
                                   }
                               }),
                               this);
    if (bindController) {
        _controller = new TmuxController(_gateway, _viewManager, this);
    } else if (_controller) {
        _controller->rebindGateway(_gateway);
    }
    connectGatewayBridgeSignals();
}

void TmuxProcessBridge::connectGatewayBridgeSignals()
{
    connect(_gateway, &TmuxGateway::ready, _controller, &TmuxController::initialize);
    connect(_gateway, &TmuxGateway::ready, this, [this]() {
        _ready = true;
        _reconnectInProgress = false;
        _reconnectRequested = false;
        _manualReconnect = false;
        _autoReconnectAttempts = 0;
        _gotExitNotification = false;
        _startupOutput.clear();
        if (_handshakeTimer) {
            _handshakeTimer->stop();
        }
        if (_ttyHintTimer) {
            _ttyHintTimer->stop();
        }
        if (_controller) {
            _controller->clearExplicitDetach();
        }
        setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner::Hidden);
    });
    connect(_gateway, &TmuxGateway::ready, this, &TmuxProcessBridge::ready);
    connect(_gateway, &TmuxGateway::exitReceived, this, [this](const QString &) {
        _gotExitNotification = true;
    });
    connect(_gateway, &TmuxGateway::unresponsive, this, [this]() {
        setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner::Unresponsive);
    });
    connect(_gateway, &TmuxGateway::responsive, this, [this]() {
        if (!_reconnectInProgress) {
            setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner::Hidden);
        }
    });
}

TmuxController *TmuxProcessBridge::controller() const
{
    return _controller;
}

QString TmuxProcessBridge::tmuxPath() const
{
    return _tmuxPath;
}

QStringList TmuxProcessBridge::tmuxArgs() const
{
    return _tmuxArgs;
}

QStringList TmuxProcessBridge::command() const
{
    return _command;
}

QStringList TmuxProcessBridge::rshCommand() const
{
    return _rshCommand;
}

void TmuxProcessBridge::setHandshakeTimeoutMs(int ms)
{
    _handshakeTimeoutMs = ms;
}

QString TmuxProcessBridge::learnedSessionName() const
{
    return _controller ? _controller->sessionName() : QString();
}

void TmuxProcessBridge::onReadyRead()
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(_socketFd, buf, sizeof(buf));
        if (n > 0) {
            _readBuffer.append(buf, n);
            if (!_ready) {
                _startupOutput.append(buf, n);
            }
        } else {
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                if (_readNotifier) {
                    _readNotifier->setEnabled(false);
                }
            }
            break;
        }
    }

    int pos;
    while ((pos = _readBuffer.indexOf('\n')) != -1) {
        QByteArray line = _readBuffer.left(pos);
        _readBuffer.remove(0, pos + 1);
        qCDebug(KonsoleTmuxBridge) << "<<" << line;
        if (_gateway) {
            _gateway->processLine(line);
        }
    }
}

void TmuxProcessBridge::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (_ignoringProcessFinished) {
        return;
    }

    char buf[4096];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(_socketFd, buf, sizeof(buf));
        if (n > 0) {
            _readBuffer.append(buf, n);
            if (!_ready) {
                _startupOutput.append(buf, n);
            }
            total += n;
        } else {
            break;
        }
    }
    qWarning() << "TmuxProcessBridge: process finished, exitCode=" << exitCode << "exitStatus=" << exitStatus << "drained=" << total << "bytes"
               << "readBuffer=" << _readBuffer.left(500);

    const QString out = QString::fromUtf8(_startupOutput).trimmed();
    const QString err = _process ? QString::fromUtf8(_process->readAllStandardError()).trimmed() : QString();
    QString reason = QStringLiteral("exit code %1").arg(exitCode);
    if (exitStatus == QProcess::CrashExit) {
        reason += QStringLiteral(" (process crashed)");
    }
    if (!out.isEmpty()) {
        reason += QStringLiteral("\n  stdout: %1").arg(out);
    }
    if (!err.isEmpty()) {
        reason += QStringLiteral("\n  stderr: %1").arg(err);
    }
    if (out.isEmpty() && err.isEmpty()) {
        reason += QStringLiteral(" (no output)");
    }

    if (!_ready) {
        if (_reconnectInProgress) {
            onReconnectHandshakeFailed(reason);
            return;
        }
        Q_EMIT startupFailed(reason);
        return;
    }

    if (_reconnectRequested) {
        _reconnectRequested = false;
        beginReconnect();
        return;
    }

    const QString sessionName = learnedSessionName();
    const bool explicitDetach = _controller && _controller->explicitDetach();
    // %exit is a client-side goodbye: prefix-d, Detach from tmux, last pane
    // exited, kill-session. Do not reconnect — that would fight a deliberate
    // detach. Transport drops have no %exit and take the path below.
    if (explicitDetach || _gotExitNotification || sessionName.isEmpty()) {
        teardown();
        return;
    }

    if (shouldAutoReconnect()) {
        beginReconnect();
        return;
    }

    setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner::Disconnected);
}

void TmuxProcessBridge::teardown()
{
    _handshakeTimer->stop();
    _ttyHintTimer->stop();
    _reconnectInProgress = false;
    if (_controller) {
        _controller->cleanup();
    }
    Q_EMIT disconnected();
}

void TmuxProcessBridge::teardownTransport()
{
    _handshakeTimer->stop();
    _ttyHintTimer->stop();
    _ignoringProcessFinished = true;
    if (_readNotifier) {
        _readNotifier->setEnabled(false);
        _readNotifier->setParent(nullptr);
        _readNotifier->deleteLater();
        _readNotifier = nullptr;
    }
    if (_socketFd >= 0) {
        close(_socketFd);
        _socketFd = -1;
    }
    if (_gateway) {
        disconnect(_gateway, nullptr, this, nullptr);
        if (_controller) {
            disconnect(_gateway, nullptr, _controller, nullptr);
        }
        _gateway->setParent(nullptr);
        _gateway->deleteLater();
        _gateway = nullptr;
    }
    if (_process) {
        disconnect(_process, nullptr, this, nullptr);
        if (_process->state() != QProcess::NotRunning) {
            _process->kill();
            _process->waitForFinished(2000);
        }
        _process->setParent(nullptr);
        _process->deleteLater();
        _process = nullptr;
    }
    _ignoringProcessFinished = false;
    _ready = false;
    _readBuffer.clear();
}

void TmuxProcessBridge::requestReconnect()
{
    if (_reconnectInProgress) {
        return;
    }
    if (learnedSessionName().isEmpty()) {
        return;
    }
    _manualReconnect = true;
    _autoReconnectAttempts = 0;
    if (_process && _process->state() != QProcess::NotRunning) {
        _reconnectRequested = true;
        _process->kill();
        return;
    }
    beginReconnect();
}

void TmuxProcessBridge::beginReconnect()
{
    const QString sessionName = learnedSessionName();
    if (sessionName.isEmpty() || _controller == nullptr) {
        teardown();
        return;
    }

    _reconnectInProgress = true;
    ++_autoReconnectAttempts;
    setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner::Reconnecting);

    teardownTransport();

    const QStringList attachCmd = {QStringLiteral("attach-session"), QStringLiteral("-t"), sessionName};
    if (!spawnProcess(attachCmd)) {
        onReconnectHandshakeFailed(QStringLiteral("failed to spawn tmux client"));
        return;
    }
    createGateway(false);
    // First launch already waits forever so an --rsh ssh password prompt can
    // sit on the controlling TTY. Reconnect must do the same when we still
    // have that TTY — otherwise the 8s handshake timer kills ssh mid-prompt.
    // No TTY (.desktop / daemon) keeps the deadline so a hung client cannot
    // sit on "Reconnecting…" forever.
    if (_handshakeTimeoutMs > 0 && !hasControllingTty()) {
        _handshakeTimer->start(_handshakeTimeoutMs);
    }
    scheduleTtyPasswordHint();
}

void TmuxProcessBridge::onHandshakeTimeout()
{
    if (_ready) {
        return;
    }
    _ttyHintTimer->stop();
    qCWarning(KonsoleTmuxBridge) << "tmux reconnect handshake timed out after" << _handshakeTimeoutMs << "ms";
    if (_process && _process->state() != QProcess::NotRunning) {
        _process->kill();
        return;
    }
    onReconnectHandshakeFailed(QStringLiteral("handshake timed out"));
}

void TmuxProcessBridge::scheduleTtyPasswordHint()
{
    _ttyHintTimer->stop();
    if (_rshCommand.isEmpty() || !hasControllingTty()) {
        return;
    }
    // Key/ControlPersist handshakes usually finish in well under this; a
    // password prompt will not. Delay so we don't flash the hint on the
    // common success path.
    _ttyHintTimer->start(2000);
}

void TmuxProcessBridge::onTtyPasswordHint()
{
    if (!_reconnectInProgress || _ready) {
        return;
    }
    setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner::ReconnectingCheckTty);
}

void TmuxProcessBridge::onReconnectHandshakeFailed(const QString &reason)
{
    qCWarning(KonsoleTmuxBridge) << "tmux reconnect failed:" << reason;
    _reconnectInProgress = false;
    _handshakeTimer->stop();
    _ttyHintTimer->stop();

    if (looksLikeSessionGone(reason)) {
        teardown();
        return;
    }

    if (shouldAutoReconnect()) {
        setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner::Reconnecting);
        scheduleAutoReconnect();
        return;
    }

    setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner::Disconnected);
}

void TmuxProcessBridge::scheduleAutoReconnect()
{
    const int shift = qBound(0, _autoReconnectAttempts - 1, 3);
    const int delayMs = 1000 * (1 << shift);
    QTimer::singleShot(delayMs, this, [this]() {
        if (_ready || learnedSessionName().isEmpty()) {
            return;
        }
        beginReconnect();
    });
}

bool TmuxProcessBridge::shouldAutoReconnect() const
{
    if (_manualReconnect || learnedSessionName().isEmpty()) {
        return false;
    }
    if (!_rshCommand.isEmpty()) {
        return _autoReconnectAttempts == 0;
    }
    return _autoReconnectAttempts < maxLocalAutoReconnects;
}

bool TmuxProcessBridge::hasControllingTty()
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

bool TmuxProcessBridge::looksLikeSessionGone(const QString &reason)
{
    const QString lower = reason.toLower();
    return lower.contains(QLatin1String("no sessions")) || lower.contains(QLatin1String("no current target"))
        || lower.contains(QLatin1String("can't find session")) || lower.contains(QLatin1String("no such session"))
        || lower.contains(QLatin1String("session not found"));
}

void TmuxProcessBridge::setViewsTmuxUnresponsive(bool unresponsive)
{
    setViewsConnectionBanner(unresponsive ? TerminalDisplay::TmuxConnectionBanner::Unresponsive : TerminalDisplay::TmuxConnectionBanner::Hidden);
}

void TmuxProcessBridge::setViewsConnectionBanner(TerminalDisplay::TmuxConnectionBanner banner)
{
    if (_viewManager == nullptr) {
        return;
    }
    const auto sessions = _viewManager->sessions();
    for (Session *session : sessions) {
        const auto views = session->views();
        for (TerminalDisplay *view : views) {
            view->setTmuxConnectionBanner(banner);
            connect(view, &TerminalDisplay::tmuxReconnectRequested, this, &TmuxProcessBridge::requestReconnect, Qt::UniqueConnection);
        }
    }
}

} // namespace Konsole

#include "moc_TmuxProcessBridge.cpp"
