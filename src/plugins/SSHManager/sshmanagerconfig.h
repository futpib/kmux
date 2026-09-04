/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef SSHMANAGERCONFIG_H
#define SSHMANAGERCONFIG_H

#include <QString>

namespace SSHManagerConfig
{

inline QString fileName()
{
    return QStringLiteral("kmuxsshconfig");
}

}

#endif // SSHMANAGERCONFIG_H
