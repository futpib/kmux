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

// Cells the widget tree could draw if tmux gave it the chance, derived
// from each leaf's current pixel size minus chrome divided by font. The
// caller takes max() of this and the layout's already-in-use cell count
// (TmuxLayoutManager::buildLayoutNode), so:
//
//   - On a fresh attach the pixel capacity wins over a tmux session that
//     spawned at the default 80x24, telling tmux to grow the pane to fit
//     kmux's window.
//   - In a settled multi-pane window the layout cells win (the pixel
//     walk is fragile in offscreen tests when QSplitter children drift a
//     pixel during a re-layout), giving stable refresh-client values.
//
// Cross-axis composition takes max — tmux's HSplit forces all children
// to the parent's height (and VSplit to the parent's width), so the
// parent's true capacity is the larger of any child along that axis.
struct PaneCells {
    int cols;
    int rows;
};

static PaneCells maxCellsForWidget(QWidget *w)
{
    // Peel the container wrapper (kTerminalContainerProperty) around a leaf.
    // Use the wrapper's outer size — it owns the layout we resize against —
    // but the display's font metrics, so cell math stays consistent.
    if (auto *td = ViewSplitter::terminalDisplayForWidget(w)) {
        int fontW = td->terminalFont()->fontWidth();
        int fontH = td->terminalFont()->fontHeight();
        if (fontW <= 0 || fontH <= 0) {
            return {0, 0};
        }
        QSize size = (w == td) ? td->size() : w->size();
        QSize chrome = td->cellChromeSize();
        int cols = qMax(0, (size.width() - chrome.width()) / fontW);
        int rows = qMax(0, (size.height() - chrome.height()) / fontH);
        return {cols, rows};
    }
    auto *splitter = qobject_cast<ViewSplitter *>(w);
    if (!splitter || splitter->count() == 0) {
        return {0, 0};
    }
    bool horizontal = splitter->orientation() == Qt::Horizontal;
    int n = splitter->count();
    int sumCols = 0, sumRows = 0;
    int maxCols = 0, maxRows = 0;
    for (int i = 0; i < n; ++i) {
        PaneCells child = maxCellsForWidget(splitter->widget(i));
        sumCols += child.cols;
        sumRows += child.rows;
        maxCols = qMax(maxCols, child.cols);
        maxRows = qMax(maxRows, child.rows);
    }
    if (horizontal) {
        return {sumCols + (n - 1), maxRows};
    }
    return {maxCols, sumRows + (n - 1)};
}

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

        // Two views of "what does this client want as a window size?":
        //
        //   layoutCells = TmuxLayoutManager::buildLayoutNode(...)
        //     The cells already in use — display->columns()/lines() summed
        //     up the splitter tree the same way select-layout encodes them.
        //     Stable across calls (driven by tmux's setForcedSize), so
        //     repeated sendClientSize calls don't drift a pane by a cell
        //     and desync the pty from tmux's pane_width.
        //
        //   pixelCells = maxCellsForWidget(...)
        //     The cells we *could* draw if tmux let us, derived from each
        //     leaf's pixel size minus chrome. Bigger than layoutCells when
        //     kmux's pixels exceed what tmux currently allocates (initial
        //     attach where tmux's pane is still its default 28 cols, or
        //     after the user enlarges the kmux window).
        //
        // Use max(layoutCells, pixelCells) per axis so:
        //
        //   - A freshly-attached pane grows to the kmux window instead of
        //     staying pinned at tmux's default and scrolling recovered
        //     content off-screen.
        //   - A settled multi-pane layout keeps emitting the same number
        //     each refresh, even when QSplitter shifts a child a pixel
        //     during a re-layout — which is what makes the per-pane chrome
        //     accounting safe to deploy.
        //
        // Both walks compose per-pane (sum + n-1 for separators on the
        // split axis, max on the cross axis) — that's what makes multi-
        // pane work: each extra pane carries its own chrome, plus a Qt
        // splitter handle eats more pixels, and telling tmux N cols when
        // only N - K*chrome - handles fit is the multi-pane wrap bug.
        // Tmux divides by the layout, the rightmost cells fall outside
        // what kmux can render, and a TUI's full-row write wraps onto
        // the next row, scrolling correct content off the top.
        auto displays = windowSplitter->findChildren<TerminalDisplay *>();
        if (displays.isEmpty()) {
            qCDebug(KonsoleTmuxResize) << "sendClientSize: no displays for windowId=" << windowId;
            continue;
        }

        TmuxLayoutNode node = TmuxLayoutManager::buildLayoutNode(windowSplitter, _paneManager);
        PaneCells pixelCells = maxCellsForWidget(windowSplitter);
        int totalCols = qBound(1, qMax(node.width, pixelCells.cols), 1023);
        int totalLines = qMax(1, qMax(node.height, pixelCells.rows));

        qCDebug(KonsoleTmuxResize) << "  computeSize tree: windowId=" << windowId << "pageSize=" << page->size() << "splitterSize=" << windowSplitter->size()
                                   << "layoutCells=" << QSize(node.width, node.height) << "pixelCells=" << QSize(pixelCells.cols, pixelCells.rows)
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
