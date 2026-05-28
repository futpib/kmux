/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXGATEWAYTEST_H
#define TMUXGATEWAYTEST_H

#include <QObject>

namespace Konsole
{
class TmuxGatewayTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testNotificationInsideServerOriginatedBlockIsRouted();
    void testNotificationOutsideAnyBlockIsRouted();
    void testResponseInsideClientOriginatedBlockIsCaptured();
};
}

#endif // TMUXGATEWAYTEST_H
