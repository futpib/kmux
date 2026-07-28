/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxTestFixtureTest.h"
#include "TmuxTestFixture.h"

#include <QTest>

using namespace Konsole;
using namespace Konsole::TmuxTestFixture;

void TmuxTestFixtureTest::testPaneDefaults()
{
    const auto layout = pane(QStringLiteral("sleep 30"));

    QCOMPARE(layout.type, LayoutSpec::Leaf);
    QCOMPARE(layout.pane.command, QStringLiteral("sleep 30"));
    QCOMPARE(layout.pane.columns, 14);
    QCOMPARE(layout.pane.lines, 3);
    QVERIFY(!layout.pane.focused);
}

void TmuxTestFixtureTest::testPaneProperties()
{
    const auto layout = pane(QStringLiteral("bash"), 80, 24, true);

    QCOMPARE(layout.pane.command, QStringLiteral("bash"));
    QCOMPARE(layout.pane.columns, 80);
    QCOMPARE(layout.pane.lines, 24);
    QVERIFY(layout.pane.focused);
}

void TmuxTestFixtureTest::testHorizontalLayout()
{
    const auto layout = horizontal({pane(QStringLiteral("left"), 10, 3), pane(QStringLiteral("right"), 20, 3)});

    QCOMPARE(layout.type, LayoutSpec::HSplit);
    QCOMPARE(layout.children.size(), 2);
    QCOMPARE(layout.children[0].pane.command, QStringLiteral("left"));
    QCOMPARE(layout.children[1].pane.command, QStringLiteral("right"));
    QCOMPARE(computeWindowSize(layout), qMakePair(31, 3));
}

void TmuxTestFixtureTest::testVerticalLayout()
{
    const auto layout = vertical({pane(QStringLiteral("top"), 10, 3), pane(QStringLiteral("bottom"), 10, 5)});

    QCOMPARE(layout.type, LayoutSpec::VSplit);
    QCOMPARE(layout.children.size(), 2);
    QCOMPARE(layout.children[0].pane.command, QStringLiteral("top"));
    QCOMPARE(layout.children[1].pane.command, QStringLiteral("bottom"));
    QCOMPARE(computeWindowSize(layout), qMakePair(10, 9));
}

void TmuxTestFixtureTest::testNestedLayout()
{
    const auto layout = horizontal({
        pane(QStringLiteral("left"), 10, 7),
        vertical({
            pane(QStringLiteral("top-right"), 10, 3),
            pane(QStringLiteral("bottom-right"), 10, 3),
        }),
    });

    QCOMPARE(layout.type, LayoutSpec::HSplit);
    QCOMPARE(layout.children.size(), 2);
    QCOMPARE(layout.children[1].type, LayoutSpec::VSplit);
    QCOMPARE(layout.children[1].children.size(), 2);
    QCOMPARE(computeWindowSize(layout), qMakePair(21, 7));
}

void TmuxTestFixtureTest::testFourPaneGrid()
{
    const auto layout = horizontal({
        vertical({pane(QStringLiteral("top-left")), pane(QStringLiteral("bottom-left"))}),
        vertical({pane(QStringLiteral("top-right")), pane(QStringLiteral("bottom-right"))}),
    });

    QCOMPARE(layout.type, LayoutSpec::HSplit);
    QCOMPARE(layout.children[0].type, LayoutSpec::VSplit);
    QCOMPARE(layout.children[1].type, LayoutSpec::VSplit);
    QCOMPARE(countPanes(layout), 4);
}

void TmuxTestFixtureTest::testAlternativeGridGrouping()
{
    const auto layout = vertical({
        horizontal({pane(QStringLiteral("top-left")), pane(QStringLiteral("top-right"))}),
        horizontal({pane(QStringLiteral("bottom-left")), pane(QStringLiteral("bottom-right"))}),
    });

    QCOMPARE(layout.type, LayoutSpec::VSplit);
    QCOMPARE(layout.children[0].type, LayoutSpec::HSplit);
    QCOMPARE(layout.children[1].type, LayoutSpec::HSplit);
    QCOMPARE(countPanes(layout), 4);
}

void TmuxTestFixtureTest::testThreeHorizontalPanes()
{
    const auto layout = horizontal({
        pane(QStringLiteral("first")),
        pane(QStringLiteral("second")),
        pane(QStringLiteral("third")),
    });

    QCOMPARE(layout.children.size(), 3);
    QCOMPARE(countPanes(layout), 3);
    QCOMPARE(computeWindowSize(layout), qMakePair(44, 3));
}

void TmuxTestFixtureTest::testEmptyCommand()
{
    const auto layout = pane();

    QVERIFY(layout.pane.command.isEmpty());
    QCOMPARE(layout.pane.columns, 14);
    QCOMPARE(layout.pane.lines, 3);
}

void TmuxTestFixtureTest::testCountPanes()
{
    const auto layout = horizontal({
        pane(),
        vertical({pane(), pane(), pane()}),
    });

    QCOMPARE(countPanes(layout), 4);
}

void TmuxTestFixtureTest::testComputeWindowSize()
{
    const auto layout = vertical({
        horizontal({pane({}, 20, 4), pane({}, 30, 4)}),
        pane({}, 51, 6),
    });

    QCOMPARE(computeWindowSize(layout), qMakePair(51, 11));
}

QTEST_MAIN(TmuxTestFixtureTest)

#include "moc_TmuxTestFixtureTest.cpp"
