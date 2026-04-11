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

Q_LOGGING_CATEGORY(KonsoleTmuxLayout, "konsole.tmux.layout", QtWarningMsg)

namespace Konsole
{

// Recursively set the height of a subtree node and propagate into children.
// Used when an HSplit parent constrains all children to the same height.
static void setSubtreeHeight(TmuxLayoutNode &node, int height)
{
    node.height = height;
    if (node.type == TmuxLayoutNodeType::Leaf) {
        return;
    }
    if (node.type == TmuxLayoutNodeType::HSplit) {
        // HSplit: height is the cross-axis — propagate to all children
        for (auto &child : node.children) {
            setSubtreeHeight(child, height);
        }
    } else {
        // VSplit: height is the split-axis — children stack vertically.
        // Their heights must sum to (height - separators). Absorb any
        // difference in the last child so tmux's checksum validation passes.
        if (!node.children.isEmpty()) {
            int currentSum = 0;
            for (const auto &child : node.children) {
                currentSum += child.height;
            }
            currentSum += node.children.size() - 1; // separators
            int diff = currentSum - height;
            if (diff != 0) {
                auto &last = node.children.last();
                int newH = qMax(1, last.height - diff);
                setSubtreeHeight(last, newH);
            }
        }
    }
}

// Recursively set the width of a subtree node and propagate into children.
// Used when a VSplit parent constrains all children to the same width.
static void setSubtreeWidth(TmuxLayoutNode &node, int width)
{
    node.width = width;
    if (node.type == TmuxLayoutNodeType::Leaf) {
        return;
    }
    if (node.type == TmuxLayoutNodeType::VSplit) {
        // VSplit: width is the cross-axis — propagate to all children
        for (auto &child : node.children) {
            setSubtreeWidth(child, width);
        }
    } else {
        // HSplit: width is the split-axis — children sit side-by-side.
        // Their widths must sum to (width - separators). Absorb any
        // difference in the last child so tmux's checksum validation passes.
        if (!node.children.isEmpty()) {
            int currentSum = 0;
            for (const auto &child : node.children) {
                currentSum += child.width;
            }
            currentSum += node.children.size() - 1; // separators
            int diff = currentSum - width;
            if (diff != 0) {
                auto &last = node.children.last();
                int newW = qMax(1, last.width - diff);
                setSubtreeWidth(last, newW);
            }
        }
    }
}

// Recursively compute absolute offsets for all nodes in the tree.
// baseX/baseY are the absolute position of this node's top-left corner.
static void computeAbsoluteOffsets(TmuxLayoutNode &node, int baseX, int baseY)
{
    node.xOffset = baseX;
    node.yOffset = baseY;

    if (node.type == TmuxLayoutNodeType::Leaf) {
        return;
    }

    bool horizontal = (node.type == TmuxLayoutNodeType::HSplit);
    int offset = 0;
    for (auto &child : node.children) {
        if (horizontal) {
            computeAbsoluteOffsets(child, baseX + offset, baseY);
            offset += child.width + 1; // +1 for separator
        } else {
            computeAbsoluteOffsets(child, baseX, baseY + offset);
            offset += child.height + 1;
        }
    }
}

TmuxLayoutManager::TmuxLayoutManager(TmuxPaneManager *paneManager, ViewManager *viewManager, QObject *parent)
    : QObject(parent)
    , _paneManager(paneManager)
    , _viewManager(viewManager)
{
}

TmuxLayoutNode TmuxLayoutManager::buildLayoutNode(ViewSplitter *splitter, TmuxPaneManager *paneManager)
{
    // Single-child splitter: unwrap and recurse into the child
    if (splitter->count() == 1) {
        auto *childDisplay = qobject_cast<TerminalDisplay *>(splitter->widget(0));
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
            return buildLayoutNode(childSplitter, paneManager);
        }
    }

    TmuxLayoutNode node;
    node.type = (splitter->orientation() == Qt::Horizontal) ? TmuxLayoutNodeType::HSplit : TmuxLayoutNodeType::VSplit;
    bool horizontal = (node.type == TmuxLayoutNodeType::HSplit);

    int offset = 0;
    int maxCross = 0;
    for (int i = 0; i < splitter->count(); ++i) {
        TmuxLayoutNode child;
        auto *display = qobject_cast<TerminalDisplay *>(splitter->widget(i));
        if (display) {
            child.type = TmuxLayoutNodeType::Leaf;
            child.paneId = paneManager->paneIdForDisplay(display);
            child.width = display->columns();
            child.height = display->lines();
        } else if (auto *childSplitter = qobject_cast<ViewSplitter *>(splitter->widget(i))) {
            child = buildLayoutNode(childSplitter, paneManager);
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
        // tmux requires all children in an HSplit to have the same height as parent;
        // propagate recursively into nested split children
        for (auto &child : node.children) {
            setSubtreeHeight(child, maxCross);
        }
    } else {
        node.width = maxCross;
        node.height = offset > 0 ? offset - 1 : 0;
        // tmux requires all children in a VSplit to have the same width as parent;
        // propagate recursively into nested split children
        for (auto &child : node.children) {
            setSubtreeWidth(child, maxCross);
        }
    }

    // Compute absolute offsets for the entire subtree.
    // The caller (or top-level) will set the final base position;
    // for now use (0,0) as the root offset — onSplitterMoved always
    // calls buildLayoutNode on the top-level splitter.
    computeAbsoluteOffsets(node, 0, 0);

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

    // Compute the pixel size the layout needs
    int layoutPixelWidth = layout.width * fontWidth;
    int layoutPixelHeight = layout.height * fontHeight;

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
        auto *display = qobject_cast<TerminalDisplay *>(splitter->widget(0));
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
            if (!qobject_cast<TerminalDisplay *>(widget)) {
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
                auto *display = qobject_cast<TerminalDisplay *>(splitter->widget(i));
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
        auto *display = qobject_cast<TerminalDisplay *>(splitter->widget(i));
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
