/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "DistroboxListParser.h"

#include <QRegularExpression>

namespace Konsole
{

QStringList parseDistroboxContainerNames(const QString &output)
{
    QStringList names;
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    bool sawHeader = false;

    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        const QString lower = trimmed.toLower();
        if (lower.startsWith(QLatin1String("id")) || lower.startsWith(QLatin1String("container"))) {
            sawHeader = true;
            continue;
        }

        if (!sawHeader) {
            continue;
        }

        QString containerName;
        const QStringList pipeColumns = trimmed.split(QLatin1Char('|'));
        if (pipeColumns.size() >= 2) {
            containerName = pipeColumns[1].trimmed();
        } else {
            const QStringList columns = trimmed.split(whitespace, Qt::SkipEmptyParts);
            if (columns.size() >= 2) {
                containerName = columns[1].trimmed();
            }
        }

        if (!containerName.isEmpty()) {
            names.append(containerName);
        }
    }

    return names;
}

} // namespace Konsole
