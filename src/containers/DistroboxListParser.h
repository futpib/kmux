/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef DISTROBOXLISTPARSER_H
#define DISTROBOXLISTPARSER_H

#include <QString>
#include <QStringList>

namespace Konsole
{

QStringList parseDistroboxContainerNames(const QString &output);

}

#endif // DISTROBOXLISTPARSER_H
