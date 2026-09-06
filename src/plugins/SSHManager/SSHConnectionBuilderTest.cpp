/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "sshconnectionbuilder.h"
#include "sshmanagerconfig.h"

#include <QTest>

class SSHConnectionBuilderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void importedConfigUsesAlias();
    void manualConfigKeepsArgumentsSeparate();
    void advancedTmuxOptionsArePreserved();
    void usesOnlyKmuxConfigNamespace();
};

void SSHConnectionBuilderTest::importedConfigUsesAlias()
{
    SSHConfigurationData data;
    data.name = QStringLiteral("bastion-alias");
    data.host = QStringLiteral("resolved.example.test");
    data.useSshConfig = true;

    QCOMPARE(SSHConnectionBuilder::sshCommand(data), QStringList({QStringLiteral("ssh"), QStringLiteral("bastion-alias")}));
}

void SSHConnectionBuilderTest::manualConfigKeepsArgumentsSeparate()
{
    SSHConfigurationData data;
    data.host = QStringLiteral("server.example.test");
    data.port = QStringLiteral("2222");
    data.sshKey = QStringLiteral("/tmp/key with spaces");
    data.username = QStringLiteral("remote-user");

    QCOMPARE(SSHConnectionBuilder::sshCommand(data),
             QStringList({QStringLiteral("ssh"),
                          QStringLiteral("-i"),
                          QStringLiteral("/tmp/key with spaces"),
                          QStringLiteral("-p"),
                          QStringLiteral("2222"),
                          QStringLiteral("remote-user@server.example.test")}));
}

void SSHConnectionBuilderTest::advancedTmuxOptionsArePreserved()
{
    SSHConfigurationData data;
    data.name = QStringLiteral("Production");
    data.host = QStringLiteral("prod.example.test");
    data.username = QStringLiteral("operator");
    data.tmuxPath = QStringLiteral("/opt/tmux/bin/tmux");
    data.tmuxSession = QStringLiteral("operations");
    data.tmuxSocketName = QStringLiteral("private");
    data.remoteWorkingDirectory = QStringLiteral("/srv/app");

    const auto options = SSHConnectionBuilder::tmuxOptions(data);
    QCOMPARE(options.displayName, data.name);
    QCOMPARE(options.rshCommand, QStringList({QStringLiteral("ssh"), QStringLiteral("operator@prod.example.test")}));
    QCOMPARE(options.tmuxPath, data.tmuxPath);
    QCOMPARE(options.sessionName, data.tmuxSession);
    QCOMPARE(options.socketName, data.tmuxSocketName);
    QCOMPARE(options.socketPath, QString());
    QCOMPARE(options.workingDirectory, data.remoteWorkingDirectory);
}

void SSHConnectionBuilderTest::usesOnlyKmuxConfigNamespace()
{
    QCOMPARE(SSHManagerConfig::fileName(), QStringLiteral("kmuxsshconfig"));
    QVERIFY(!SSHManagerConfig::fileName().contains(QLatin1String("konsole"), Qt::CaseInsensitive));
}

QTEST_APPLESS_MAIN(SSHConnectionBuilderTest)

#include "SSHConnectionBuilderTest.moc"
