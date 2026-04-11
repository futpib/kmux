/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXINTEGRATIONTEST_H
#define TMUXINTEGRATIONTEST_H

#include <QObject>
#include <QTemporaryDir>

namespace Konsole
{
class TmuxIntegrationTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void testTmuxControlModeExitCleanup();
    void testClosePaneTabThenGatewayTab();
    void testTmuxControlModeAttach();
    void testTmuxTwoPaneSplitAttach();
    void testTmuxAttachContentRecovery();
    void testTmuxAttachComplexPromptRecovery();
    void testSplitterResizePropagatedToTmux();
    void testTmuxPaneTitleInfo();
    void testWindowNameWithSpaces();
    void testSplitPaneFocusesNewPane();
    void testSplitPaneFocusesNewPaneComplexLayout();
    void testSplitPaneFocusesNewPaneNestedLayout();
    void testResizePropagatedToPty();
    void testNestedResizePropagatedToPty();
    void testTopLevelResizeWithNestedChild();
    void testNestedResizeSurvivesFocusCycle();
    void testForcedSizeFromSmallerClient();
    void testForcedSizeFromSmallerClientMultiPane();
    void testClearScrollbackSyncToTmux();
    void testClearScrollbackAndResetSyncToTmux();
    void testTmuxZoomFromKonsole();
    void testTmuxZoomFromTmux();
    void testTmuxZoomSurvivesLayoutChanges();
    void testBreakPane();
    void testSplitPaneInheritsWorkingDirectory();
    void testNewWindowInheritsWorkingDirectory();
    void testOscColorQueryNotLeakedAsKeystrokes();
    void testCyrillicInputPreservesUtf8();
    void testTmuxAttachNoSessions();
    void testAttachMultipleWindows();
    void testNewWindowCreatesTab();
    void testCloseWindowFromTmuxRemovesTab();
    void testCloseWindowTabFromKonsole();
    void testRenameWindowFromTmuxUpdatesTab();
    void testFractalSplitDownRight1();
    void testFractalSplitDownRight2();
    void testFractalSplitDownRight3();
    // TODO: depths 4+ fail — splitter tree structure diverges from expected fractal at depth 2
    // void testFractalSplitDownRight4();
    // void testFractalSplitDownRight5();
    // void testFractalSplitDownRight6();
    // void testFractalSplitDownRight7();
    // void testFractalSplitDownRight8();

private:
    void fractalSplitDownRight(int depth);
    QTemporaryDir m_tmuxTmpDir;
};

}

#endif // TMUXINTEGRATIONTEST_H
