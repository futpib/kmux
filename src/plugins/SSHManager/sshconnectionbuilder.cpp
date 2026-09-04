/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "sshconnectionbuilder.h"

namespace SSHConnectionBuilder
{

QStringList sshCommand(const SSHConfigurationData &data)
{
    QStringList command = {QStringLiteral("ssh")};
    if (data.useSshConfig) {
        command << data.name;
        return command;
    }

    if (!data.sshKey.isEmpty()) {
        command << QStringLiteral("-i") << data.sshKey;
    }
    if (!data.port.isEmpty()) {
        command << QStringLiteral("-p") << data.port;
    }

    QString destination = data.host;
    if (!data.username.isEmpty()) {
        destination.prepend(data.username + QLatin1Char('@'));
    }
    command << destination;
    return command;
}

Konsole::TmuxConnectionOptions tmuxOptions(const SSHConfigurationData &data)
{
    Konsole::TmuxConnectionOptions options;
    options.displayName = data.name;
    options.rshCommand = sshCommand(data);
    options.tmuxPath = data.tmuxPath;
    options.sessionName = data.tmuxSession;
    options.socketName = data.tmuxSocketName;
    options.socketPath = data.tmuxSocketPath;
    options.workingDirectory = data.remoteWorkingDirectory;
    return options;
}

}
