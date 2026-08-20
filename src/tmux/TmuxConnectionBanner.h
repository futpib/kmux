/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXCONNECTIONBANNER_H
#define TMUXCONNECTIONBANNER_H

namespace Konsole
{

enum class TmuxConnectionBanner {
    Hidden,
    Unresponsive,
    Reconnecting,
    ReconnectingCheckTty,
    Disconnected,
};

}

#endif // TMUXCONNECTIONBANNER_H
