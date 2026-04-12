/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "MainWindowTest.h"

#include "../MainWindow.h"
#include "../ViewManager.h"

#include <KConfigGroup>
#include <KSharedConfig>
#include <KToolBar>
#include <QAction>
#include <QCoreApplication>
#include <QMenu>
#include <QPointer>
#include <QTest>

using namespace Konsole;

void MainWindowTest::initTestCase()
{
    QCoreApplication::setApplicationName(QStringLiteral("kmux"));

    QVERIFY(m_homeDir.isValid());
    m_originalHome = qgetenv("HOME");
    m_originalXdgConfigHome = qgetenv("XDG_CONFIG_HOME");
    m_originalXdgStateHome = qgetenv("XDG_STATE_HOME");
    qputenv("HOME", m_homeDir.path().toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (m_homeDir.path() + QStringLiteral("/.config")).toLocal8Bit());
    qputenv("XDG_STATE_HOME", (m_homeDir.path() + QStringLiteral("/.local/state")).toLocal8Bit());
}

void MainWindowTest::cleanupTestCase()
{
    qputenv("HOME", m_originalHome);
    qputenv("XDG_CONFIG_HOME", m_originalXdgConfigHome);
    qputenv("XDG_STATE_HOME", m_originalXdgStateHome);
}

namespace
{
KToolBar *findToolBar(MainWindow *mw, const QString &name)
{
    for (KToolBar *bar : mw->findChildren<KToolBar *>()) {
        if (bar->objectName() == name) {
            return bar;
        }
    }
    return nullptr;
}

// Drive the same "Show <toolbar>" toggle the user invokes via the
// Settings → Toolbars Shown submenu (also the toolbar right-click
// context menu). KXmlGuiWindow builds that submenu lazily inside
// toolBarMenuAction() — so we open it, find the toggle labeled after
// the target toolbar, and trigger it.
bool triggerShowToolbarToggle(MainWindow *mw, const QString &toolbarName)
{
    KToolBar *target = findToolBar(mw, toolbarName);
    if (!target) {
        return false;
    }

    QAction *menuAction = mw->toolBarMenuAction();
    if (!menuAction) {
        return false;
    }
    QMenu *m = menuAction->menu();
    if (!m) {
        return false;
    }
    Q_EMIT m->aboutToShow();

    const QString expected = target->windowTitle();
    const auto actions = m->actions();
    for (QAction *a : actions) {
        if (a->text() == expected) {
            a->trigger();
            return true;
        }
    }
    return false;
}

void openCloseWindow(std::function<void(MainWindow *)> body)
{
    auto *mw = new MainWindow();
    mw->viewManager()->newSession(mw->viewManager()->defaultProfile(), QString());
    mw->show();
    QVERIFY(QTest::qWaitForWindowActive(mw));
    QCoreApplication::processEvents();

    body(mw);

    QPointer<MainWindow> guard(mw);
    mw->close();
    while (guard) {
        QCoreApplication::processEvents();
    }
}
}

void MainWindowTest::testSessionToolbarVisibilityPersists()
{
    // First run: open window, toggle sessionToolbar off via the same
    // action the context menu uses, then close the window normally.
    openCloseWindow([](MainWindow *mw) {
        KToolBar *bar = findToolBar(mw, QStringLiteral("sessionToolbar"));
        QVERIFY(bar);
        QVERIFY(bar->isVisible());
        QVERIFY2(triggerShowToolbarToggle(mw, QStringLiteral("sessionToolbar")), "options_show_toolbar_sessionToolbar action should exist");
        QCoreApplication::processEvents();
        QVERIFY(!bar->isVisible());
    });

    // Second run: fresh window, sessionToolbar should come back hidden.
    openCloseWindow([](MainWindow *mw) {
        KToolBar *bar = findToolBar(mw, QStringLiteral("sessionToolbar"));
        QVERIFY(bar);
        QVERIFY2(!bar->isVisible(), "sessionToolbar should still be hidden on second run — setting did not persist");
    });
}

void MainWindowTest::testApplyReadsSameFileAsSaveWrote()
{
    // Reproduces the file-split bug observed in the real app:
    //   - setAutoSaveSettings() saves MainWindow state to one file.
    //   - On restart applyMainWindowSettings() reads from a different file
    //     — hasStateKey=false — so sessionToolbar comes back visible.
    // First run: toggle sessionToolbar off and close so save runs.
    openCloseWindow([](MainWindow *mw) {
        KToolBar *bar = findToolBar(mw, QStringLiteral("sessionToolbar"));
        QVERIFY(bar);
        QVERIFY(triggerShowToolbarToggle(mw, QStringLiteral("sessionToolbar")));
        QCoreApplication::processEvents();
    });

    // Second run: open a fresh window and ask KMainWindow which group it
    // treats as its autosave group. That group's State= key must be the
    // one that was just saved — otherwise save and restore are talking
    // to different files and sessionToolbar visibility can't be restored.
    auto *mw = new MainWindow();
    mw->viewManager()->newSession(mw->viewManager()->defaultProfile(), QString());
    mw->show();
    QVERIFY(QTest::qWaitForWindowActive(mw));
    QCoreApplication::processEvents();

    const KConfigGroup autoGroup = mw->autoSaveConfigGroup();
    const QString autoFile = autoGroup.config()->name();
    const int stateLen = autoGroup.readEntry("State", QByteArray()).size();

    qWarning() << "autoSave file=" << autoFile << "group=" << autoGroup.name() << "stateLen=" << stateLen;

    QPointer<MainWindow> guard(mw);
    mw->close();
    while (guard) {
        QCoreApplication::processEvents();
    }

    QVERIFY2(stateLen > 0,
             "applyMainWindowSettings read an empty State= on second run — "
             "save and restore are talking to different config files");
}

QTEST_MAIN(MainWindowTest)

#include "moc_MainWindowTest.cpp"
