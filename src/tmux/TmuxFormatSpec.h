/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXFORMATSPEC_H
#define TMUXFORMATSPEC_H

#include <QList>
#include <QString>
#include <QStringList>

#include "konsoleprivate_export.h"

namespace Konsole
{

// Builds tmux `-F "..."` format strings and parses their responses without
// the schema-fragility of ad-hoc tab/space delimiters. Each spec carries a
// per-instance random nonce as the field/record boundary, so user-controlled
// values (window names, pane titles, paths) can never collide with the
// separator and shift fields. Construct once per call site and reuse.
class KONSOLEPRIVATE_EXPORT TmuxFormatSpec
{
public:
    class Row
    {
    public:
        // Returns the field's raw value. Empty if the row is invalid or the
        // field name isn't in this spec — callers that distinguish "missing"
        // from "empty value" need to check the spec separately.
        QString value(const QString &fieldName) const;

    private:
        friend class TmuxFormatSpec;
        const TmuxFormatSpec *_spec = nullptr;
        QStringList _values;
    };

    // Field names are tmux format variables WITHOUT the `#{...}` braces,
    // e.g. `"session_id"`, `"pane_title"`. Order is preserved in the format
    // string but accessed by name in Row, so reordering doesn't shift values.
    explicit TmuxFormatSpec(QStringList fieldNames);

    // The string to pass as tmux's `-F` argument.
    QString tmuxFormatString() const;

    // Parses a tmux command response into rows. Records that don't split into
    // exactly the expected number of fields are dropped — they shouldn't
    // happen unless tmux misbehaves, since the boundary is a per-spec nonce.
    QList<Row> parseRows(const QString &response) const;

private:
    QStringList _fields;
    QString _fieldSep;
    QString _recordSep;
};

} // namespace Konsole

#endif // TMUXFORMATSPEC_H
