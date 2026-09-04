/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef SSHCONNECTIONBUILDER_H
#define SSHCONNECTIONBUILDER_H

#include "sshconfigurationdata.h"

#include "tmux/TmuxConnectionOptions.h"

#include <QStringList>

namespace SSHConnectionBuilder
{

QStringList sshCommand(const SSHConfigurationData &data);
Konsole::TmuxConnectionOptions tmuxOptions(const SSHConfigurationData &data);

}

#endif // SSHCONNECTIONBUILDER_H
