/*
    SPDX-FileCopyrightText: 2015 René J.V. Bertin <rjvbertin@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef SHORTCUT_H
#define SHORTCUT_H

#include <qnamespace.h>

namespace Konsole
{
/**
 * Platform-specific main shortcut "opcode":
 */
constexpr Qt::Modifiers ACCEL =
#ifdef Q_OS_MACOS
    // Use plain Command key for shortcuts
    Qt::Modifiers(Qt::CTRL);
#else
    // Use Ctrl+Shift for shortcuts
    Qt::Modifiers(Qt::CTRL) | Qt::SHIFT;
#endif
}

#endif // SHORTCUT_H
