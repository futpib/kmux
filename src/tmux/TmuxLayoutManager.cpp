/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxLayoutManager.h"

#include "TmuxLayoutParser.h"
#include "TmuxPaneManager.h"

#include "ViewManager.h"
#include "session/Session.h"
#include "terminalDisplay/TerminalDisplay.h"
#include "terminalDisplay/TerminalFonts.h"
#include "widgets/TabPageWidget.h"
#include "widgets/ViewContainer.h"
#include "widgets/ViewSplitter.h"

#include <QLoggingCategory>

#include <utility>

Q_LOGGING_CATEGORY(KonsoleTmuxLayout, "konsole.tmux.layout", QtWarningMsg)

namespace Konsole
{

// Assign final dimensions and absolute offsets in one top-down pass. The
// last child absorbs a split-axis difference, matching tmux's layout rules.
static void applyLayoutBounds(TmuxLayoutNode &node, int width, int height, int x, int y)
{
    node.width = width;
    node.height = height;
    node.xOffset = x;
    node.yOffset = y;
    if (node.type == TmuxLayoutNodeType::Leaf || node.children.isEmpty()) {
        return;
    }
    const bool horizontal = node.type == TmuxLayoutNodeType::HSplit;
    int total = node.children.size() - 1;
    for (const auto &child : std::as_const(node.children)) {
        total += horizontal ? child.width : child.height;
    }
    const int difference = (horizontal ? width : height) - total;
    int offset = 0;
    for (auto &child : node.children) {
        int extent = horizontal ? child.width : child.height;
        if (&child == &node.children.last()) {
            extent = qMax(1, extent + difference);
        }
        applyLayoutBounds(child, horizontal ? extent : width, horizontal ? height : extent, x + (horizontal ? offset : 0), y + (horizontal ? 0 : offset));
        offset += extent + 1;
    }
}

void TmuxLayoutManager::resizeLayout(TmuxLayoutNode &node, int width, int height)
{
    applyLayoutBounds(node, width, height, node.xOffset, node.yOffset);
}

// Size advertisements need only the aggregate cell count, not a layout tree
// with pane identities, normalized descendants and serialized offsets.
QSize TmuxLayoutManager::layoutSize(ViewSplitter *splitter)
{
    const bool horizontal = splitter->orientation() == Qt::Horizontal;
    int extent = 0;
    int cross = 0;
    int children = 0;
    for (int i = 0; i < splitter->count(); ++i) {
        QSize size;
        if (auto *display = ViewSplitter::terminalDisplayForWidget(splitter->widget(i))) {
            size = QSize(display->columns(), display->lines());
        } else if (auto *nested = qobject_cast<ViewSplitter *>(splitter->widget(i))) {
            size = layoutSize(nested);
        } else {
            continue;
        }
        extent += horizontal ? size.width() : size.height();
        cross = qMax(cross, horizontal ? size.height() : size.width());
        ++children;
    }
    extent += qMax(0, children - 1);
    return horizontal ? QSize(extent, cross) : QSize(cross, extent);
}

TmuxLayoutManager::TmuxLayoutManager(TmuxPaneManager *paneManager, ViewManager *viewManager, QObject *parent)
    : QObject(parent)
    , _paneManager(paneManager)
    , _viewManager(viewManager)
{
}

// Measure each subtree once before normalizing the completed tree.
static TmuxLayoutNode buildMeasuredLayout(ViewSplitter *splitter, TmuxPaneManager *paneManager)
{
    // Single-child splitter: unwrap and recurse into the child.
    // Displays are wrapped in a container QWidget (see ensureContainerWidget),
    // so widget(i) is the wrapper — use terminalDisplayForWidget to peel it.
    if (splitter->count() == 1) {
        auto *childDisplay = ViewSplitter::terminalDisplayForWidget(splitter->widget(0));
        if (childDisplay) {
            TmuxLayoutNode leaf;
            leaf.type = TmuxLayoutNodeType::Leaf;
            leaf.paneId = paneManager->paneIdForDisplay(childDisplay);
            leaf.width = childDisplay->columns();
            leaf.height = childDisplay->lines();
            leaf.xOffset = 0;
            leaf.yOffset = 0;
            return leaf;
        }
        auto *childSplitter = qobject_cast<ViewSplitter *>(splitter->widget(0));
        if (childSplitter) {
            return buildMeasuredLayout(childSplitter, paneManager);
        }
    }

    TmuxLayoutNode node;
    node.type = (splitter->orientation() == Qt::Horizontal) ? TmuxLayoutNodeType::HSplit : TmuxLayoutNodeType::VSplit;
    bool horizontal = (node.type == TmuxLayoutNodeType::HSplit);

    int offset = 0;
    int maxCross = 0;
    for (int i = 0; i < splitter->count(); ++i) {
        TmuxLayoutNode child;
        auto *display = ViewSplitter::terminalDisplayForWidget(splitter->widget(i));
        if (display) {
            child.type = TmuxLayoutNodeType::Leaf;
            child.paneId = paneManager->paneIdForDisplay(display);
            child.width = display->columns();
            child.height = display->lines();
        } else if (auto *childSplitter = qobject_cast<ViewSplitter *>(splitter->widget(i))) {
            child = buildMeasuredLayout(childSplitter, paneManager);
        } else {
            continue;
        }

        if (horizontal) {
            child.xOffset = offset;
            child.yOffset = 0;
            offset += child.width + 1; // +1 for separator
            maxCross = qMax(maxCross, child.height);
        } else {
            child.xOffset = 0;
            child.yOffset = offset;
            offset += child.height + 1; // +1 for separator
            maxCross = qMax(maxCross, child.width);
        }

        node.children.append(child);
    }

    if (horizontal) {
        node.width = offset > 0 ? offset - 1 : 0; // subtract last separator
        node.height = maxCross;
    } else {
        node.width = maxCross;
        node.height = offset > 0 ? offset - 1 : 0;
    }

    return node;
}

TmuxLayoutNode TmuxLayoutManager::buildLayoutNode(ViewSplitter *splitter, TmuxPaneManager *paneManager)
{
    TmuxLayoutNode node = buildMeasuredLayout(splitter, paneManager);
    resizeLayout(node, node.width, node.height);
    return node;
}

// Constrain a top-level tmux splitter's size so that the entire layout
// shrinks to the top-left corner when a smaller client constrains the
// tmux window.  The TabPageWidget manages positioning — when constrained
// the splitter sits at the top-left with empty space filling the rest.
// When Konsole's window is focused it drives the tmux window size, so
// no constraint is applied — the layout should fill the available space.
static void constrainSplitterToLayout(ViewSplitter *splitter, const TmuxLayoutNode &layout)
{
    auto *page = qobject_cast<TabPageWidget *>(splitter->parentWidget());
    if (!page) {
        return;
    }

    // When Konsole's window is focused, it controls the tmux size via
    // refresh-client -C, so the layout should stretch to fill the tab.
    QWidget *window = page->window();
    if (window && window->isActiveWindow()) {
        qCDebug(KonsoleTmuxLayout) << "constrainSplitterToLayout: window is active, clearing constraint";
        page->clearConstrainedSize();
        return;
    }

    // Find font metrics from the first TerminalDisplay in the tree
    auto displays = splitter->findChildren<TerminalDisplay *>();
    if (displays.isEmpty()) {
        return;
    }
    auto *td = displays.first();
    int fontWidth = td->terminalFont()->fontWidth();
    int fontHeight = td->terminalFont()->fontHeight();
    if (fontWidth <= 0 || fontHeight <= 0) {
        return;
    }

    // Compute the pixel size the layout needs. Add chrome (margin, scroll bar,
    // header, hsl gutter) so the display has enough pixels to render every
    // layout cell — without that, the splitter is pinned to layout*font, the
    // display fills the splitter, contentRect = splitter - chrome, and the
    // cells the display can actually paint shrink to layout - ceil(chrome/
    // font). sendClientSize then re-advertises the full layout, layout-change
    // pins again, the display loses another row/col, and so on.
    QSize chrome = td->cellChromeSize();
    int layoutPixelWidth = layout.width * fontWidth + chrome.width();
    int layoutPixelHeight = layout.height * fontHeight + chrome.height();

    // Compare with the page's available space
    QSize available = page->size();

    qCDebug(KonsoleTmuxLayout) << "constrainSplitterToLayout: layout=" << layout.width << "x" << layout.height
                               << "layoutPx=" << layoutPixelWidth << "x" << layoutPixelHeight
                               << "available=" << available
                               << "fontW=" << fontWidth << "fontH=" << fontHeight;

    if (layoutPixelWidth < available.width() || layoutPixelHeight < available.height()) {
        QSize constrained(qMin(layoutPixelWidth, available.width()), qMin(layoutPixelHeight, available.height()));
        qCDebug(KonsoleTmuxLayout) << "constrainSplitterToLayout: → constraining to" << constrained;
        page->setConstrainedSize(constrained);
    } else {
        qCDebug(KonsoleTmuxLayout) << "constrainSplitterToLayout: → clearing constraint (layout fits)";
        page->clearConstrainedSize();
    }
}

int TmuxLayoutManager::applyLayout(int tabIndex, const TmuxLayoutNode &layout)
{
    qCDebug(KonsoleTmuxLayout) << "applyLayout: tabIndex=" << tabIndex
                               << "layout: type=" << static_cast<int>(layout.type)
                               << "size=" << layout.width << "x" << layout.height
                               << "children=" << layout.children.size();

    TabbedViewContainer *container = _viewManager->activeContainer();
    if (!container) {
        return -1;
    }

    if (tabIndex >= 0) {
        auto *oldSplitter = container->viewSplitterAt(tabIndex);
        if (oldSplitter) {
            if (!updateSplitterSizes(oldSplitter, layout, false)) {
                // Structure changed — collect existing displays, build new splitter, swap
                QMap<int, TerminalDisplay *> existingDisplays;
                collectDisplays(oldSplitter, existingDisplays);

                // Detach displays we want to reuse from the old splitter tree
                // so they aren't destroyed when the old splitter is deleted
                for (auto *display : existingDisplays) {
                    display->setParent(nullptr);
                }

                // Build a new splitter with the updated layout
                auto *newSplitter = new ViewSplitter();
                newSplitter->setTmuxMode(true);
                buildSplitterTree(newSplitter, layout, existingDisplays);
                connectSplitterSignals(newSplitter);

                // Swap: insert new tab first, then remove old one so the
                // container is never empty (which would close the window).
                QString tabText = container->tabText(tabIndex);
                QIcon tabIcon = container->tabIcon(tabIndex);
                auto *oldPage = container->tabPageAt(tabIndex);
                container->addSplitter(newSplitter, tabIndex + 1);
                container->removeTab(tabIndex);
                tabIndex = container->indexOfSplitter(newSplitter);
                container->setTabText(tabIndex, tabText);
                container->setTabIcon(tabIndex, tabIcon);

                // The old splitter (and any remaining children like nested splitters)
                // will be deleted. Displays were already detached above.
                // Disconnect to prevent viewDestroyed from firing during deletion.
                oldSplitter->disconnect();
                oldSplitter->setParent(nullptr);
                delete oldSplitter;
                if (oldPage) {
                    oldPage->disconnect();
                    delete oldPage;
                }

                // Destroy leftover displays that are no longer in the layout
                for (auto *display : existingDisplays) {
                    display->deleteLater();
                }
            }
            // Constrain splitter size when layout is smaller than available space
            auto *currentSplitter = container->viewSplitterAt(tabIndex);
            if (currentSplitter) {
                constrainSplitterToLayout(currentSplitter, layout);
            }
        }
        return tabIndex;
    }

    auto *splitter = new ViewSplitter();
    splitter->setTmuxMode(true);
    QMap<int, TerminalDisplay *> noExisting;

    if (layout.type == TmuxLayoutNodeType::Leaf) {
        Session *session = _paneManager->sessionForPane(layout.paneId);
        TerminalDisplay *display = _viewManager->createView(session);
        splitter->addTerminalDisplay(display, Qt::Horizontal);
    } else {
        buildSplitterTree(splitter, layout, noExisting);
        connectSplitterSignals(splitter);
    }

    container->addSplitter(splitter);
    return container->indexOfSplitter(splitter);
}

bool TmuxLayoutManager::updateSplitterSizes(ViewSplitter *splitter, const TmuxLayoutNode &node, bool skipSizeUpdate)
{
    if (node.type == TmuxLayoutNodeType::Leaf) {
        auto *display = ViewSplitter::terminalDisplayForWidget(splitter->widget(0));
        if (splitter->count() != 1 || !display) {
            return false;
        }
        if (!skipSizeUpdate) {
            display->setSize(node.width, node.height);
            display->setForcedSize(node.width, node.height);
        }
        return true;
    }

    Qt::Orientation expected = (node.type == TmuxLayoutNodeType::HSplit) ? Qt::Horizontal : Qt::Vertical;
    if (splitter->orientation() != expected) {
        return false;
    }

    if (splitter->count() != node.children.size()) {
        return false;
    }

    for (int i = 0; i < node.children.size(); ++i) {
        const auto &child = node.children[i];
        QWidget *widget = splitter->widget(i);

        if (child.type == TmuxLayoutNodeType::Leaf) {
            auto *display = ViewSplitter::terminalDisplayForWidget(widget);
            if (!display) {
                return false;
            }
            // Verify the pane ID matches — a swap changes which pane
            // is at each position without changing the tree structure.
            if (_paneManager->paneIdForDisplay(display) != child.paneId) {
                return false;
            }
        } else {
            auto *childSplitter = qobject_cast<ViewSplitter *>(widget);
            if (!childSplitter) {
                return false;
            }
            if (!updateSplitterSizes(childSplitter, child, skipSizeUpdate)) {
                return false;
            }
        }
    }

    if (!skipSizeUpdate) {
        for (int i = 0; i < node.children.size(); ++i) {
            const auto &child = node.children[i];
            if (child.type == TmuxLayoutNodeType::Leaf) {
                auto *display = ViewSplitter::terminalDisplayForWidget(splitter->widget(i));
                if (display) {
                    display->setSize(child.width, child.height);
                    display->setForcedSize(child.width, child.height);
                }
            }
        }

        QList<int> sizes;
        for (int i = 0; i < node.children.size(); ++i) {
            QWidget *widget = splitter->widget(i);
            if (splitter->orientation() == Qt::Horizontal) {
                sizes.append(widget->sizeHint().width());
            } else {
                sizes.append(widget->sizeHint().height());
            }
        }
        splitter->setSizes(sizes);
    }

    return true;
}

void TmuxLayoutManager::collectDisplays(ViewSplitter *splitter, QMap<int, TerminalDisplay *> &displayMap)
{
    for (int i = 0; i < splitter->count(); ++i) {
        auto *display = ViewSplitter::terminalDisplayForWidget(splitter->widget(i));
        if (display) {
            int paneId = _paneManager->paneIdForDisplay(display);
            if (paneId >= 0) {
                displayMap[paneId] = display;
            }
        } else if (auto *childSplitter = qobject_cast<ViewSplitter *>(splitter->widget(i))) {
            collectDisplays(childSplitter, displayMap);
        }
    }
}


void TmuxLayoutManager::buildSplitterTree(ViewSplitter *splitter, const TmuxLayoutNode &node, QMap<int, TerminalDisplay *> &existingDisplays)
{
    auto getOrCreateDisplay = [&](int paneId) -> TerminalDisplay * {
        if (existingDisplays.contains(paneId)) {
            return existingDisplays.take(paneId);
        }
        Session *session = _paneManager->sessionForPane(paneId);
        if (session) {
            return _viewManager->createView(session);
        }
        return nullptr;
    };

    if (node.type == TmuxLayoutNodeType::Leaf) {
        TerminalDisplay *display = getOrCreateDisplay(node.paneId);
        if (display) {
            splitter->addTerminalDisplay(display, Qt::Horizontal);
        }
        return;
    }

    Qt::Orientation orientation = (node.type == TmuxLayoutNodeType::HSplit) ? Qt::Horizontal : Qt::Vertical;
    splitter->setOrientation(orientation);

    splitter->setUpdatesEnabled(false);

    for (const auto &child : node.children) {
        if (child.type == TmuxLayoutNodeType::Leaf) {
            TerminalDisplay *display = getOrCreateDisplay(child.paneId);
            if (display) {
                splitter->addTerminalDisplay(display, -1);
                display->setSize(child.width, child.height);
                display->setForcedSize(child.width, child.height);
            }
        } else {
            auto *childSplitter = new ViewSplitter();
            childSplitter->setTmuxMode(true);
            buildSplitterTree(childSplitter, child, existingDisplays);
            splitter->addSplitter(childSplitter);
        }
    }

    QList<int> sizes;
    for (int i = 0; i < node.children.size(); ++i) {
        QWidget *widget = splitter->widget(i);
        if (orientation == Qt::Horizontal) {
            sizes.append(widget->sizeHint().width());
        } else {
            sizes.append(widget->sizeHint().height());
        }
    }
    splitter->setSizes(sizes);

    splitter->setUpdatesEnabled(true);
}

void TmuxLayoutManager::connectSplitterSignals(ViewSplitter *splitter)
{
    disconnect(splitter, &QSplitter::splitterMoved, this, nullptr);
    connect(splitter, &QSplitter::splitterMoved, this, [this, splitter]() {
        qCDebug(KonsoleTmuxLayout) << "QSplitter::splitterMoved on" << splitter
                                   << "orientation=" << (splitter->orientation() == Qt::Horizontal ? "H" : "V")
                                   << "count=" << splitter->count();
        Q_EMIT splitterMoved(splitter);
    });

    for (int i = 0; i < splitter->count(); ++i) {
        auto *handle = qobject_cast<ViewSplitterHandle *>(splitter->handle(i));
        if (handle) {
            disconnect(handle, &ViewSplitterHandle::dragStarted, this, nullptr);
            disconnect(handle, &ViewSplitterHandle::dragFinished, this, nullptr);
            connect(handle, &ViewSplitterHandle::dragStarted, this, [this, handle]() {
                qCDebug(KonsoleTmuxLayout) << "dragStarted from handle" << handle;
                Q_EMIT splitterDragStarted();
            });
            connect(handle, &ViewSplitterHandle::dragFinished, this, [this, handle]() {
                qCDebug(KonsoleTmuxLayout) << "dragFinished from handle" << handle;
                Q_EMIT splitterDragFinished();
            });
        }
    }

    for (int i = 0; i < splitter->count(); ++i) {
        auto *childSplitter = qobject_cast<ViewSplitter *>(splitter->widget(i));
        if (childSplitter) {
            connectSplitterSignals(childSplitter);
        }
    }
}

} // namespace Konsole

#include "moc_TmuxLayoutManager.cpp"
