/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxPrefixPalette.h"

#include "TmuxCommand.h"
#include "TmuxController.h"
#include "TmuxGateway.h"

#include "ViewManager.h"
#include "widgets/ViewContainer.h"

#include <KActionCollection>
#include <KLocalizedString>

#include <QAction>
#include <QHeaderView>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>

namespace Konsole
{

TmuxPrefixPalette::TmuxPrefixPalette(ViewManager *viewManager, TmuxController *controller, const QList<TmuxPrefixBinding> &bindings)
    : QFrame(viewManager->activeContainer()->window())
    , _viewManager(viewManager)
    , _controller(controller)
    , _bindings(bindings)
{
    setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    setProperty("_breeze_force_frame", true);

    // Cache the prefix key/modifiers/token so a re-press whose modifier the
    // compositor dropped can still be recognised as the prefix (see
    // isPrefixRepressWithDroppedModifier / tryTriggerByKey).
    if (_controller) {
        const QKeySequence seq = _controller->prefixShortcut();
        if (!seq.isEmpty()) {
            const QKeyCombination combo = seq[0];
            _prefixKey = combo.key();
            _prefixModifiers = combo.keyboardModifiers();
        }
        _prefixToken = _controller->prefixToken();
    }

    window()->installEventFilter(this);

    auto *layout = new QVBoxLayout();
    layout->setSpacing(0);
    layout->setContentsMargins(QMargins());
    setLayout(layout);

    _treeView = new QTreeView(this);
    _treeView->setRootIsDecorated(false);
    _treeView->setUniformRowHeights(true);
    _treeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // Keep keyboard focus on the palette frame, not the tree view: every
    // keystroke is a tmux binding the palette dispatches in keyPressEvent, so
    // the tree must never steal keys for its own navigation. The mouse, however,
    // can select and run rows.
    _treeView->setFocusPolicy(Qt::NoFocus);
    _treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    _treeView->setSelectionBehavior(QAbstractItemView::SelectRows);
    // Highlight the row under the cursor so it's clear what a click will run.
    _treeView->setMouseTracking(true);
    _treeView->setHeaderHidden(true);
    layout->addWidget(_treeView, 1);

    _model = new QStandardItemModel(this);
    _treeView->setModel(_model);

    populateModel();

    // Mouse interaction mirrors the tmux tree switcher (Ctrl+B w): single-click
    // selects (highlights) a row, double-click runs its binding. The keyboard
    // path is untouched — keys still trigger bindings directly via keyPressEvent.
    connect(_treeView, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        _treeView->setCurrentIndex(index);
    });
    connect(_treeView, &QTreeView::activated, this, [this](const QModelIndex &index) {
        if (!index.isValid()) {
            return;
        }
        const int row = index.row();
        if (row >= 0 && row < _bindings.size()) {
            triggerBinding(_bindings.at(row));
        }
    });

    // Column 1 (raw tmux command) stretches and elides: commands can run long
    // (e.g. `switch-client -n`, `send-keys -R C-l`) and the horizontal scrollbar
    // is disabled, so giving it ResizeToContents would push column 2 off-screen.
    // Column 2 (kmux action label) takes its natural width so the label stays
    // fully visible when present.
    _treeView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    _treeView->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    _treeView->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

    setFocusPolicy(Qt::StrongFocus);

    updateViewGeometry();
    show();
    raise();
    setFocus();
}

QString TmuxPrefixPalette::interceptedActionName(const QString &command)
{
    // Accept both `choose-tree` and `choose-tree -Z…` forms. The flag detection
    // ignores -Z (zoom, purely a tmux rendering concern) and looks for -s / -w
    // to decide between the sessions/windows variants.
    const QString trimmed = command.trimmed();
    if (!trimmed.startsWith(QLatin1String("choose-tree"))) {
        return {};
    }
    const QString rest = trimmed.mid(QLatin1String("choose-tree").size());
    const bool wantsSessions = rest.contains(QRegularExpression(QStringLiteral("(^|\\s)-\\w*s\\b")));
    const bool wantsWindows = rest.contains(QRegularExpression(QStringLiteral("(^|\\s)-\\w*w\\b")));
    if (wantsSessions) {
        return QStringLiteral("tmux-tree-switcher-sessions");
    }
    if (wantsWindows) {
        return QStringLiteral("tmux-tree-switcher-windows");
    }
    return QStringLiteral("tmux-tree-switcher");
}

void TmuxPrefixPalette::populateModel()
{
    _model->removeRows(0, _model->rowCount());
    auto *actionCollection = _viewManager ? _viewManager->actionCollection() : nullptr;
    for (const TmuxPrefixBinding &b : std::as_const(_bindings)) {
        auto *keyItem = new QStandardItem(b.keyToken);
        keyItem->setEditable(false);
        auto *cmdItem = new QStandardItem(b.command);
        cmdItem->setEditable(false);
        QString actionLabel;
        const QString interceptName = interceptedActionName(b.command);
        if (!interceptName.isEmpty() && actionCollection) {
            if (QAction *action = actionCollection->action(interceptName)) {
                // Strip KDE menu accelerator markers ("&"), keep the plain label.
                actionLabel = action->text().remove(QLatin1Char('&'));
            }
        }
        auto *actionItem = new QStandardItem(actionLabel);
        actionItem->setEditable(false);
        _model->appendRow({keyItem, cmdItem, actionItem});
    }
}

void TmuxPrefixPalette::triggerBinding(const TmuxPrefixBinding &binding)
{
    const QString interceptName = interceptedActionName(binding.command);
    auto *actionCollection = _viewManager ? _viewManager->actionCollection() : nullptr;
    if (!interceptName.isEmpty() && actionCollection) {
        if (QAction *action = actionCollection->action(interceptName)) {
            // Defer the trigger so the palette's own keyPressEvent / focus
            // chain finishes unwinding first. Triggering synchronously from a
            // nested key-event stack races the palette's FocusOut-driven
            // self-deletion with the popup (e.g. TmuxTreeSwitcher) the action
            // opens, and has been observed to tear the new popup down before
            // the user — or a test — can interact with it.
            QPointer<QAction> guarded(action);
            QTimer::singleShot(0, action, [guarded]() {
                if (guarded) {
                    guarded->trigger();
                }
            });
            hide();
            deleteLater();
            return;
        }
    }
    if (_controller && _controller->gateway()) {
        _controller->gateway()->sendCommand(TmuxCommand(binding.command));
    }
    hide();
    deleteLater();
}

bool TmuxPrefixPalette::tryTriggerByKey(const QKeyEvent *event)
{
    const QString token = keyEventToTmuxToken(event);
    if (token.isEmpty()) {
        return false;
    }
    for (const TmuxPrefixBinding &b : std::as_const(_bindings)) {
        if (b.keyToken == token) {
            triggerBinding(b);
            return true;
        }
    }
    // Wayland modifier-state race recovery: the press right after the palette
    // grabs focus can arrive with a prefix modifier dropped, so a re-press of the
    // prefix key (tmux send-prefix, e.g. C-b C-b) comes through as a bare key that
    // matches no token and would be silently discarded. Recognise exactly that
    // case and dispatch the prefix's own binding.
    if (isPrefixRepressWithDroppedModifier(event)) {
        for (const TmuxPrefixBinding &b : std::as_const(_bindings)) {
            if (b.keyToken == _prefixToken) {
                triggerBinding(b);
                return true;
            }
        }
    }
    return false;
}

bool TmuxPrefixPalette::isPrefixRepressWithDroppedModifier(const QKeyEvent *event) const
{
    if (_prefixToken.isEmpty() || _prefixKey == Qt::Key_unknown) {
        return false;
    }
    if (event->key() != _prefixKey) {
        return false;
    }
    constexpr Qt::KeyboardModifiers relevant = Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier | Qt::MetaModifier;
    const Qt::KeyboardModifiers evMods = event->modifiers() & relevant;
    const Qt::KeyboardModifiers prefixMods = _prefixModifiers & relevant;
    // Same base key as the prefix, strictly fewer modifiers, and no modifier
    // beyond the prefix's own set — i.e. a prefix modifier went missing. This is
    // never true for an unrelated key or one carrying extra modifiers, so it
    // cannot promote e.g. a bare "o" into "C-o".
    return evMods != prefixMods && (evMods & ~prefixMods) == 0;
}

QString TmuxPrefixPalette::keyEventToTmuxToken(const QKeyEvent *event)
{
    const int key = event->key();
    const Qt::KeyboardModifiers mods = event->modifiers();
    const bool ctrl = (mods & Qt::ControlModifier) != 0;
    const bool alt = (mods & Qt::AltModifier) != 0;

    QString base;

    switch (key) {
    case Qt::Key_Up:
        base = QStringLiteral("Up");
        break;
    case Qt::Key_Down:
        base = QStringLiteral("Down");
        break;
    case Qt::Key_Left:
        base = QStringLiteral("Left");
        break;
    case Qt::Key_Right:
        base = QStringLiteral("Right");
        break;
    case Qt::Key_PageUp:
        base = QStringLiteral("PPage");
        break;
    case Qt::Key_PageDown:
        base = QStringLiteral("NPage");
        break;
    case Qt::Key_Home:
        base = QStringLiteral("Home");
        break;
    case Qt::Key_End:
        base = QStringLiteral("End");
        break;
    case Qt::Key_Insert:
        base = QStringLiteral("IC");
        break;
    case Qt::Key_Delete:
        base = QStringLiteral("DC");
        break;
    case Qt::Key_Backspace:
        base = QStringLiteral("BSpace");
        break;
    case Qt::Key_Tab:
        base = QStringLiteral("Tab");
        break;
    case Qt::Key_Space:
        base = QStringLiteral("Space");
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        base = QStringLiteral("Enter");
        break;
    default:
        break;
    }

    if (base.isEmpty() && key >= Qt::Key_F1 && key <= Qt::Key_F35) {
        base = QStringLiteral("F") + QString::number(1 + key - Qt::Key_F1);
    }

    if (base.isEmpty()) {
        // Printable character. Prefer event->text() so shifted glyphs
        // (e.g. `!`, `%`) come through; tmux tokens are glyph-sensitive.
        const QString text = event->text();
        if (!text.isEmpty() && text.at(0).isPrint()) {
            base = text;
        } else if (key >= 0x20 && key < 0x7f) {
            base = QString(QChar(key)).toLower();
        }
    }

    if (base.isEmpty()) {
        return {};
    }

    QString token;
    if (ctrl) {
        token += QStringLiteral("C-");
    }
    if (alt) {
        token += QStringLiteral("M-");
    }
    token += base;
    return token;
}

bool TmuxPrefixPalette::event(QEvent *event)
{
    // Qt dispatches QEvent::ShortcutOverride before resolving a global QAction
    // shortcut. If we don't accept it, Ctrl+B (or any other prefix-bound chord
    // that re-uses a kmux shortcut, e.g. the prefix key itself) is swallowed
    // by the parent window's `tmux-prefix-palette` QAction and re-spawns the
    // palette instead of dispatching the matching tmux binding. Accept the
    // override for keys we have a binding for so keyPressEvent gets them.
    if (event->type() == QEvent::ShortcutOverride) {
        auto *ke = static_cast<QKeyEvent *>(event);
        const QString token = keyEventToTmuxToken(ke);
        if (!token.isEmpty()) {
            for (const TmuxPrefixBinding &b : std::as_const(_bindings)) {
                if (b.keyToken == token) {
                    event->accept();
                    return true;
                }
            }
        }
    }
    return QFrame::event(event);
}

void TmuxPrefixPalette::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        event->accept();
        hide();
        deleteLater();
        return;
    }
    // Pure modifier presses (no text, no base key) — ignore so the user can
    // still type e.g. C-o (first the Ctrl press alone arrives).
    if (event->key() == Qt::Key_Control || event->key() == Qt::Key_Shift || event->key() == Qt::Key_Alt || event->key() == Qt::Key_Meta) {
        QFrame::keyPressEvent(event);
        return;
    }
    if (tryTriggerByKey(event)) {
        event->accept();
        return;
    }
    // Unknown key — close the palette; matches tmux behaviour (unknown key
    // after prefix is a no-op that leaves prefix mode).
    event->accept();
    hide();
    deleteLater();
}

bool TmuxPrefixPalette::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::FocusOut && obj == this) {
        hide();
        deleteLater();
        return true;
    }
    if (window() == obj && event->type() == QEvent::Resize) {
        updateViewGeometry();
    }
    return QWidget::eventFilter(obj, event);
}

void TmuxPrefixPalette::updateViewGeometry()
{
    QRect boundingRect = window()->contentsRect();
    constexpr int minWidth = 300;
    constexpr int minHeight = 300;
    const int maxWidth = boundingRect.width();
    const int maxHeight = boundingRect.height();
    const int preferredWidth = maxWidth / 2;
    const int preferredHeight = maxHeight / 2;
    const QSize size{qMin(maxWidth, qMax(preferredWidth, minWidth)), qMin(maxHeight, qMax(preferredHeight, minHeight))};
    setFixedSize(size);

    int x = (boundingRect.width() - size.width()) / 2;
    int y = boundingRect.top() + 40;
    move(x, y);
}

} // namespace Konsole

#include "moc_TmuxPrefixPalette.cpp"
