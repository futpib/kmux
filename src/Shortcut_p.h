/*
    SPDX-FileCopyrightText: 2015 René J.V. Bertin <rjvbertin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef SHORTCUT_H
#define SHORTCUT_H

#include <QKeySequence>
#include <qnamespace.h>

namespace Konsole
{
/**
 * Platform-specific main shortcut "opcode":
 */
#ifdef Q_OS_MACOS
inline constexpr int ACCEL = Qt::CTRL;
#else
inline constexpr int ACCEL = Qt::CTRL | Qt::SHIFT;
#endif
}

#endif // SHORTCUT_H
