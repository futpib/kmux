/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXTESTFIXTURETEST_H
#define TMUXTESTFIXTURETEST_H

#include <QObject>

namespace Konsole
{
class TmuxTestFixtureTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testPaneDefaults();
    void testPaneProperties();
    void testHorizontalLayout();
    void testVerticalLayout();
    void testNestedLayout();
    void testFourPaneGrid();
    void testAlternativeGridGrouping();
    void testThreeHorizontalPanes();
    void testEmptyCommand();
    void testCountPanes();
    void testComputeWindowSize();
};

}

#endif // TMUXTESTFIXTURETEST_H
