/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxTreeSwitcher.h"

#include "TmuxController.h"
#include "TmuxTreeModel.h"

#include "ViewManager.h"
#include "widgets/ViewContainer.h"

#include <KFuzzyMatcher>
#include <KLocalizedString>

#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <QStyle>
#include <QTreeView>
#include <QVBoxLayout>

namespace Konsole
{

namespace
{

class FuzzyFilterProxy : public QSortFilterProxyModel
{
public:
    explicit FuzzyFilterProxy(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setRecursiveFilteringEnabled(true);
    }

    void setPattern(const QString &p)
    {
        beginResetModel();
        _pattern = p;
        endResetModel();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (_pattern.isEmpty()) {
            return true;
        }
        QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        const QString name = idx.data(Qt::DisplayRole).toString();
        KFuzzyMatcher::Result r = KFuzzyMatcher::match(_pattern, name);
        if (auto *m = qobject_cast<TmuxTreeModel *>(sourceModel())) {
            m->setScore(idx, r.matched ? r.score : 0);
        }
        return r.matched;
    }

private:
    QString _pattern;
};

} // anonymous namespace

TmuxTreeSwitcher::TmuxTreeSwitcher(ViewManager *viewManager, TmuxController *controller)
    : QFrame(viewManager->activeContainer()->window())
    , _viewManager(viewManager)
    , _controller(controller)
{
    setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    setProperty("_breeze_force_frame", true);

    window()->installEventFilter(this);

    auto *layout = new QVBoxLayout();
    layout->setSpacing(0);
    layout->setContentsMargins(QMargins());
    setLayout(layout);

    _inputLine = new QLineEdit(this);
    _inputLine->setClearButtonEnabled(true);
    _inputLine->addAction(QIcon::fromTheme(QStringLiteral("search")), QLineEdit::LeadingPosition);
    _inputLine->setTextMargins(QMargins() + style()->pixelMetric(QStyle::PM_ButtonMargin));
    _inputLine->setPlaceholderText(i18nc("@label:textbox", "Search..."));
    _inputLine->setFont(QApplication::font());
    _inputLine->setFrame(false);
    setFocusProxy(_inputLine);
    layout->addWidget(_inputLine);

    _treeView = new QTreeView(this);
    _treeView->setHeaderHidden(true);
    _treeView->setRootIsDecorated(true);
    _treeView->setUniformRowHeights(true);
    _treeView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    _treeView->setProperty("_breeze_borders_sides", QVariant::fromValue(QFlags(Qt::TopEdge)));
    layout->addWidget(_treeView, 1);

    _model = new TmuxTreeModel(this);
    _proxyModel = new FuzzyFilterProxy(this);
    _proxyModel->setSourceModel(_model);
    _treeView->setModel(_proxyModel);
    _treeView->expandAll();

    connect(_inputLine, &QLineEdit::returnPressed, this, &TmuxTreeSwitcher::activateCurrent);
    connect(_treeView, &QTreeView::activated, this, &TmuxTreeSwitcher::activateCurrent);
    connect(_treeView, &QTreeView::clicked, this, [this](const QModelIndex &idx) {
        _treeView->setCurrentIndex(idx);
    });

    _inputLine->installEventFilter(this);
    _treeView->installEventFilter(this);

    connect(_inputLine, &QLineEdit::textChanged, this, [this](const QString &text) {
        static_cast<FuzzyFilterProxy *>(_proxyModel)->setPattern(text);
        _treeView->expandAll();
        reselectFirst();
    });

    setHidden(true);
    updateState();
}

void TmuxTreeSwitcher::updateState()
{
    // Query tmux for all sessions/windows/panes across the whole server.
    QPointer<TmuxTreeSwitcher> self(this);
    _controller->queryTree([self](QList<TmuxController::SessionDescriptor> sessions) {
        if (!self)
            return;
        self->_model->setData(sessions);
        self->_treeView->expandAll();
        self->reselectFirst();
    });

    updateViewGeometry();
    show();
    raise();
    setFocus();
}

void TmuxTreeSwitcher::reselectFirst()
{
    // Find the globally active pane: active session → active window → active pane.
    auto *m = _proxyModel;
    std::function<QModelIndex(const QModelIndex &, bool)> findActive = [&](const QModelIndex &parent, bool ancestorsActive) -> QModelIndex {
        int rows = m->rowCount(parent);
        for (int i = 0; i < rows; ++i) {
            QModelIndex idx = m->index(i, 0, parent);
            bool thisActive = idx.data(TmuxTreeModel::IsActiveRole).toBool();
            bool chainActive = ancestorsActive && thisActive;
            int type = idx.data(TmuxTreeModel::NodeTypeRole).toInt();
            if (chainActive && type == TmuxTreeModel::PaneNode) {
                return idx;
            }
            QModelIndex child = findActive(idx, chainActive);
            if (child.isValid())
                return child;
        }
        return {};
    };
    QModelIndex target = findActive({}, true);
    if (!target.isValid() && m->rowCount() > 0) {
        // first leaf: descend always to the first child
        target = m->index(0, 0);
        while (m->rowCount(target) > 0) {
            target = m->index(0, 0, target);
        }
    }
    if (target.isValid()) {
        _treeView->setCurrentIndex(target);
    }
}

void TmuxTreeSwitcher::activateCurrent()
{
    QModelIndex idx = _treeView->currentIndex();
    if (!idx.isValid()) {
        hide();
        deleteLater();
        return;
    }
    int type = idx.data(TmuxTreeModel::NodeTypeRole).toInt();
    int id = idx.data(TmuxTreeModel::IdRole).toInt();

    // For a pane/window/session in a different session than the current one,
    // we need switch-client first; tmux will re-initialize the controller.
    if (type == TmuxTreeModel::SessionNode) {
        if (id != _controller->sessionId()) {
            _controller->requestSwitchSession(id);
        }
    } else {
        // Find which session this item belongs to
        QModelIndex sessionIdx = idx;
        while (sessionIdx.isValid() && sessionIdx.data(TmuxTreeModel::NodeTypeRole).toInt() != TmuxTreeModel::SessionNode) {
            sessionIdx = sessionIdx.parent();
        }
        int targetSessionId = sessionIdx.isValid() ? sessionIdx.data(TmuxTreeModel::IdRole).toInt() : _controller->sessionId();

        if (targetSessionId != _controller->sessionId()) {
            // Switch session first, then select the target
            _controller->requestSwitchSession(targetSessionId);
        }
        if (type == TmuxTreeModel::WindowNode) {
            _controller->requestSelectWindow(id);
        } else if (type == TmuxTreeModel::PaneNode) {
            // Walk up to find the parent window and select it first —
            // select-pane only works within the active window.
            QModelIndex windowIdx = idx.parent();
            if (windowIdx.isValid() && windowIdx.data(TmuxTreeModel::NodeTypeRole).toInt() == TmuxTreeModel::WindowNode) {
                int windowId = windowIdx.data(TmuxTreeModel::IdRole).toInt();
                _controller->requestSelectWindow(windowId);
            }
            _controller->requestSelectPane(id);
        }
    }

    hide();
    deleteLater();
    window()->setFocus();
}

bool TmuxTreeSwitcher::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::ShortcutOverride) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (obj == _inputLine) {
            const bool forward = (keyEvent->key() == Qt::Key_Up) || (keyEvent->key() == Qt::Key_Down) || (keyEvent->key() == Qt::Key_PageUp) || (keyEvent->key() == Qt::Key_PageDown) || (keyEvent->key() == Qt::Key_Left) || (keyEvent->key() == Qt::Key_Right);
            if (forward) {
                QCoreApplication::sendEvent(_treeView, event);
                return true;
            }
        } else if (obj == _treeView) {
            const bool backToInput = (keyEvent->key() != Qt::Key_Up) && (keyEvent->key() != Qt::Key_Down) && (keyEvent->key() != Qt::Key_PageUp) && (keyEvent->key() != Qt::Key_PageDown) && (keyEvent->key() != Qt::Key_Tab) && (keyEvent->key() != Qt::Key_Backtab) && (keyEvent->key() != Qt::Key_Left) && (keyEvent->key() != Qt::Key_Right) && (keyEvent->key() != Qt::Key_Return) && (keyEvent->key() != Qt::Key_Enter);
            if (backToInput) {
                QCoreApplication::sendEvent(_inputLine, event);
                return true;
            }
        }
        if (keyEvent->key() == Qt::Key_Escape) {
            hide();
            deleteLater();
            return true;
        }
    }

    if (event->type() == QEvent::FocusOut && !(_inputLine->hasFocus() || _treeView->hasFocus())) {
        hide();
        deleteLater();
        return true;
    }

    if (window() == obj && event->type() == QEvent::Resize) {
        updateViewGeometry();
    }
    return QWidget::eventFilter(obj, event);
}

void TmuxTreeSwitcher::updateViewGeometry()
{
    QRect boundingRect = window()->contentsRect();
    constexpr int minWidth = 250;
    constexpr int minHeight = 300;
    const int maxWidth = boundingRect.width();
    const int maxHeight = boundingRect.height();
    const int preferredWidth = maxWidth / 3;
    const int preferredHeight = maxHeight / 2;
    const QSize size{qMin(maxWidth, qMax(preferredWidth, minWidth)), qMin(maxHeight, qMax(preferredHeight, minHeight))};
    setFixedSize(size);

    // Center horizontally, place near the top of the content area
    int x = (boundingRect.width() - size.width()) / 2;
    int y = boundingRect.top() + 40;
    move(x, y);
}

} // namespace Konsole

#include "moc_TmuxTreeSwitcher.cpp"
