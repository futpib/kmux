/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ToolboxListParser.h"

#include <QRegularExpression>

namespace Konsole
{

QStringList parseToolboxContainerNames(const QString &output)
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
        if (!sawHeader) {
            if (lower.startsWith(QLatin1String("container id")) || lower.startsWith(QLatin1String("id"))) {
                sawHeader = true;
            }
            continue;
        }

        const QStringList columns = trimmed.split(whitespace, Qt::SkipEmptyParts);
        if (columns.size() >= 2) {
            names.append(columns[1]);
        }
    }

    return names;
}

} // namespace Konsole
