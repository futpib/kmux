/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXPROCESSBRIDGE_H
#define TMUXPROCESSBRIDGE_H

#include <QObject>
#include <QProcess>
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

    bool start(const QString &tmuxPath = QString(), const QStringList &extraArgs = {});

    TmuxController *controller() const;

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
    QByteArray _readBuffer;
};

} // namespace Konsole

#endif // TMUXPROCESSBRIDGE_H
