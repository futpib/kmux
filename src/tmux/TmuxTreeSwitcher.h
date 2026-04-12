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
    explicit TmuxTreeSwitcher(ViewManager *viewManager, TmuxController *controller);

    void updateState();
    void updateViewGeometry();

    // Exposed for testing: select a specific tree index and activate it
    QTreeView *treeView() const
    {
        return _treeView;
    }

public Q_SLOTS:
    void activateCurrent();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void reselectFirst();

    ViewManager *_viewManager;
    TmuxController *_controller;
    QLineEdit *_inputLine;
    QTreeView *_treeView;
    TmuxTreeModel *_model = nullptr;
    QSortFilterProxyModel *_proxyModel = nullptr;
};

} // namespace Konsole

#endif // TMUXTREESWITCHER_H
