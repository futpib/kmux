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
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void populateModel();
    void triggerBinding(const TmuxPrefixBinding &binding);
    bool tryTriggerByKey(const QKeyEvent *event);
    // Dispatch a single post-prefix keystroke: trigger the matching binding,
    // send-prefix on a prefix re-press, or close on Escape/unknown. Shared by
    // keyPressEvent and the application-wide event filter.
    void handleKey(QKeyEvent *event);
    // True if obj is a widget in the palette's own top-level window — used to
    // scope the qApp event filter to our window.
    bool isForOurWindow(QObject *obj) const;
    // True when `event` is a re-press of the prefix key whose only difference
    // from the real prefix chord is one or more *missing* prefix modifiers — the
    // shape of the Wayland modifier-state race that drops Ctrl from the first key
    // after the palette grabs focus. Never true for an unrelated key or one with
    // extra modifiers, so it can't promote, say, a bare `o` into `C-o`.
    bool isPrefixRepressWithDroppedModifier(const QKeyEvent *event) const;

    // Some tmux commands (e.g. choose-tree, where we want our native UI) are
    // handled client-side by dispatching to a kmux QAction instead of forwarding
    // to tmux. Returns the action name (in ViewManager::actionCollection()) to
    // trigger for the given tmux command string, or an empty string if the
    // command should be forwarded to tmux unchanged.
    static QString interceptedActionName(const QString &command);

    ViewManager *_viewManager;
    TmuxController *_controller;
    QList<TmuxPrefixBinding> _bindings;

    // The prefix key, cached from the controller at construction, used to detect
    // a modifier-dropped re-press of the prefix (the tmux send-prefix gesture).
    Qt::Key _prefixKey = Qt::Key_unknown;
    Qt::KeyboardModifiers _prefixModifiers = Qt::NoModifier;
    QString _prefixToken;

    QTreeView *_treeView;
    QStandardItemModel *_model = nullptr;
};

} // namespace Konsole

#endif // TMUXPREFIXPALETTE_H
