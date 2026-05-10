/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXFORMATSPECTEST_H
#define TMUXFORMATSPECTEST_H

#include <QObject>

namespace Konsole
{
class TmuxFormatSpecTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testFormatStringEmbedsFieldsBetweenBoundaries();
    void testParseRowsRoundtripsValues();
    void testParseRowsSurvivesTabInValue();
    void testParseRowsSurvivesNewlineInValue();
    void testParseRowsHandlesEmptyResponse();
    void testParseRowsDropsMalformedRecords();
    void testFieldsWithDuplicateBoundaryNonceAreDistinct();
};
}

#endif // TMUXFORMATSPECTEST_H
