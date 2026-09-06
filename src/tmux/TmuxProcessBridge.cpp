/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxProcessBridge.h"

#include "TmuxConnectionBanner.h"
#include "TmuxController.h"
#include "TmuxControllerRegistry.h"
#include "TmuxGateway.h"
#include "TmuxReconnectPolicy.h"

#include "ViewManager.h"
#include "session/Session.h"
#include "terminalDisplay/TerminalDisplay.h"

#include <QDir>
#include <QLatin1StringView>
#include <QLoggingCategory>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(KonsoleTmuxBridge, "konsole.tmux.bridge", QtWarningMsg)

namespace Konsole
{

constexpr QLatin1StringView CONTROL_CLIENT_TERM("xterm-256color");

TmuxProcessBridge::TmuxProcessBridge(ViewManager *viewManager, QObject *parent)
    : QObject(parent)
    , _viewManager(viewManager)
{
    _policy = new TmuxReconnectPolicy(this);
    connect(_policy, &TmuxReconnectPolicy::bannerChanged, this, &TmuxProcessBridge::setViewsConnectionBanner);
    connect(_policy, &TmuxReconnectPolicy::reconnectRequested, this, &TmuxProcessBridge::spawnReconnectClient);
    connect(_policy, &TmuxReconnectPolicy::killProcessRequested, this, &TmuxProcessBridge::killControlProcess);
    connect(_policy, &TmuxReconnectPolicy::teardownRequested, this, &TmuxProcessBridge::teardownSession);
    connect(_policy, &TmuxReconnectPolicy::firstLaunchFailed, this, &TmuxProcessBridge::startupFailed);
}

TmuxProcessBridge::~TmuxProcessBridge()
{
    disconnect(_policy, nullptr, this, nullptr);
    _policy->abort();
    _ignoringProcessFinished = true;
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

bool TmuxProcessBridge::start(const QString &tmuxPath,
                              const QStringList &tmuxArgs,
                              const QStringList &command,
                              const QStringList &rshCommand,
                              const QProcessEnvironment &processEnvironment)
{
    _tmuxPath = tmuxPath;
    _tmuxArgs = tmuxArgs;
    _command = command;
    _rshCommand = rshCommand;
    _processEnvironment = processEnvironment.isEmpty() ? QProcessEnvironment::systemEnvironment() : processEnvironment;
    _policy->setHasRsh(!_rshCommand.isEmpty());

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
        // SSH does not forward TERM without a PTY. The control connection has
        // no PTY by design, so set the terminal type in the remote command too.
        leadingArgs << QStringLiteral("env") << QStringLiteral("TERM=%1").arg(CONTROL_CLIENT_TERM) << resolvedTmuxPath;
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
    // tmux records the control process's TERM as client_termname. kmux has no
    // PTY here, but it presents the same xterm-compatible terminal as a normal
    // Konsole profile; leaving TERM absent or inherited as "dumb" makes clients
    // such as Codex reject an otherwise fully capable pane.
    _processEnvironment.insert(QStringLiteral("TERM"), CONTROL_CLIENT_TERM.toString());
    _process->setProcessEnvironment(_processEnvironment);
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
    _policy->onSpawnStarted();
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
        _startupOutput.clear();
        if (_controller) {
            _controller->clearExplicitDetach();
        }
        _policy->onReady();
    });
    connect(_gateway, &TmuxGateway::ready, this, &TmuxProcessBridge::ready);
    connect(_gateway, &TmuxGateway::exitReceived, this, [this](const QString &) {
        _policy->onExitNotification();
    });
    connect(_gateway, &TmuxGateway::unresponsive, _policy, &TmuxReconnectPolicy::onUnresponsive);
    connect(_gateway, &TmuxGateway::responsive, _policy, &TmuxReconnectPolicy::onResponsive);
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

QProcessEnvironment TmuxProcessBridge::processEnvironment() const
{
    return _processEnvironment;
}

void TmuxProcessBridge::setHandshakeTimeoutMs(int ms)
{
    _policy->setHandshakeTimeoutMs(ms);
}

QString TmuxProcessBridge::learnedSessionName() const
{
    return _controller ? _controller->sessionName() : QString();
}

void TmuxProcessBridge::syncPolicySessionFacts()
{
    _policy->setHasSessionName(!learnedSessionName().isEmpty());
    _policy->setExplicitDetach(_controller && _controller->explicitDetach());
    _policy->setHasRsh(!_rshCommand.isEmpty());
}

void TmuxProcessBridge::onReadyRead()
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(_socketFd, buf, sizeof(buf));
        if (n > 0) {
            _readBuffer.append(buf, n);
            if (!_policy->isReady()) {
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

QString TmuxProcessBridge::processExitReason(int exitCode, QProcess::ExitStatus exitStatus) const
{
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
    return reason;
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
            if (!_policy->isReady()) {
                _startupOutput.append(buf, n);
            }
            total += n;
        } else {
            break;
        }
    }
    qWarning() << "TmuxProcessBridge: process finished, exitCode=" << exitCode << "exitStatus=" << exitStatus << "drained=" << total << "bytes"
               << "readBuffer=" << _readBuffer.left(500);

    syncPolicySessionFacts();
    _policy->onProcessFinished(processExitReason(exitCode, exitStatus));
}

void TmuxProcessBridge::teardownSession()
{
    if (_controller) {
        _controller->cleanup();
    }
    Q_EMIT disconnected();
}

void TmuxProcessBridge::teardownTransport()
{
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
    _readBuffer.clear();
}

void TmuxProcessBridge::requestReconnect()
{
    syncPolicySessionFacts();
    const bool running = _process && _process->state() != QProcess::NotRunning;
    _policy->requestReconnect(running);
}

void TmuxProcessBridge::spawnReconnectClient()
{
    const QString sessionName = learnedSessionName();
    if (sessionName.isEmpty() || _controller == nullptr) {
        teardownSession();
        return;
    }

    teardownTransport();

    const QStringList attachCmd = {QStringLiteral("attach-session"), QStringLiteral("-t"), sessionName};
    if (!spawnProcess(attachCmd)) {
        _policy->onHandshakeFailed(QStringLiteral("failed to spawn tmux client"));
        return;
    }
    createGateway(false);
    _policy->armReconnectWatchdogs();
}

void TmuxProcessBridge::killControlProcess()
{
    if (_process && _process->state() != QProcess::NotRunning) {
        _process->kill();
        return;
    }
    _policy->onHandshakeFailed(QStringLiteral("handshake timed out"));
}

void TmuxProcessBridge::setViewsConnectionBanner(TmuxConnectionBanner banner)
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
