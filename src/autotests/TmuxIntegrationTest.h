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
    void testCtrlShiftEBoundToTmuxZoom();
    void testTmuxZoomReflectedAsKonsoleMaximize();
    void testTmuxPrefixPaletteDetach();
    void testTmuxPrefixPaletteNextWindow();
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
    void testSwapPaneFromTmux();
    void testSwapPaneFromKonsole();
    void testMovePaneFromTmux();
    void testMovePaneFromKonsole();
    void testMovePaneFromTwoToOneFromTmux();
    void testMovePaneFromTwoToOneFromKonsole();
    void testFractalSplitDownRight8();
    void testFourEqualPanesTopRightFocused();
    void testSplitShortcutFocusInitialSplitAgain();
    void testNewTabFromTmuxPane();
    void testNewMainWindowFromTmuxPane();
    void testNewMainWindowFromTmuxPaneRegistersPlugins();
    void testDetachViewBreaksPane();
    void testDetachTabFromTmuxCreatesNewKmuxWindow();
    void testDetachTabFromTmuxViaContainerSignal();
    void testDetachFromTmuxAction();
    void testTreeSwitcherActivePreselected();
    void testTreeSwitcherSwitchesPaneSameWindow();
    void testTreeSwitcherSwitchesPaneDifferentWindow();
    void testTreeSwitcherSwitchesWindow();
    void testTreeSwitcherEscapeClosesNoChange();
    void testTreeSwitcherFuzzyFilter();
    void testTreeSwitcherSwitchesSession();
    void testTreeSwitcherSwitchesSessionWithTwoPanes();
    void testTreeSwitcherStaleSessionIsNoop();
    void testTreeSwitcherActivatePaneAlreadyActiveIsNoop();
    void testTreeSwitcherActivateCurrentWindowIsNoop();
    void testTreeSwitcherActivateCurrentSessionIsNoop();
    void testClosePaneFromSessionControllerConfirmed();
    void testClosePaneFromSessionControllerCancelled();
    void testCloseTabFromContainerConfirmed();

private:
    QTemporaryDir m_tmuxTmpDir;
};

}

#endif // TMUXINTEGRATIONTEST_H
