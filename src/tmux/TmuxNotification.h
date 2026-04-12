/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXNOTIFICATION_H
#define TMUXNOTIFICATION_H

#include <QByteArray>
#include <QString>

#include <variant>

namespace Konsole
{

struct TmuxOutputNotification {
    int paneId;
    QByteArray data;
};

struct TmuxLayoutChangedNotification {
    int windowId;
    QString layout;
    QString visibleLayout;
    bool zoomed;
};

struct TmuxWindowAddedNotification {
    int windowId;
};

struct TmuxWindowClosedNotification {
    int windowId;
};

struct TmuxWindowRenamedNotification {
    int windowId;
    QString name;
};

struct TmuxWindowPaneChangedNotification {
    int windowId;
    int paneId;
};

struct TmuxSessionChangedNotification {
    int sessionId;
    QString name;
};

struct TmuxSessionRenamedNotification {
    QString name;
};

struct TmuxSessionsChangedNotification {
};

struct TmuxSessionWindowChangedNotification {
    int sessionId;
    int windowId;
};

struct TmuxPanePausedNotification {
    int paneId;
};

struct TmuxPaneContinuedNotification {
    int paneId;
};

struct TmuxClientSessionChangedNotification {
    QString clientName;
    int sessionId;
    QString sessionName;
};

struct TmuxClientDetachedNotification {
    QString clientName;
};

struct TmuxExitNotification {
    QString reason;
};

using TmuxNotification = std::variant<TmuxOutputNotification,
                                      TmuxLayoutChangedNotification,
                                      TmuxWindowAddedNotification,
                                      TmuxWindowClosedNotification,
                                      TmuxWindowRenamedNotification,
                                      TmuxWindowPaneChangedNotification,
                                      TmuxSessionChangedNotification,
                                      TmuxSessionRenamedNotification,
                                      TmuxSessionsChangedNotification,
                                      TmuxSessionWindowChangedNotification,
                                      TmuxPanePausedNotification,
                                      TmuxPaneContinuedNotification,
                                      TmuxClientSessionChangedNotification,
                                      TmuxClientDetachedNotification,
                                      TmuxExitNotification>;

} // namespace Konsole

#endif // TMUXNOTIFICATION_H
