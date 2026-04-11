/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxProcessBridge.h"

#include "TmuxController.h"
#include "TmuxControllerRegistry.h"
#include "TmuxGateway.h"

#include <QStandardPaths>

namespace Konsole
{

TmuxProcessBridge::TmuxProcessBridge(ViewManager *viewManager, QObject *parent)
    : QObject(parent)
    , _viewManager(viewManager)
{
}

TmuxProcessBridge::~TmuxProcessBridge()
{
    if (_controller) {
        TmuxControllerRegistry::instance()->unregisterController(_controller);
    }
    if (_process && _process->state() != QProcess::NotRunning) {
        _process->terminate();
        _process->waitForFinished(3000);
    }
}

bool TmuxProcessBridge::start(const QString &tmuxPath, const QStringList &extraArgs)
{
    QString path = tmuxPath;
    if (path.isEmpty()) {
        path = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    }
    if (path.isEmpty()) {
        return false;
    }

    _process = new QProcess(this);
    _process->setProcessChannelMode(QProcess::SeparateChannels);

    _gateway = new TmuxGateway(TmuxGateway::WriteCallback([this](const QByteArray &data) {
                                   if (_process && _process->state() == QProcess::Running) {
                                       _process->write(data);
                                   }
                               }),
                               this);

    _controller = new TmuxController(_gateway, nullptr, _viewManager, this);

    connect(_process, &QProcess::readyReadStandardOutput, this, &TmuxProcessBridge::onReadyRead);
    connect(_process, &QProcess::finished, this, &TmuxProcessBridge::onProcessFinished);

    connect(_gateway, &TmuxGateway::ready, _controller, &TmuxController::initialize);
    connect(_gateway, &TmuxGateway::exitReceived, this, &TmuxProcessBridge::teardown);

    TmuxControllerRegistry::instance()->registerController(_controller);

    QStringList args = extraArgs;
    args << QStringLiteral("-C") << QStringLiteral("new-session") << QStringLiteral("-A");
    _process->start(path, args);
    if (!_process->waitForStarted(5000)) {
        return false;
    }

    return true;
}

TmuxController *TmuxProcessBridge::controller() const
{
    return _controller;
}

void TmuxProcessBridge::onReadyRead()
{
    _readBuffer.append(_process->readAllStandardOutput());

    int pos;
    while ((pos = _readBuffer.indexOf('\n')) != -1) {
        QByteArray line = _readBuffer.left(pos);
        _readBuffer.remove(0, pos + 1);
        _gateway->processLine(line);
    }
}

void TmuxProcessBridge::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitCode)
    Q_UNUSED(exitStatus)
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
