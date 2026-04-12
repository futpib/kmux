/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef MAINWINDOWTEST_H
#define MAINWINDOWTEST_H

#include <QObject>
#include <QTemporaryDir>

namespace Konsole
{
class MainWindowTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void testSessionToolbarVisibilityPersists();
    void testApplyReadsSameFileAsSaveWrote();

private:
    QTemporaryDir m_homeDir;
    QByteArray m_originalHome;
    QByteArray m_originalXdgConfigHome;
    QByteArray m_originalXdgStateHome;
};

} // namespace Konsole

#endif
