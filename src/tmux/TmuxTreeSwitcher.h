/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXTREESWITCHER_H
#define TMUXTREESWITCHER_H

#include <QFrame>

#include "konsoleprivate_export.h"

class QLineEdit;
class QTreeView;
class QSortFilterProxyModel;

namespace Konsole
{

class TmuxController;
class TmuxTreeModel;
class ViewManager;

/**
 * Popup widget showing a tree of tmux sessions → windows → panes.
 * Enter switches to the selected item. Esc closes.
 */
class KONSOLEPRIVATE_EXPORT TmuxTreeSwitcher : public QFrame
{
    Q_OBJECT
public:
    // Matches tmux's choose-tree -s/-w/-p flags. Controls how much of the
    // session → window → pane tree is expanded when the picker opens.
    enum class InitialMode {
        Panes, // fully expanded (tmux -p / default)
        Windows, // sessions expanded, windows visible, panes collapsed (tmux -w)
        Sessions, // only sessions visible at the top level (tmux -s)
    };

    explicit TmuxTreeSwitcher(ViewManager *viewManager, TmuxController *controller, InitialMode mode = InitialMode::Panes);

    void updateState();
    void updateViewGeometry();

    // Exposed for testing: select a specific tree index and activate it
    QTreeView *treeView() const
    {
        return _treeView;
    }

    InitialMode initialMode() const
    {
        return _initialMode;
    }

public Q_SLOTS:
    void activateCurrent();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void reselectFirst();
    void applyInitialExpansion();

    ViewManager *_viewManager;
    TmuxController *_controller;
    QLineEdit *_inputLine;
    QTreeView *_treeView;
    TmuxTreeModel *_model = nullptr;
    QSortFilterProxyModel *_proxyModel = nullptr;
    InitialMode _initialMode = InitialMode::Panes;
};

} // namespace Konsole

#endif // TMUXTREESWITCHER_H
