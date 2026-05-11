/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TOOLBOXLISTPARSER_H
#define TOOLBOXLISTPARSER_H

#include <QString>
#include <QStringList>

namespace Konsole
{

QStringList parseToolboxContainerNames(const QString &output);

}

#endif // TOOLBOXLISTPARSER_H
