/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXPROCESSBRIDGETEST_H
#define TMUXPROCESSBRIDGETEST_H

#include <QObject>
#include <QTemporaryDir>

namespace Konsole
{
class TmuxProcessBridgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanup();
    void testConnectNoServer();
    void testConnectServerNoSessions();
    void testConnectServerPreexistingSession();
    void testRshSingleTokenWrapper();
    void testRshMultiTokenWrapperAndDefaultTmuxPath();

private:
    void killTmuxServer();
    QString tmuxSocketPath() const;

    QTemporaryDir m_tmuxTmpDir;
    QString m_tmuxPath;
};

}

#endif // TMUXPROCESSBRIDGETEST_H
