/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxResizeCoordinator.h"

#include "TmuxCommand.h"
#include "TmuxController.h"
#include "TmuxGateway.h"
#include "TmuxLayoutManager.h"
#include "TmuxLayoutParser.h"
#include "TmuxPaneManager.h"

#include "ViewManager.h"
#include "terminalDisplay/TerminalDisplay.h"
#include "terminalDisplay/TerminalFonts.h"
#include "widgets/TabPageWidget.h"
#include "widgets/ViewContainer.h"
#include "widgets/ViewSplitter.h"

#include <QApplication>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(KonsoleTmuxResize, "konsole.tmux.resize", QtWarningMsg)

namespace Konsole
{

static void setSubtreeHeight(TmuxLayoutNode &node, int height)
{
    node.height = height;
    if (node.type == TmuxLayoutNodeType::Leaf) {
        return;
    }
    if (node.type == TmuxLayoutNodeType::HSplit) {
        for (auto &child : node.children) {
            setSubtreeHeight(child, height);
        }
    } else {
        if (!node.children.isEmpty()) {
            int currentSum = 0;
            for (const auto &child : node.children) {
                currentSum += child.height;
            }
            currentSum += node.children.size() - 1;
            int diff = currentSum - height;
            if (diff != 0) {
                auto &last = node.children.last();
                int newH = qMax(1, last.height - diff);
                setSubtreeHeight(last, newH);
            }
        }
    }
}

static void setSubtreeWidth(TmuxLayoutNode &node, int width)
{
    node.width = width;
    if (node.type == TmuxLayoutNodeType::Leaf) {
        return;
    }
    if (node.type == TmuxLayoutNodeType::VSplit) {
        for (auto &child : node.children) {
            setSubtreeWidth(child, width);
        }
    } else {
        if (!node.children.isEmpty()) {
            int currentSum = 0;
            for (const auto &child : node.children) {
                currentSum += child.width;
            }
            currentSum += node.children.size() - 1;
            int diff = currentSum - width;
            if (diff != 0) {
                auto &last = node.children.last();
                int newW = qMax(1, last.width - diff);
                setSubtreeWidth(last, newW);
            }
        }
    }
}

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
            offset += child.width + 1;
        } else {
            computeAbsoluteOffsets(child, baseX, baseY + offset);
            offset += child.height + 1;
        }
    }
}

TmuxResizeCoordinator::TmuxResizeCoordinator(TmuxGateway *gateway, TmuxController *controller, TmuxPaneManager *paneManager, TmuxLayoutManager *layoutManager, ViewManager *viewManager, QObject *parent)
    : QObject(parent)
    , _gateway(gateway)
    , _controller(controller)
    , _paneManager(paneManager)
    , _layoutManager(layoutManager)
    , _viewManager(viewManager)
{
    _resizeTimer.setSingleShot(true);
    _resizeTimer.setInterval(100);
    connect(&_resizeTimer, &QTimer::timeout, this, &TmuxResizeCoordinator::sendClientSize);

    connect(qApp, &QApplication::focusChanged, this, [this]() {
        _resizeTimer.start();
    });
    connect(viewManager, &ViewManager::activeViewChanged, this, [this]() {
        qCDebug(KonsoleTmuxResize) << "activeViewChanged → starting resize timer";
        _resizeTimer.start();
    });
}

void TmuxResizeCoordinator::onPaneViewSizeChanged(bool suppressResize)
{
    qCDebug(KonsoleTmuxResize) << "onPaneViewSizeChanged: suppressResize=" << suppressResize;
    if (suppressResize) {
        return;
    }
    _resizeTimer.start();
}

// Recursively clamp a layout tree so that its root dimensions match targetW x targetH.
// The difference between the node's current size and the target is absorbed by the
// last child along the split axis; the cross-axis is propagated to all children.
static void clampLayoutToSize(TmuxLayoutNode &node, int targetW, int targetH)
{
    if (node.type == TmuxLayoutNodeType::Leaf) {
        node.width = targetW;
        node.height = targetH;
        return;
    }

    bool horizontal = (node.type == TmuxLayoutNodeType::HSplit);
    node.width = targetW;
    node.height = targetH;

    if (node.children.isEmpty()) {
        return;
    }

    if (horizontal) {
        // Cross-axis: all children get the same height
        for (auto &child : node.children) {
            setSubtreeHeight(child, targetH);
        }
        // Split axis: compute current total width of children
        int currentTotal = 0;
        for (const auto &child : node.children) {
            currentTotal += child.width;
        }
        currentTotal += node.children.size() - 1; // separators
        int diff = currentTotal - targetW;
        if (diff != 0) {
            // Absorb the difference in the last child
            auto &last = node.children.last();
            int newWidth = last.width - diff;
            if (newWidth < 1) {
                newWidth = 1;
            }
            clampLayoutToSize(last, newWidth, targetH);
        }
    } else {
        // Cross-axis: all children get the same width
        for (auto &child : node.children) {
            setSubtreeWidth(child, targetW);
        }
        // Split axis: compute current total height of children
        int currentTotal = 0;
        for (const auto &child : node.children) {
            currentTotal += child.height;
        }
        currentTotal += node.children.size() - 1; // separators
        int diff = currentTotal - targetH;
        if (diff != 0) {
            auto &last = node.children.last();
            int newHeight = last.height - diff;
            if (newHeight < 1) {
                newHeight = 1;
            }
            clampLayoutToSize(last, targetW, newHeight);
        }
    }

    // Recompute absolute offsets after clamping
    computeAbsoluteOffsets(node, node.xOffset, node.yOffset);
}

void TmuxResizeCoordinator::onSplitterMoved(ViewSplitter *splitter)
{
    ViewSplitter *topLevel = splitter->getToplevelSplitter();
    TmuxLayoutNode node = TmuxLayoutManager::buildLayoutNode(topLevel, _paneManager);

    // Find window ID for this splitter's tab
    TabbedViewContainer *container = _viewManager->activeContainer();
    if (!container) {
        qCDebug(KonsoleTmuxResize) << "onSplitterMoved: no active container, aborting";
        return;
    }
    int tabIndex = container->indexOfSplitter(topLevel);

    const auto &windowToTab = _controller->windowToTabIndex();
    int windowId = -1;
    for (auto it = windowToTab.constBegin(); it != windowToTab.constEnd(); ++it) {
        if (it.value() == tabIndex) {
            windowId = it.key();
            break;
        }
    }

    if (windowId < 0) {
        qCDebug(KonsoleTmuxResize) << "onSplitterMoved: no windowId found for tabIndex=" << tabIndex << ", aborting";
        return;
    }

    // Send refresh-client -C first so tmux knows the window size,
    // then select-layout to set the exact pane proportions.
    // If we send select-layout alone, a subsequent refresh-client -C
    // (from the debounced timer) would cause tmux to re-layout from
    // scratch, overriding our custom layout.
    sendClientSize();

    // Clamp the layout to the actual tmux window size. Konsole's widgets
    // may be larger than what tmux allocated (e.g. due to other attached
    // clients constraining the window). If we send a layout bigger than
    // the window, tmux rejects it with "size mismatch".
    QSize tmuxSize = _tmuxWindowSizes.value(windowId);
    if (tmuxSize.isValid() && (node.width > tmuxSize.width() || node.height > tmuxSize.height())) {
        int clampW = qMin(node.width, tmuxSize.width());
        int clampH = qMin(node.height, tmuxSize.height());
        qCDebug(KonsoleTmuxResize) << "onSplitterMoved: clamping layout from"
                                   << node.width << "x" << node.height
                                   << "to" << clampW << "x" << clampH
                                   << "(tmux window size)";
        clampLayoutToSize(node, clampW, clampH);
    }

    QString layoutString = TmuxLayoutParser::serialize(node);
    qCDebug(KonsoleTmuxResize) << "onSplitterMoved: windowId=" << windowId << "tabIndex=" << tabIndex << "layout=" << layoutString;

    TmuxCommand selectLayout = TmuxCommand(QStringLiteral("select-layout"))
                                   .windowTarget(windowId)
                                   .singleQuotedArg(layoutString);
    qCDebug(KonsoleTmuxResize) << "onSplitterMoved: sending select-layout:" << selectLayout.build();
    _gateway->sendCommand(selectLayout);
}

void TmuxResizeCoordinator::sendClientSize()
{
    TabbedViewContainer *container = _viewManager->activeContainer();
    if (!container) {
        qCDebug(KonsoleTmuxResize) << "sendClientSize: no active container, aborting";
        return;
    }

    qCDebug(KonsoleTmuxResize) << "sendClientSize: activeTabIndex=" << container->currentIndex();

    const auto &windowToTab = _controller->windowToTabIndex();
    for (auto it = windowToTab.constBegin(); it != windowToTab.constEnd(); ++it) {
        int windowId = it.key();
        int tabIndex = it.value();

        TabPageWidget *page = container->tabPageAt(tabIndex);
        if (!page) {
            qCDebug(KonsoleTmuxResize) << "sendClientSize: no page for windowId=" << windowId << "tabIndex=" << tabIndex;
            continue;
        }

        auto *windowSplitter = page->splitter();
        if (!windowSplitter) {
            qCDebug(KonsoleTmuxResize) << "sendClientSize: no splitter for windowId=" << windowId << "tabIndex=" << tabIndex;
            continue;
        }

        // Measure the page (the full tab area), not the splitter or pane
        // content rectangles. When this client is inactive the splitter is
        // pinned by constrainSplitterToLayout to a sub-area of the page, and
        // per-pane contentRect excludes TerminalDisplay's inner margins — so
        // walking the pane tree under-reports by ~1 col per layout apply,
        // which with two clients creates a shrink feedback loop on each
        // focus swap. The page size is the client's true capacity and
        // mirrors what constrainSplitterToLayout compares against.
        auto displays = windowSplitter->findChildren<TerminalDisplay *>();
        if (displays.isEmpty()) {
            qCDebug(KonsoleTmuxResize) << "sendClientSize: no displays for windowId=" << windowId;
            continue;
        }
        auto *td = displays.first();
        int fontWidth = td->terminalFont()->fontWidth();
        int fontHeight = td->terminalFont()->fontHeight();
        if (fontWidth <= 0 || fontHeight <= 0) {
            qCDebug(KonsoleTmuxResize) << "sendClientSize: bad font metrics for windowId=" << windowId << "fontW=" << fontWidth << "fontH=" << fontHeight;
            continue;
        }

        QSize pageSize = page->size();
        int totalCols = qBound(1, pageSize.width() / fontWidth, 1023);
        int totalLines = qMax(1, pageSize.height() / fontHeight);

        qCDebug(KonsoleTmuxResize) << "  computeSize page: windowId=" << windowId << "pageSize=" << pageSize << "fontW=" << fontWidth << "fontH=" << fontHeight
                                   << "→ cols=" << totalCols << "lines=" << totalLines;

        if (totalCols <= 0 || totalLines <= 0) {
            qCDebug(KonsoleTmuxResize) << "sendClientSize: skipping windowId=" << windowId << "totalSize=" << QSize(totalCols, totalLines) << "(non-positive)";
            continue;
        }

        QSize &lastSize = _lastClientSizes[windowId];
        if (totalCols != lastSize.width() || totalLines != lastSize.height()) {
            qCDebug(KonsoleTmuxResize) << "sendClientSize: windowId=" << windowId
                                       << "size changed from" << lastSize << "to" << QSize(totalCols, totalLines)
                                       << "→ sending refresh-client -C";
            lastSize = QSize(totalCols, totalLines);
            _gateway->sendCommand(TmuxCommand(QStringLiteral("refresh-client"))
                                      .flag(QStringLiteral("-C"))
                                      .arg(QLatin1Char('@') + QString::number(windowId) + QLatin1Char(':') + QString::number(totalCols) + QLatin1Char('x')
                                           + QString::number(totalLines)));
        } else {
            qCDebug(KonsoleTmuxResize) << "sendClientSize: windowId=" << windowId << "size unchanged at" << lastSize << "→ skipping";
        }
    }
}

void TmuxResizeCoordinator::setWindowSize(int windowId, int cols, int lines)
{
    QSize newSize(cols, lines);
    if (_tmuxWindowSizes.value(windowId) != newSize) {
        qCDebug(KonsoleTmuxResize) << "setWindowSize: windowId=" << windowId << "size=" << newSize;
        _tmuxWindowSizes[windowId] = newSize;
    }
}

void TmuxResizeCoordinator::stop()
{
    _resizeTimer.stop();
}

} // namespace Konsole

#include "moc_TmuxResizeCoordinator.cpp"
