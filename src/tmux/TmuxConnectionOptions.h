/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXCONNECTIONOPTIONS_H
#define TMUXCONNECTIONOPTIONS_H

#include <QMetaType>
#include <QString>
#include <QStringList>

namespace Konsole
{

/**
 * User-facing options for opening a tmux server in a new kmux window.
 *
 * Keeping this as structured data avoids turning SSH and tmux arguments back
 * into a shell string when a GUI action uses the same transport as --rsh.
 */
struct TmuxConnectionOptions {
    QString displayName;
    QStringList rshCommand;
    QString tmuxPath;
    QString sessionName;
    QString socketName;
    QString socketPath;
    QString workingDirectory;
};

}

Q_DECLARE_METATYPE(Konsole::TmuxConnectionOptions)

#endif // TMUXCONNECTIONOPTIONS_H
