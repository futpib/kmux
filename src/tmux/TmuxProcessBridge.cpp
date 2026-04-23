/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxProcessBridge.h"

#include "TmuxController.h"
#include "TmuxControllerRegistry.h"
#include "TmuxGateway.h"

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

TmuxProcessBridge::TmuxProcessBridge(ViewManager *viewManager, QObject *parent)
    : QObject(parent)
    , _viewManager(viewManager)
{
}

TmuxProcessBridge::~TmuxProcessBridge()
{
    if (_process && _process->state() != QProcess::NotRunning) {
        qWarning() << "TmuxProcessBridge: destructor terminating tmux process";
        _process->terminate();
        _process->waitForFinished(3000);
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
    QString resolvedTmuxPath = tmuxPath;
    QString executable;
    QStringList leadingArgs;

    if (rshCommand.isEmpty()) {
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
        executable = rshCommand.first();
        leadingArgs = rshCommand.mid(1);
        leadingArgs << resolvedTmuxPath;
    }

    _tmuxPath = resolvedTmuxPath;
    _tmuxArgs = tmuxArgs;
    _command = command;
    _rshCommand = rshCommand;

    // Create a socketpair for tmux's stdout. Sockets have flow control
    // (backpressure) unlike pipes, so tmux's non-blocking writes won't
    // get EAGAIN/EPIPE when the buffer is momentarily full.
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    _socketFd = fds[0]; // parent reads from this end
    int childFd = fds[1]; // child writes to this end (becomes stdout)

    // Set parent end non-blocking for QSocketNotifier
    fcntl(_socketFd, F_SETFL, fcntl(_socketFd, F_GETFL) | O_NONBLOCK);

    _process = new QProcess(this);
    _process->setProcessChannelMode(QProcess::ForwardedOutputChannel);

    // In the child process, replace stdout with our socket
    _process->setChildProcessModifier([childFd, fds]() {
        dup2(childFd, STDOUT_FILENO);
        ::close(childFd);
        ::close(fds[0]);
    });

    _gateway = new TmuxGateway(TmuxGateway::WriteCallback([this](const QByteArray &data) {
                                   if (_process && _process->state() == QProcess::Running) {
                                       _process->write(data);
                                   }
                               }),
                               this);

    _controller = new TmuxController(_gateway, _viewManager, this);

    // Read from our socket, not from QProcess's stdout
    _readNotifier = new QSocketNotifier(_socketFd, QSocketNotifier::Read, this);
    connect(_readNotifier, &QSocketNotifier::activated, this, &TmuxProcessBridge::onReadyRead);
    connect(_process, &QProcess::finished, this, &TmuxProcessBridge::onProcessFinished);

    connect(_gateway, &TmuxGateway::ready, _controller, &TmuxController::initialize);
    connect(_gateway, &TmuxGateway::exitReceived, this, &TmuxProcessBridge::teardown);

    TmuxControllerRegistry::instance()->registerController(_controller);

    QStringList args = leadingArgs;
    args << tmuxArgs;
    if (KonsoleTmuxBridge().isDebugEnabled()) {
        args << QStringLiteral("-vvvv");
    }
    args << QStringLiteral("-C");
    args << command;
    _process->start(executable, args);

    // Close the child's end in the parent after fork
    ::close(childFd);

    if (!_process->waitForStarted(5000)) {
        return false;
    }

    return true;
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

void TmuxProcessBridge::onReadyRead()
{
    char buf[4096];
    for (;;) {
        ssize_t n = read(_socketFd, buf, sizeof(buf));
        if (n > 0) {
            _readBuffer.append(buf, n);
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
        _gateway->processLine(line);
    }
}

void TmuxProcessBridge::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    // Drain any remaining data from the socket before tearing down
    char buf[4096];
    ssize_t total = 0;
    for (;;) {
        ssize_t n = read(_socketFd, buf, sizeof(buf));
        if (n > 0) {
            _readBuffer.append(buf, n);
            total += n;
        } else {
            break;
        }
    }
    qWarning() << "TmuxProcessBridge: process finished, exitCode=" << exitCode << "exitStatus=" << exitStatus << "drained=" << total << "bytes"
               << "readBuffer=" << _readBuffer.left(500);
    teardown();
}

void TmuxProcessBridge::teardown()
{
    if (_controller) {
        _controller->cleanup();
    }
    Q_EMIT disconnected();
}

} // namespace Konsole

#include "moc_TmuxProcessBridge.cpp"
