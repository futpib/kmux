/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxFormatSpec.h"

#include <QRandomGenerator>

namespace Konsole
{

namespace
{
QString makeBoundary(QLatin1String role, quint32 nonce)
{
    // Printable ASCII only, no `#` (tmux's format-string escape char) and no
    // quote/backslash that could clash with the surrounding `-F "..."`. Long
    // enough that even a one-byte mutation in transit can't accidentally
    // produce a substring that splits a record.
    return QStringLiteral("__KMUX_%1_%2__").arg(role).arg(nonce, 8, 16, QLatin1Char('0'));
}
} // namespace

TmuxFormatSpec::TmuxFormatSpec(QStringList fieldNames)
    : _fields(std::move(fieldNames))
{
    auto *rng = QRandomGenerator::system();
    _fieldSep = makeBoundary(QLatin1String("FS"), rng->generate());
    _recordSep = makeBoundary(QLatin1String("RS"), rng->generate());
}

QString TmuxFormatSpec::tmuxFormatString() const
{
    QString s;
    for (int i = 0; i < _fields.size(); ++i) {
        if (i > 0) {
            s += _fieldSep;
        }
        s += QStringLiteral("#{") + _fields[i] + QLatin1Char('}');
    }
    // Explicit record terminator — don't rely on tmux's automatic `\n` after
    // each format result, since field values may legitimately contain newlines.
    s += _recordSep;
    return s;
}

QList<TmuxFormatSpec::Row> TmuxFormatSpec::parseRows(const QString &response) const
{
    QList<Row> rows;
    const QStringList records = response.split(_recordSep);
    for (const QString &rec : records) {
        // tmux still appends `\n` after each `-F` result, which lands as a
        // leading newline on the *next* record after we split on the record
        // separator. Strip just `\n`/`\r` (not all whitespace) so legitimate
        // leading tabs/spaces inside the first field aren't lost.
        int begin = 0;
        int end = rec.size();
        while (begin < end && (rec[begin] == QLatin1Char('\n') || rec[begin] == QLatin1Char('\r'))) {
            ++begin;
        }
        while (end > begin && (rec[end - 1] == QLatin1Char('\n') || rec[end - 1] == QLatin1Char('\r'))) {
            --end;
        }
        if (begin == end) {
            continue;
        }
        const QString trimmed = rec.mid(begin, end - begin);
        QStringList values = trimmed.split(_fieldSep);
        if (values.size() != _fields.size()) {
            continue;
        }
        Row row;
        row._spec = this;
        row._values = std::move(values);
        rows.append(std::move(row));
    }
    return rows;
}

QString TmuxFormatSpec::Row::value(const QString &fieldName) const
{
    if (!_spec) {
        return {};
    }
    const int idx = _spec->_fields.indexOf(fieldName);
    if (idx < 0 || idx >= _values.size()) {
        return {};
    }
    return _values[idx];
}

} // namespace Konsole
