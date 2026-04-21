/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXPREFIXPALETTE_H
#define TMUXPREFIXPALETTE_H

#include <QFrame>
#include <QList>
#include <QString>

#include "konsoleprivate_export.h"

class QKeyEvent;
class QTreeView;
class QStandardItemModel;

namespace Konsole
{

class TmuxController;
class ViewManager;

struct TmuxPrefixBinding {
    QString keyToken; // tmux key token, e.g. "z", "Space", "C-o", "M-1"
    QString command; // raw tmux command string (everything after the key)
};

/**
 * Popup widget showing the tmux prefix key bindings. Opens in response to the
 * user pressing the (dynamically bound) tmux prefix shortcut. The next
 * keystroke looks up the matching tmux binding and sends its command string
 * back to tmux via the gateway — tmux executes it and kmux reacts to any
 * resulting notifications through existing handlers. Escape cancels.
 */
class KONSOLEPRIVATE_EXPORT TmuxPrefixPalette : public QFrame
{
    Q_OBJECT
public:
    TmuxPrefixPalette(ViewManager *viewManager, TmuxController *controller, const QList<TmuxPrefixBinding> &bindings);

    void updateViewGeometry();

    // Exposed for testing.
    QTreeView *treeView() const
    {
        return _treeView;
    }

    // Translate a QKeyEvent into the tmux key token format (e.g. "z", "Space",
    // "C-o", "M-1", "PPage"). Empty if the key has no tmux representation.
    static QString keyEventToTmuxToken(const QKeyEvent *event);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void populateModel();
    void triggerBinding(const TmuxPrefixBinding &binding);
    bool tryTriggerByKey(const QKeyEvent *event);

    // Some tmux commands (e.g. choose-tree, where we want our native UI) are
    // handled client-side by dispatching to a kmux QAction instead of forwarding
    // to tmux. Returns the action name (in ViewManager::actionCollection()) to
    // trigger for the given tmux command string, or an empty string if the
    // command should be forwarded to tmux unchanged.
    static QString interceptedActionName(const QString &command);

    ViewManager *_viewManager;
    TmuxController *_controller;
    QList<TmuxPrefixBinding> _bindings;

    QTreeView *_treeView;
    QStandardItemModel *_model = nullptr;
};

} // namespace Konsole

#endif // TMUXPREFIXPALETTE_H
