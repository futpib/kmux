/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../containers/DistroboxListParser.h"
#include "../containers/ToolboxListParser.h"

#include <QTest>

namespace Konsole
{
class ContainerDetectorParsingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parseDistrobox_data();
    void parseDistrobox();
    void parseToolbox_data();
    void parseToolbox();
};

void ContainerDetectorParsingTest::parseDistrobox_data()
{
    QTest::addColumn<QString>("output");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("pipe-table-valid")
        << QStringLiteral("ID           | NAME                 | STATUS             | IMAGE\n"
                          "f5678ef7cf1d | Android-Development  | Up 11 minutes      | quay.io/toolbx-images/debian-toolbox:latest\n"
                          "997f5777978a | pmos                 | Up 11 minutes      | docker.io/library/debian:latest\n")
        << QStringList({QStringLiteral("Android-Development"), QStringLiteral("pmos")});

    QTest::newRow("whitespace-table-valid")
        << QStringLiteral("ID NAME STATUS IMAGE\n"
                          "abc123 fedora-41 Up registry.fedoraproject.org/fedora-toolbox:41\n")
        << QStringList({QStringLiteral("fedora-41")});

    QTest::newRow("error-text")
        << QStringLiteral("Error: no containers found for current runtime\n")
        << QStringList{};

    QTest::newRow("missing-header")
        << QStringLiteral("f5678ef7cf1d | Android-Development | Up 11 minutes | quay.io/toolbx-images/debian-toolbox:latest\n")
        << QStringList{};

    QTest::newRow("empty")
        << QString()
        << QStringList{};

    QTest::newRow("explicit-empty-string")
        << QStringLiteral("")
        << QStringList{};

    QTest::newRow("whitespace-only")
        << QStringLiteral("   \n\t  \n")
        << QStringList{};
}

void ContainerDetectorParsingTest::parseDistrobox()
{
    QFETCH(QString, output);
    QFETCH(QStringList, expected);
    QCOMPARE(parseDistroboxContainerNames(output), expected);
}

void ContainerDetectorParsingTest::parseToolbox_data()
{
    QTest::addColumn<QString>("output");
    QTest::addColumn<QStringList>("expected");

    QTest::newRow("toolbox-valid")
        << QStringLiteral("CONTAINER ID  CONTAINER NAME  CREATED       STATUS   IMAGE NAME\n"
                          "ecd075209720  codex           17 hours ago  running  registry.fedoraproject.org/fedora-toolbox:latest\n"
                          "24451bedace8  tuxedo-noble    8 minutes ago running  docker.io/library/ubuntu:24.04\n")
        << QStringList({QStringLiteral("codex"), QStringLiteral("tuxedo-noble")});

    QTest::newRow("short-header-valid")
        << QStringLiteral("ID NAME STATUS IMAGE\n"
                          "abcd1234 toolbox-fedora running registry.fedoraproject.org/fedora-toolbox:latest\n")
        << QStringList({QStringLiteral("toolbox-fedora")});

    QTest::newRow("error-text")
        << QStringLiteral("Error: toolbox command failed\n")
        << QStringList{};

    QTest::newRow("missing-header")
        << QStringLiteral("ecd075209720 codex 17-hours running registry.fedoraproject.org/fedora-toolbox:latest\n")
        << QStringList{};

    QTest::newRow("header-only")
        << QStringLiteral("CONTAINER ID  CONTAINER NAME  CREATED       STATUS   IMAGE NAME\n")
        << QStringList{};

    QTest::newRow("explicit-empty-string")
        << QStringLiteral("")
        << QStringList{};

    QTest::newRow("whitespace-only")
        << QStringLiteral(" \n\t\n ")
        << QStringList{};
}

void ContainerDetectorParsingTest::parseToolbox()
{
    QFETCH(QString, output);
    QFETCH(QStringList, expected);
    QCOMPARE(parseToolboxContainerNames(output), expected);
}

} // namespace Konsole

QTEST_MAIN(Konsole::ContainerDetectorParsingTest)

#include "ContainerDetectorParsingTest.moc"
