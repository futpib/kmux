/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXTREEROWFORMATTEST_H
#define TMUXTREEROWFORMATTEST_H

#include <QObject>

namespace Konsole
{
class TmuxTreeRowFormatTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // Window-row behaviour: tmux only promotes the lone pane's title to
    // the window row when the window has exactly one pane.
    void testWindowRowSinglePaneAppendsMeaningfulTitle();
    void testWindowRowSinglePaneSuppressesEmptyTitle();
    void testWindowRowSinglePaneSuppressesHostShortTitle();
    void testWindowRowMultiPaneSuppressesTitleEvenIfMeaningful();
    void testWindowRowZeroPanesSuppressesTitle();
    void testWindowRowRendersFlagsAfterName();
    void testWindowRowEmptyHostShortStillSuppressesEmptyTitle();

    // Pane-row behaviour: the active '*' marker, the marked 'M' marker,
    // and the same title-suffix gate.
    void testPaneRowMeaningfulTitleAppended();
    void testPaneRowEmptyTitleSuppressed();
    void testPaneRowHostShortTitleSuppressed();
    void testPaneRowActiveMarker();
    void testPaneRowMarkedMarker();
    void testPaneRowActiveAndMarkedTogether();
    void testPaneRowQuotingPreservesTitleCharacters();
};
}

#endif // TMUXTREEROWFORMATTEST_H
