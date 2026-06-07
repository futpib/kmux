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
    void testUnresponsiveFiresWhenCommandUnanswered();
    void testReplyBeforeTimeoutStaysResponsive();
    void testIdleLinkNeverUnresponsive();
    void testActivityRecoversFromUnresponsive();
    void testUnresponsiveFiresDespitePeriodicResends();
    void testExtendedOutputParsedAsOutput();
};
}

#endif // TMUXGATEWAYTEST_H
