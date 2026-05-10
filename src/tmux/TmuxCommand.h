/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXCOMMAND_H
#define TMUXCOMMAND_H

#include <QString>
#include <QStringList>

#include "TmuxFormatSpec.h"

namespace Konsole
{

class TmuxCommand
{
public:
    explicit TmuxCommand(const QString &verb)
        : _verb(verb)
    {
    }

    TmuxCommand &paneTarget(int paneId)
    {
        _parts.append(QStringLiteral("-t %") + QString::number(paneId));
        return *this;
    }

    TmuxCommand &windowTarget(int windowId)
    {
        _parts.append(QStringLiteral("-t @") + QString::number(windowId));
        return *this;
    }

    // Scope the command to a specific tmux session (e.g. `list-windows -t $N`
    // for "windows in session N"). Prefer this over allSessions() whenever the
    // caller really wants state for *one* session — tmux's `-a` iterates
    // sessions in lex order by name, and the *first* row matching a filter
    // wins, which silently mis-targets when multiple sessions are open.
    // Note: `list-panes` additionally needs an explicit `-s` flag to interpret
    // the target as a session rather than a window — chain `flag("-s")` first.
    TmuxCommand &sessionTarget(int sessionId)
    {
        _parts.append(QStringLiteral("-t $") + QString::number(sessionId));
        return *this;
    }

    TmuxCommand &paneSource(int paneId)
    {
        _parts.append(QStringLiteral("-s %") + QString::number(paneId));
        return *this;
    }

    // Cross-session scope (`-a`). Use sparingly: when the caller really wants
    // to enumerate every session on the server (e.g. the tree switcher's
    // session chooser). For "give me state about *my* session" use
    // sessionTarget() instead.
    TmuxCommand &allSessions()
    {
        _parts.append(QStringLiteral("-a"));
        return *this;
    }

    TmuxCommand &flag(const QString &f)
    {
        _parts.append(f);
        return *this;
    }

    TmuxCommand &format(const QString &fmt)
    {
        _parts.append(QStringLiteral("-F \"") + fmt + QStringLiteral("\""));
        return *this;
    }

    // Prefer this overload over the raw-string one: TmuxFormatSpec uses a
    // unique nonce as the field/record boundary, so user-controlled values
    // (window names, pane titles, paths) can't shift the schema by smuggling
    // in the separator character.
    TmuxCommand &format(const TmuxFormatSpec &spec)
    {
        return format(spec.tmuxFormatString());
    }

    TmuxCommand &quotedArg(const QString &value)
    {
        _parts.append(QLatin1Char('"') + value + QLatin1Char('"'));
        return *this;
    }

    TmuxCommand &singleQuotedArg(const QString &value)
    {
        _parts.append(QLatin1Char('\'') + value + QLatin1Char('\''));
        return *this;
    }

    TmuxCommand &arg(const QString &value)
    {
        _parts.append(value);
        return *this;
    }

    QString build() const
    {
        QString result = _verb;
        for (const QString &part : _parts) {
            result += QLatin1Char(' ') + part;
        }
        return result;
    }

private:
    QString _verb;
    QStringList _parts;
};

} // namespace Konsole

#endif // TMUXCOMMAND_H
