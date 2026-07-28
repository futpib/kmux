/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxTestFixture.h"

#include <QCoreApplication>
#include <QProcess>
#include <QResizeEvent>
#include <QStandardPaths>
#include <QTest>

#include "../MainWindow.h"
#include "../ViewManager.h"
#include "../profile/ProfileManager.h"
#include "../session/Session.h"
#include "../terminalDisplay/TerminalDisplay.h"
#include "../terminalDisplay/TerminalFonts.h"
#include "../tmux/TmuxLayoutParser.h"
#include "../tmux/TmuxProcessBridge.h"
#include "../widgets/ViewContainer.h"
#include "../widgets/ViewSplitter.h"

using namespace Konsole;

namespace
{

// Collect all pane dimensions (columns, lines) from leaf nodes in order
void collectPaneDimensions(const TmuxTestFixture::LayoutSpec &layout, QList<QPair<int, int>> &dims)
{
    if (layout.type == TmuxTestFixture::LayoutSpec::Leaf) {
        dims.append(qMakePair(layout.pane.columns, layout.pane.lines));
    } else {
        for (const auto &child : layout.children) {
            collectPaneDimensions(child, dims);
        }
    }
}

// Collect all pane commands from a layout tree in order
void collectPaneCommands(const TmuxTestFixture::LayoutSpec &layout, QStringList &cmds)
{
    if (layout.type == TmuxTestFixture::LayoutSpec::Leaf) {
        cmds.append(layout.pane.command);
    } else {
        for (const auto &child : layout.children) {
            collectPaneCommands(child, cmds);
        }
    }
}

// Propagate height to all nodes in a subtree (for HSplit parent constraint)
void setSubtreeHeight(TmuxLayoutNode &node, int height)
{
    if (node.type == TmuxLayoutNodeType::Leaf) {
        node.height = height;
    } else if (node.type == TmuxLayoutNodeType::HSplit) {
        node.height = height;
        for (auto &c : node.children) {
            setSubtreeHeight(c, height);
        }
    } else {
        // VSplit: set outer height, don't recurse into children
        node.height = height;
    }
}

// Propagate width to all nodes in a subtree (for VSplit parent constraint)
void setSubtreeWidth(TmuxLayoutNode &node, int width)
{
    if (node.type == TmuxLayoutNodeType::Leaf) {
        node.width = width;
    } else if (node.type == TmuxLayoutNodeType::VSplit) {
        node.width = width;
        for (auto &c : node.children) {
            setSubtreeWidth(c, width);
        }
    } else {
        // HSplit: set outer width, don't recurse into children
        node.width = width;
    }
}

// Build a TmuxLayoutNode from a LayoutSpec and a list of tmux pane IDs (in leaf order).
// baseX/baseY are the absolute position of this node within the tmux window.
// This produces a layout string that select-layout can apply atomically.
TmuxLayoutNode buildTmuxLayoutFromSpec(const TmuxTestFixture::LayoutSpec &layout, const QList<int> &paneIds, int &leafIndex, int baseX = 0, int baseY = 0)
{
    TmuxLayoutNode node;

    if (layout.type == TmuxTestFixture::LayoutSpec::Leaf) {
        node.type = TmuxLayoutNodeType::Leaf;
        node.width = layout.pane.columns;
        node.height = layout.pane.lines;
        node.xOffset = baseX;
        node.yOffset = baseY;
        node.paneId = (leafIndex < paneIds.size()) ? paneIds[leafIndex] : leafIndex;
        ++leafIndex;
        return node;
    }

    node.type = (layout.type == TmuxTestFixture::LayoutSpec::HSplit) ? TmuxLayoutNodeType::HSplit : TmuxLayoutNodeType::VSplit;
    bool horizontal = (node.type == TmuxLayoutNodeType::HSplit);

    int offset = 0;
    int maxCross = 0;
    for (const auto &child : layout.children) {
        int childX = horizontal ? (baseX + offset) : baseX;
        int childY = horizontal ? baseY : (baseY + offset);
        TmuxLayoutNode childNode = buildTmuxLayoutFromSpec(child, paneIds, leafIndex, childX, childY);

        if (horizontal) {
            offset += childNode.width + 1; // +1 for separator
            maxCross = qMax(maxCross, childNode.height);
        } else {
            offset += childNode.height + 1; // +1 for separator
            maxCross = qMax(maxCross, childNode.width);
        }

        node.children.append(childNode);
    }

    if (horizontal) {
        node.width = offset > 0 ? offset - 1 : 0;
        node.height = maxCross;
        for (auto &c : node.children) {
            setSubtreeHeight(c, maxCross);
        }
    } else {
        node.width = maxCross;
        node.height = offset > 0 ? offset - 1 : 0;
        for (auto &c : node.children) {
            setSubtreeWidth(c, maxCross);
        }
    }

    node.xOffset = baseX;
    node.yOffset = baseY;
    return node;
}

// Recursively walk the layout tree and splitter tree in parallel,
// collecting (TerminalDisplay*, PaneSpec) pairs for leaf nodes.
void collectDisplayPanePairs(const TmuxTestFixture::LayoutSpec &layout,
                             ViewSplitter *splitter,
                             QList<QPair<TerminalDisplay *, TmuxTestFixture::PaneSpec>> &pairs)
{
    if (layout.type == TmuxTestFixture::LayoutSpec::Leaf) {
        // The splitter's widget at this level should be a TerminalDisplay
        // (or the splitter itself is the parent and we were called for a leaf child)
        // When called from a split parent, splitter is actually the parent splitter
        // and we need to get the child widget at the right index.
        // But this function is called with the correct widget — if it's a leaf,
        // the widget passed should be a TerminalDisplay's parent splitter.
        // Actually, for leaves we get called from the split-level iteration below,
        // where we pass the child widget. If the child is a TerminalDisplay directly,
        // the splitter parameter may be null. We handle this by having the caller
        // pass the display directly via a separate path.
        // Let's handle both cases:
        if (splitter) {
            // Leaf inside a splitter that has exactly one TerminalDisplay
            auto displays = splitter->findChildren<TerminalDisplay *>(Qt::FindDirectChildrenOnly);
            if (!displays.isEmpty()) {
                pairs.append(qMakePair(displays.first(), layout.pane));
            }
        }
        return;
    }

    if (!splitter) {
        return;
    }

    for (int i = 0; i < layout.children.size() && i < splitter->count(); ++i) {
        const auto &child = layout.children[i];
        QWidget *childWidget = splitter->widget(i);

        if (child.type == TmuxTestFixture::LayoutSpec::Leaf) {
            // Child widget should be a TerminalDisplay
            auto *display = qobject_cast<TerminalDisplay *>(childWidget);
            if (display) {
                pairs.append(qMakePair(display, child.pane));
            }
        } else {
            // Child widget should be a ViewSplitter
            auto *childSplitter = qobject_cast<ViewSplitter *>(childWidget);
            collectDisplayPanePairs(child, childSplitter, pairs);
        }
    }
}

// Find the pane splitter tab in the container that matches the expected pane count.
ViewSplitter *findPaneSplitter(TabbedViewContainer *container, int expectedPanes)
{
    for (int i = 0; i < container->count(); ++i) {
        auto *splitter = container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == expectedPanes) {
                return splitter;
            }
        }
    }
    return nullptr;
}

// Compute the pixel size a TerminalDisplay needs so that calcGeometry()
// will yield the given columns and lines.
// Uses TerminalDisplay::setSize() as a base, then adds the highlight scrolled
// lines width that setSize() doesn't account for but calcGeometry() subtracts.
QSize displayPixelSize(TerminalDisplay *display, int columns, int lines)
{
    // Save original values
    int origCols = display->columns();
    int origLines = display->lines();

    // setSize(columns, lines) computes the pixel size and stores it in _size
    display->setSize(columns, lines);
    QSize result = display->sizeHint();

    // Restore original
    display->setSize(origCols, origLines);

    // setSize() doesn't account for HighlightScrolledLines width, but calcGeometry()
    // subtracts it from the content rect. HIGHLIGHT_SCROLLED_LINES_WIDTH = 3 per side.
    // Add this to prevent losing columns due to the mismatch.
    static const int HIGHLIGHT_SCROLLED_LINES_WIDTH = 3;
    result.setWidth(result.width() + 2 * HIGHLIGHT_SCROLLED_LINES_WIDTH);

    return result;
}

// Verify splitter tree structure matches layout spec
bool verifySplitterStructure(const TmuxTestFixture::LayoutSpec &layout, ViewSplitter *splitter)
{
    if (layout.type == TmuxTestFixture::LayoutSpec::Leaf) {
        // A leaf should be a single TerminalDisplay (or a ViewSplitter with one child)
        return true;
    }

    if (!splitter) {
        return false;
    }

    // Check orientation
    Qt::Orientation expectedOrientation = (layout.type == TmuxTestFixture::LayoutSpec::HSplit) ? Qt::Horizontal : Qt::Vertical;
    if (splitter->orientation() != expectedOrientation) {
        return false;
    }

    // Check child count
    if (splitter->count() != layout.children.size()) {
        return false;
    }

    // Recursively check children
    for (int i = 0; i < layout.children.size(); ++i) {
        if (layout.children[i].type != TmuxTestFixture::LayoutSpec::Leaf) {
            auto *childSplitter = qobject_cast<ViewSplitter *>(splitter->widget(i));
            if (!verifySplitterStructure(layout.children[i], childSplitter)) {
                return false;
            }
        }
    }

    return true;
}

} // anonymous namespace

namespace Konsole
{
namespace TmuxTestFixture
{

LayoutSpec pane(const QString &command, int columns, int lines, bool focused)
{
    LayoutSpec layout;
    layout.pane.command = command;
    layout.pane.columns = columns;
    layout.pane.lines = lines;
    layout.pane.focused = focused;
    return layout;
}

LayoutSpec horizontal(std::initializer_list<LayoutSpec> children)
{
    LayoutSpec layout;
    layout.type = LayoutSpec::HSplit;
    layout.children = QList<LayoutSpec>(children);
    return layout;
}

LayoutSpec vertical(std::initializer_list<LayoutSpec> children)
{
    LayoutSpec layout;
    layout.type = LayoutSpec::VSplit;
    layout.children = QList<LayoutSpec>(children);
    return layout;
}

int countPanes(const LayoutSpec &layout)
{
    if (layout.type == LayoutSpec::Leaf) {
        return 1;
    }
    int count = 0;
    for (const auto &child : layout.children) {
        count += countPanes(child);
    }
    return count;
}

QPair<int, int> computeWindowSize(const LayoutSpec &layout)
{
    if (layout.type == LayoutSpec::Leaf) {
        return qMakePair(layout.pane.columns, layout.pane.lines);
    }

    if (layout.type == LayoutSpec::HSplit) {
        // Sum widths + (N-1) separators, max height
        int totalWidth = 0;
        int maxHeight = 0;
        for (int i = 0; i < layout.children.size(); ++i) {
            auto childSize = computeWindowSize(layout.children[i]);
            totalWidth += childSize.first;
            if (i > 0) {
                totalWidth += 1; // separator column
            }
            maxHeight = qMax(maxHeight, childSize.second);
        }
        return qMakePair(totalWidth, maxHeight);
    }

    // VSplit: max width, sum heights + (N-1) separators
    int maxWidth = 0;
    int totalHeight = 0;
    for (int i = 0; i < layout.children.size(); ++i) {
        auto childSize = computeWindowSize(layout.children[i]);
        maxWidth = qMax(maxWidth, childSize.first);
        totalHeight += childSize.second;
        if (i > 0) {
            totalHeight += 1; // separator row
        }
    }
    return qMakePair(maxWidth, totalHeight);
}

void setupSinglePane(const QString &command, const QString &tmuxPath, const QString &socketDir, SessionContext &ctx, int columns, int lines)
{
    setupTmuxSession(pane(command, columns, lines), tmuxPath, socketDir, ctx);
}

void setupTmuxSession(const LayoutSpec &layout, const QString &tmuxPath, const QString &socketDir, SessionContext &ctx)
{
    static int sessionCounter = 0;
    ctx.sessionName = QStringLiteral("konsole-fixture-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(sessionCounter);
    ctx.socketPath = socketDir + QStringLiteral("/tmux-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(sessionCounter++);

    // Collect all pane commands
    QStringList cmds;
    collectPaneCommands(layout, cmds);
    QString firstCmd = cmds.isEmpty() ? QStringLiteral("sleep 30") : cmds.first();
    if (firstCmd.isEmpty()) {
        firstCmd = QStringLiteral("sleep 30");
    }

    // Build new-session arguments
    QStringList args = {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-session"), QStringLiteral("-d"), QStringLiteral("-s"), ctx.sessionName};

    auto windowSize = computeWindowSize(layout);
    args << QStringLiteral("-x") << QString::number(windowSize.first);
    args << QStringLiteral("-y") << QString::number(windowSize.second);

    args << firstCmd;

    QProcess tmuxNewSession;
    tmuxNewSession.start(tmuxPath, args);
    QVERIFY(tmuxNewSession.waitForFinished(5000));
    QCOMPARE(tmuxNewSession.exitCode(), 0);

    // Create splits according to the layout.
    if (layout.type != LayoutSpec::Leaf) {
        struct SplitTask {
            LayoutSpec layout;
            int tmuxPaneIndex;
        };

        QList<SplitTask> tasks;
        tasks.append({layout, 0});
        int nextPaneIndex = 1;

        while (!tasks.isEmpty()) {
            SplitTask task = tasks.takeFirst();

            if (task.layout.type == LayoutSpec::Leaf) {
                continue;
            }

            QString dir = (task.layout.type == LayoutSpec::HSplit) ? QStringLiteral("-h") : QStringLiteral("-v");

            // First child inherits the current pane index
            int firstChildPaneIndex = task.tmuxPaneIndex;
            tasks.append({task.layout.children[0], firstChildPaneIndex});

            // Subsequent children need splits
            for (int i = 1; i < task.layout.children.size(); ++i) {
                // Get command for this child's first leaf
                QStringList childCmds;
                collectPaneCommands(task.layout.children[i], childCmds);
                QString childCmd = childCmds.isEmpty() ? QStringLiteral("sleep 30") : childCmds.first();
                if (childCmd.isEmpty()) {
                    childCmd = QStringLiteral("sleep 30");
                }

                // Split the target pane
                // For splits after the first, we need to target the right pane.
                // When splitting horizontally from pane N, tmux creates a new pane to the right.
                // The new pane gets the next available index.
                const int targetPane = firstChildPaneIndex;

                QProcess tmuxSplit;
                tmuxSplit.start(tmuxPath,
                                {QStringLiteral("-S"),
                                 ctx.socketPath,
                                 QStringLiteral("split-window"),
                                 dir,
                                 QStringLiteral("-t"),
                                 QStringLiteral("%1:%2.%3").arg(ctx.sessionName).arg(0).arg(targetPane),
                                 childCmd});
                QVERIFY2(tmuxSplit.waitForFinished(5000), qPrintable(QStringLiteral("split-window timed out")));
                QCOMPARE(tmuxSplit.exitCode(), 0);

                int newPaneIndex = nextPaneIndex++;
                tasks.append({task.layout.children[i], newPaneIndex});
            }
        }
    }

    // Set exact pane dimensions.
    // First try resize-pane for each pane (works for simple layouts).
    // If verification fails, fall back to select-layout (atomic, handles complex layouts).
    {
        QList<QPair<int, int>> expectedDims;
        collectPaneDimensions(layout, expectedDims);

        QProcess tmuxListPanes;
        tmuxListPanes.start(tmuxPath,
                            {QStringLiteral("-S"),
                             ctx.socketPath,
                             QStringLiteral("list-panes"),
                             QStringLiteral("-t"),
                             ctx.sessionName,
                             QStringLiteral("-F"),
                             QStringLiteral("#{pane_index} #{pane_id}")});
        QVERIFY(tmuxListPanes.waitForFinished(5000));
        QCOMPARE(tmuxListPanes.exitCode(), 0);
        QStringList paneLines = QString::fromUtf8(tmuxListPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));

        int expectedPanes = countPanes(layout);
        QCOMPARE(paneLines.size(), expectedPanes);

        QList<int> paneIndices;
        QList<int> paneIds;
        for (const QString &line : paneLines) {
            QStringList parts = line.split(QLatin1Char(' '));
            paneIndices.append(parts[0].toInt());
            paneIds.append(parts[1].mid(1).toInt()); // strip % prefix
        }

        // Try resize-pane for each pane (may fail silently for complex layouts)
        for (int i = 0; i < expectedPanes; ++i) {
            QProcess resize;
            resize.start(tmuxPath,
                         {QStringLiteral("-S"),
                          ctx.socketPath,
                          QStringLiteral("resize-pane"),
                          QStringLiteral("-t"),
                          QStringLiteral("%1:%2.%3").arg(ctx.sessionName).arg(0).arg(paneIndices[i]),
                          QStringLiteral("-x"),
                          QString::number(expectedDims[i].first),
                          QStringLiteral("-y"),
                          QString::number(expectedDims[i].second)});
            QVERIFY2(resize.waitForFinished(5000), qPrintable(QStringLiteral("resize-pane timed out for pane %1").arg(paneIndices[i])));
        }

        // Verify dimensions — if any mismatch, fall back to select-layout
        QProcess verifyPanes;
        verifyPanes.start(tmuxPath,
                          {QStringLiteral("-S"),
                           ctx.socketPath,
                           QStringLiteral("list-panes"),
                           QStringLiteral("-t"),
                           ctx.sessionName,
                           QStringLiteral("-F"),
                           QStringLiteral("#{pane_width} #{pane_height}")});
        QVERIFY(verifyPanes.waitForFinished(5000));
        QStringList verifyLines = QString::fromUtf8(verifyPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));

        bool needsFallback = (verifyLines.size() != expectedPanes);
        if (!needsFallback) {
            for (int i = 0; i < expectedPanes; ++i) {
                QStringList parts = verifyLines[i].split(QLatin1Char(' '));
                if (parts.size() != 2 || parts[0].toInt() != expectedDims[i].first || parts[1].toInt() != expectedDims[i].second) {
                    needsFallback = true;
                    break;
                }
            }
        }

        if (needsFallback) {
            // Build a TmuxLayoutNode and use select-layout for atomic layout application
            int leafIndex = 0;
            TmuxLayoutNode layoutNode = buildTmuxLayoutFromSpec(layout, paneIds, leafIndex);
            QString layoutString = TmuxLayoutParser::serialize(layoutNode);

            auto windowSize = computeWindowSize(layout);

            QProcess selectLayout1;
            selectLayout1.start(tmuxPath,
                                {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("select-layout"), QStringLiteral("-t"), ctx.sessionName, layoutString});
            QVERIFY(selectLayout1.waitForFinished(5000));

            QProcess resizeWindow;
            resizeWindow.start(tmuxPath,
                               {QStringLiteral("-S"),
                                ctx.socketPath,
                                QStringLiteral("resize-window"),
                                QStringLiteral("-t"),
                                ctx.sessionName,
                                QStringLiteral("-x"),
                                QString::number(windowSize.first),
                                QStringLiteral("-y"),
                                QString::number(windowSize.second)});
            QVERIFY(resizeWindow.waitForFinished(5000));

            QProcess selectLayout2;
            selectLayout2.start(tmuxPath,
                                {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("select-layout"), QStringLiteral("-t"), ctx.sessionName, layoutString});
            QVERIFY2(selectLayout2.waitForFinished(5000), qPrintable(QStringLiteral("select-layout timed out")));
            QCOMPARE(selectLayout2.exitCode(), 0);
        }
    }

    // Post-setup verification: assert exact pane dimensions
    {
        QList<QPair<int, int>> expectedDims;
        collectPaneDimensions(layout, expectedDims);

        QProcess tmuxListPanes;
        tmuxListPanes.start(tmuxPath,
                            {QStringLiteral("-S"),
                             ctx.socketPath,
                             QStringLiteral("list-panes"),
                             QStringLiteral("-t"),
                             ctx.sessionName,
                             QStringLiteral("-F"),
                             QStringLiteral("#{pane_width} #{pane_height}")});
        QVERIFY(tmuxListPanes.waitForFinished(5000));
        QCOMPARE(tmuxListPanes.exitCode(), 0);
        QStringList paneLines = QString::fromUtf8(tmuxListPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));

        int expectedPanes = countPanes(layout);
        QCOMPARE(paneLines.size(), expectedPanes);

        for (int i = 0; i < paneLines.size(); ++i) {
            QStringList parts = paneLines[i].split(QLatin1Char(' '));
            QCOMPARE(parts.size(), 2);
            int actualWidth = parts[0].toInt();
            int actualHeight = parts[1].toInt();
            QCOMPARE(actualWidth, expectedDims[i].first);
            QCOMPARE(actualHeight, expectedDims[i].second);
        }
    }
}

void attachKonsole(const QString &tmuxPath, const SessionContext &ctx, AttachResult &result)
{
    auto *mw = new MainWindow();
    result.mw = mw;
    ViewManager *vm = mw->viewManager();

    auto *bridge = new TmuxProcessBridge(vm, mw);
    result.bridge = bridge;

    bool started = bridge->start(tmuxPath,
                                 {QStringLiteral("-S"), ctx.socketPath},
                                 {QStringLiteral("new-session"), QStringLiteral("-A"), QStringLiteral("-s"), ctx.sessionName});
    QVERIFY(started);

    result.container = vm->activeContainer();
    QVERIFY(result.container);

    // Wait for tmux control mode to create pane tab(s)
    QTRY_VERIFY_WITH_TIMEOUT(result.container && result.container->count() >= 1, 10000);
}

void applyKonsoleLayout(const LayoutSpec &layout, ViewManager *vm)
{
    auto *container = vm->activeContainer();
    QVERIFY(container);

    int expectedPanes = countPanes(layout);
    ViewSplitter *paneSplitter = findPaneSplitter(container, expectedPanes);
    QVERIFY2(paneSplitter, qPrintable(QStringLiteral("Expected a ViewSplitter with %1 TerminalDisplay children").arg(expectedPanes)));

    // Get font metrics from the first TerminalDisplay
    auto *firstDisplay = paneSplitter->findChildren<TerminalDisplay *>().first();
    QVERIFY(firstDisplay);
    QVERIFY(firstDisplay->terminalFont()->fontWidth() > 0);
    QVERIFY(firstDisplay->terminalFont()->fontHeight() > 0);

    // Collect all (display, pane) pairs
    QList<QPair<TerminalDisplay *, PaneSpec>> pairs;
    if (layout.type == LayoutSpec::Leaf) {
        pairs.append(qMakePair(firstDisplay, layout.pane));
    } else {
        collectDisplayPanePairs(layout, paneSplitter, pairs);
    }

    // Resize each display individually and send resize events.
    // This approach works even when the widget isn't shown (offscreen tests).
    for (const auto &pair : pairs) {
        auto *display = pair.first;
        int cols = pair.second.columns;
        int lns = pair.second.lines;
        QSize targetSize = displayPixelSize(display, cols, lns);
        QSize oldSize = display->size();
        display->resize(targetSize);
        QResizeEvent resizeEvent(targetSize, oldSize);
        QCoreApplication::sendEvent(display, &resizeEvent);
    }
    QCoreApplication::processEvents();

    // Handle focus
    for (const auto &pair : pairs) {
        if (pair.second.focused) {
            pair.first->setFocus(Qt::OtherFocusReason);
        }
    }
}

void assertKonsoleLayout(const LayoutSpec &layout, ViewManager *vm)
{
    auto *container = vm->activeContainer();
    QVERIFY(container);

    // Find the pane tab (the one with a ViewSplitter containing TerminalDisplays, not the gateway)
    ViewSplitter *paneSplitter = nullptr;
    int expectedPanes = countPanes(layout);

    for (int i = 0; i < container->count(); ++i) {
        auto *splitter = container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == expectedPanes) {
                paneSplitter = splitter;
                break;
            }
        }
    }

    QVERIFY2(paneSplitter, qPrintable(QStringLiteral("Expected a ViewSplitter with %1 TerminalDisplay children").arg(expectedPanes)));

    // Check orientation derived from layout type
    if (layout.type != LayoutSpec::Leaf) {
        Qt::Orientation expected = (layout.type == LayoutSpec::HSplit) ? Qt::Horizontal : Qt::Vertical;
        QCOMPARE(paneSplitter->orientation(), expected);
    }

    // Verify structure matches layout tree
    if (layout.type != LayoutSpec::Leaf) {
        QVERIFY2(verifySplitterStructure(layout, paneSplitter), "ViewSplitter tree structure does not match fixture");
    }

    // Collect (display, pane) pairs and verify dimensions and focus
    QList<QPair<TerminalDisplay *, PaneSpec>> pairs;
    if (layout.type == LayoutSpec::Leaf) {
        // Single pane: the splitter should contain exactly one TerminalDisplay
        auto displays = paneSplitter->findChildren<TerminalDisplay *>();
        QVERIFY(!displays.isEmpty());
        pairs.append(qMakePair(displays.first(), layout.pane));
    } else {
        collectDisplayPanePairs(layout, paneSplitter, pairs);
    }

    // Verify dimensions for each leaf pane
    for (const auto &pair : pairs) {
        auto *display = pair.first;
        const auto &pane = pair.second;

        QVERIFY2(display->columns() == pane.columns, qPrintable(QStringLiteral("Display columns %1 != expected %2").arg(display->columns()).arg(pane.columns)));
        QVERIFY2(display->lines() == pane.lines, qPrintable(QStringLiteral("Display lines %1 != expected %2").arg(display->lines()).arg(pane.lines)));
    }

    // Verify focus
    for (const auto &pair : pairs) {
        if (pair.second.focused) {
            QVERIFY2(pair.first->hasFocus(), "Expected pane should have focus but doesn't");
        }
    }
}

void killTmuxSession(const QString &tmuxPath, const SessionContext &ctx)
{
    QProcess tmuxKill;
    tmuxKill.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("kill-session"), QStringLiteral("-t"), ctx.sessionName});
    tmuxKill.waitForFinished(5000);
}

QString findTmuxOrSkip()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        // Can't call QSKIP from a non-test function directly, so return empty
        // The caller should check and QSKIP
    }
    return tmuxPath;
}

} // namespace TmuxTestFixture
} // namespace Konsole
