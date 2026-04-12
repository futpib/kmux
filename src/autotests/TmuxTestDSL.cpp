/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxTestDSL.h"

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

// Dedent: strip common leading whitespace from all non-empty lines
QStringList dedentLines(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));

    // Find minimum indentation of non-empty lines
    int minIndent = INT_MAX;
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        int indent = 0;
        for (int i = 0; i < line.size(); ++i) {
            if (line[i] == QLatin1Char(' ')) {
                ++indent;
            } else if (line[i] == QLatin1Char('\t')) {
                indent += 4;
            } else {
                break;
            }
        }
        minIndent = qMin(minIndent, indent);
    }

    if (minIndent == INT_MAX) {
        minIndent = 0;
    }

    QStringList result;
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) {
            result.append(QString());
        } else {
            // Strip minIndent characters of leading whitespace
            int stripped = 0;
            int pos = 0;
            while (pos < line.size() && stripped < minIndent) {
                if (line[pos] == QLatin1Char(' ')) {
                    ++stripped;
                    ++pos;
                } else if (line[pos] == QLatin1Char('\t')) {
                    stripped += 4;
                    ++pos;
                } else {
                    break;
                }
            }
            result.append(line.mid(pos));
        }
    }

    // Trim leading/trailing empty lines
    while (!result.isEmpty() && result.first().isEmpty()) {
        result.removeFirst();
    }
    while (!result.isEmpty() && result.last().isEmpty()) {
        result.removeLast();
    }

    return result;
}

QChar charAt(const QStringList &lines, int row, int col)
{
    if (row < 0 || row >= lines.size()) {
        return QChar();
    }
    // Each box-drawing char is multi-byte in UTF-8 but one QChar in UTF-16...
    // Actually, box-drawing chars are in the U+2500 range, which is a single QChar.
    if (col < 0 || col >= lines[row].size()) {
        return QChar();
    }
    return lines[row][col];
}

// Parse key-value annotations from lines within a pane region
TmuxTestDSL::PaneSpec parseAnnotations(const QStringList &lines, int top, int left, int bottom, int right)
{
    TmuxTestDSL::PaneSpec pane;
    QString lastKey;

    for (int row = top + 1; row < bottom; ++row) {
        // Extract text between left border and right border
        // The borders are at columns left and right
        int startCol = left + 1;
        int endCol = right;
        if (startCol >= lines[row].size()) {
            continue;
        }
        // Check that left border is │
        if (charAt(lines, row, left) != QChar(0x2502)) { // │
            continue;
        }

        QString interior;
        if (endCol <= lines[row].size()) {
            interior = lines[row].mid(startCol, endCol - startCol);
        } else {
            interior = lines[row].mid(startCol);
        }
        interior = interior.trimmed();

        if (interior.isEmpty()) {
            continue;
        }

        // Check if this is a key: value line
        int colonPos = interior.indexOf(QLatin1Char(':'));
        if (colonPos > 0 && colonPos < interior.size()) {
            QString key = interior.left(colonPos).trimmed().toLower();
            QString value = interior.mid(colonPos + 1).trimmed();

            if (key == QStringLiteral("id")) {
                pane.id = value;
            } else if (key == QStringLiteral("cmd")) {
                pane.cmd = value;
            } else if (key == QStringLiteral("title")) {
                pane.title = value;
            } else if (key == QStringLiteral("contains")) {
                if (!value.isEmpty()) {
                    pane.contains.append(value);
                }
            } else if (key == QStringLiteral("focused")) {
                pane.focused = (value.toLower() == QStringLiteral("true"));
            } else if (key == QStringLiteral("columns")) {
                pane.columns = value.toInt();
            } else if (key == QStringLiteral("lines")) {
                pane.lines = value.toInt();
            }
            lastKey = key;
        } else if (!lastKey.isEmpty()) {
            // Continuation line: append to previous key's value
            if (lastKey == QStringLiteral("cmd")) {
                if (!pane.cmd.isEmpty()) {
                    pane.cmd += QLatin1Char(' ');
                }
                pane.cmd += interior;
            } else if (lastKey == QStringLiteral("contains")) {
                pane.contains.append(interior);
            } else if (lastKey == QStringLiteral("title")) {
                if (!pane.title.isEmpty()) {
                    pane.title += QLatin1Char(' ');
                }
                pane.title += interior;
            }
        }
    }

    return pane;
}

// Recursive parser: parse a rectangular region of the box drawing
TmuxTestDSL::LayoutSpec parseRegion(const QStringList &lines, int top, int left, int bottom, int right)
{
    // Scan top border for ┬ (U+252C) where bottom border has ┴ (U+2534) or ┼ (U+253C)
    // This indicates a vertical split (side-by-side panes = HSplit in our terminology,
    // but actually the box ┬ means a vertical divider between horizontally arranged panes)
    QList<int> vsplitCols;
    for (int col = left + 1; col < right; ++col) {
        QChar topChar = charAt(lines, top, col);
        QChar botChar = charAt(lines, bottom, col);
        if ((topChar == QChar(0x252C) || topChar == QChar(0x253C))       // ┬ or ┼
            && (botChar == QChar(0x2534) || botChar == QChar(0x253C))) { // ┴ or ┼
            // Verify the divider runs the full height
            bool fullDivider = true;
            for (int row = top + 1; row < bottom; ++row) {
                QChar ch = charAt(lines, row, col);
                if (ch != QChar(0x2502) && ch != QChar(0x253C)       // │ or ┼
                    && ch != QChar(0x251C) && ch != QChar(0x2524)) { // ├ or ┤
                    fullDivider = false;
                    break;
                }
            }
            if (fullDivider) {
                vsplitCols.append(col);
            }
        }
    }

    if (!vsplitCols.isEmpty()) {
        // HSplit (side-by-side panes separated by vertical dividers)
        TmuxTestDSL::LayoutSpec spec;
        spec.type = TmuxTestDSL::LayoutSpec::HSplit;

        int prevCol = left;
        for (int splitCol : vsplitCols) {
            spec.children.append(parseRegion(lines, top, prevCol, bottom, splitCol));
            prevCol = splitCol;
        }
        spec.children.append(parseRegion(lines, top, prevCol, bottom, right));
        return spec;
    }

    // Scan left border for ├ (U+251C) where right border has ┤ (U+2524) or ┼ (U+253C)
    // This indicates a horizontal split (stacked panes = VSplit)
    QList<int> hsplitRows;
    for (int row = top + 1; row < bottom; ++row) {
        QChar leftChar = charAt(lines, row, left);
        QChar rightChar = charAt(lines, row, right);
        if ((leftChar == QChar(0x251C) || leftChar == QChar(0x253C))         // ├ or ┼
            && (rightChar == QChar(0x2524) || rightChar == QChar(0x253C))) { // ┤ or ┼
            // Verify the divider runs the full width
            bool fullDivider = true;
            for (int col = left + 1; col < right; ++col) {
                QChar ch = charAt(lines, row, col);
                if (ch != QChar(0x2500) && ch != QChar(0x253C)       // ─ or ┼
                    && ch != QChar(0x252C) && ch != QChar(0x2534)) { // ┬ or ┴
                    fullDivider = false;
                    break;
                }
            }
            if (fullDivider) {
                hsplitRows.append(row);
            }
        }
    }

    if (!hsplitRows.isEmpty()) {
        // VSplit (stacked panes separated by horizontal dividers)
        TmuxTestDSL::LayoutSpec spec;
        spec.type = TmuxTestDSL::LayoutSpec::VSplit;

        int prevRow = top;
        for (int splitRow : hsplitRows) {
            spec.children.append(parseRegion(lines, prevRow, left, splitRow, right));
            prevRow = splitRow;
        }
        spec.children.append(parseRegion(lines, prevRow, left, bottom, right));
        return spec;
    }

    // Leaf pane: parse annotations from interior
    TmuxTestDSL::LayoutSpec spec;
    spec.type = TmuxTestDSL::LayoutSpec::Leaf;
    spec.pane = parseAnnotations(lines, top, left, bottom, right);

    // Auto-populate columns/lines from box interior dimensions if not explicitly set
    if (!spec.pane.columns.has_value()) {
        spec.pane.columns = right - left - 1;
    }
    if (!spec.pane.lines.has_value()) {
        spec.pane.lines = bottom - top - 1;
    }

    return spec;
}

// Parse footer metadata lines (after the bottom border)
void parseFooter(const QStringList &footerLines, TmuxTestDSL::DiagramSpec &spec)
{
    for (const QString &line : footerLines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }

        int colonPos = trimmed.indexOf(QLatin1Char(':'));
        if (colonPos <= 0) {
            continue;
        }

        QString key = trimmed.left(colonPos).trimmed().toLower();
        QString value = trimmed.mid(colonPos + 1).trimmed();

        if (key == QStringLiteral("tab")) {
            spec.tab = value;
        } else if (key == QStringLiteral("ratio")) {
            QStringList parts = value.split(QLatin1Char(':'));
            QList<int> ratioValues;
            for (const QString &p : parts) {
                ratioValues.append(p.trimmed().toInt());
            }
            spec.ratio = ratioValues;
        }
    }
}

// Collect all pane dimensions (columns, lines) from leaf nodes in order
void collectPaneDimensions(const TmuxTestDSL::LayoutSpec &layout, QList<QPair<int, int>> &dims)
{
    if (layout.type == TmuxTestDSL::LayoutSpec::Leaf) {
        dims.append(qMakePair(layout.pane.columns.value_or(80), layout.pane.lines.value_or(24)));
    } else {
        for (const auto &child : layout.children) {
            collectPaneDimensions(child, dims);
        }
    }
}

// Collect all pane IDs from a layout tree
void collectPaneIds(const TmuxTestDSL::LayoutSpec &layout, QStringList &ids)
{
    if (layout.type == TmuxTestDSL::LayoutSpec::Leaf) {
        if (!layout.pane.id.isEmpty()) {
            ids.append(layout.pane.id);
        }
    } else {
        for (const auto &child : layout.children) {
            collectPaneIds(child, ids);
        }
    }
}

// Collect all pane commands from a layout tree in order
void collectPaneCommands(const TmuxTestDSL::LayoutSpec &layout, QStringList &cmds)
{
    if (layout.type == TmuxTestDSL::LayoutSpec::Leaf) {
        cmds.append(layout.pane.cmd);
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
TmuxLayoutNode buildTmuxLayoutFromSpec(const TmuxTestDSL::LayoutSpec &layout, const QList<int> &paneIds, int &leafIndex,
                                       int baseX = 0, int baseY = 0)
{
    TmuxLayoutNode node;

    if (layout.type == TmuxTestDSL::LayoutSpec::Leaf) {
        node.type = TmuxLayoutNodeType::Leaf;
        node.width = layout.pane.columns.value_or(80);
        node.height = layout.pane.lines.value_or(24);
        node.xOffset = baseX;
        node.yOffset = baseY;
        node.paneId = (leafIndex < paneIds.size()) ? paneIds[leafIndex] : leafIndex;
        ++leafIndex;
        return node;
    }

    node.type = (layout.type == TmuxTestDSL::LayoutSpec::HSplit) ? TmuxLayoutNodeType::HSplit : TmuxLayoutNodeType::VSplit;
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

// Build tmux split commands to create the layout
// Returns list of (splitDirection, command) pairs where splitDirection is -h or -v
struct SplitStep {
    QString direction; // "-h" or "-v"
    QString cmd;
    QString targetPaneId; // tmux pane ID to split from (like "%0")
};

void buildSplitSteps(const TmuxTestDSL::LayoutSpec &layout,
                     const QString & /*parentPaneId*/,
                     QList<SplitStep> &steps,
                     int &nextLeafIndex,
                     int totalLeaves)
{
    Q_UNUSED(totalLeaves)
    if (layout.type == TmuxTestDSL::LayoutSpec::Leaf) {
        ++nextLeafIndex;
        return;
    }

    // For the first child, it uses the parent pane (already exists).
    // For subsequent children, we need to split.
    // The direction for HSplit (side-by-side) is -h, for VSplit (stacked) is -v.
    QString dir = (layout.type == TmuxTestDSL::LayoutSpec::HSplit) ? QStringLiteral("-h") : QStringLiteral("-v");

    for (int i = 0; i < layout.children.size(); ++i) {
        if (i == 0) {
            // First child uses the existing pane
            buildSplitSteps(layout.children[i], {}, steps, nextLeafIndex, totalLeaves);
        } else {
            // Record a split step (target will be resolved later)
            SplitStep step;
            step.direction = dir;
            // Get the command for the first leaf of this child
            QStringList cmds;
            collectPaneCommands(layout.children[i], cmds);
            step.cmd = cmds.isEmpty() ? QString() : cmds.first();
            steps.append(step);

            // Process remaining leaves of this child (they'll generate their own splits)
            buildSplitSteps(layout.children[i], {}, steps, nextLeafIndex, totalLeaves);
        }
    }
}

// Recursively walk the layout tree and splitter tree in parallel,
// collecting (TerminalDisplay*, PaneSpec) pairs for leaf nodes.
void collectDisplayPanePairs(const TmuxTestDSL::LayoutSpec &layout,
                             ViewSplitter *splitter,
                             QList<QPair<TerminalDisplay *, TmuxTestDSL::PaneSpec>> &pairs)
{
    if (layout.type == TmuxTestDSL::LayoutSpec::Leaf) {
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

        if (child.type == TmuxTestDSL::LayoutSpec::Leaf) {
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
bool verifySplitterStructure(const TmuxTestDSL::LayoutSpec &layout, ViewSplitter *splitter)
{
    if (layout.type == TmuxTestDSL::LayoutSpec::Leaf) {
        // A leaf should be a single TerminalDisplay (or a ViewSplitter with one child)
        return true;
    }

    if (!splitter) {
        return false;
    }

    // Check orientation
    Qt::Orientation expectedOrientation = (layout.type == TmuxTestDSL::LayoutSpec::HSplit) ? Qt::Horizontal : Qt::Vertical;
    if (splitter->orientation() != expectedOrientation) {
        return false;
    }

    // Check child count
    if (splitter->count() != layout.children.size()) {
        return false;
    }

    // Recursively check children
    for (int i = 0; i < layout.children.size(); ++i) {
        if (layout.children[i].type != TmuxTestDSL::LayoutSpec::Leaf) {
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
namespace TmuxTestDSL
{

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
        return qMakePair(layout.pane.columns.value_or(80), layout.pane.lines.value_or(24));
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

DiagramSpec parse(const QString &diagram)
{
    QStringList lines = dedentLines(diagram);

    // Find bounding box: locate ┌ (top-left) and ┘ (bottom-right)
    int topRow = -1, leftCol = -1;
    int bottomRow = -1, rightCol = -1;

    for (int row = 0; row < lines.size(); ++row) {
        for (int col = 0; col < lines[row].size(); ++col) {
            if (lines[row][col] == QChar(0x250C)) { // ┌
                if (topRow == -1) {
                    topRow = row;
                    leftCol = col;
                }
            }
            if (lines[row][col] == QChar(0x2518)) { // ┘
                bottomRow = row;
                rightCol = col;
            }
        }
    }

    DiagramSpec spec;

    if (topRow >= 0 && bottomRow >= 0) {
        spec.layout = parseRegion(lines, topRow, leftCol, bottomRow, rightCol);

        // Parse footer lines (after bottom border)
        QStringList footerLines;
        for (int row = bottomRow + 1; row < lines.size(); ++row) {
            footerLines.append(lines[row]);
        }
        parseFooter(footerLines, spec);
    }

    return spec;
}

void setupTmuxSession(const DiagramSpec &spec, const QString &tmuxPath, const QString &socketDir, SessionContext &ctx)
{
    static int sessionCounter = 0;
    ctx.sessionName = QStringLiteral("konsole-dsl-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(sessionCounter);
    ctx.socketPath = socketDir + QStringLiteral("/tmux-test-%1-%2").arg(QCoreApplication::applicationPid()).arg(sessionCounter++);

    // Collect all pane commands
    QStringList cmds;
    collectPaneCommands(spec.layout, cmds);
    QString firstCmd = cmds.isEmpty() ? QStringLiteral("sleep 30") : cmds.first();
    if (firstCmd.isEmpty()) {
        firstCmd = QStringLiteral("sleep 30");
    }

    // Build new-session arguments
    QStringList args = {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-session"), QStringLiteral("-d"), QStringLiteral("-s"), ctx.sessionName};

    auto windowSize = computeWindowSize(spec.layout);
    args << QStringLiteral("-x") << QString::number(windowSize.first);
    args << QStringLiteral("-y") << QString::number(windowSize.second);

    args << firstCmd;

    QProcess tmuxNewSession;
    tmuxNewSession.start(tmuxPath, args);
    QVERIFY(tmuxNewSession.waitForFinished(5000));
    QCOMPARE(tmuxNewSession.exitCode(), 0);

    // Now create splits according to the layout
    if (spec.layout.type != LayoutSpec::Leaf) {
        // We need to create additional panes by splitting
        // Simple approach: flatten the layout into a sequence of split operations
        // For each non-first leaf pane, split from the appropriate existing pane

        // We'll use a simpler recursive approach:
        // Process the layout tree and issue split-window commands
        struct PaneInfo {
            int tmuxPaneIndex; // 0-based pane index in the tmux window
            QString id;
        };

        QList<PaneInfo> createdPanes;
        createdPanes.append({0, cmds.isEmpty() ? QString() : QString()});

        // For the layout tree, we need to split in the right order.
        // The simplest correct approach: do it level by level.
        // Actually, tmux split-window always splits the target pane.
        // Let's use a recursive approach that tracks which tmux pane index
        // corresponds to which region of the layout.

        // Simpler: just issue splits in order.
        // For HSplit with N children: split pane 0 horizontally N-1 times
        // For VSplit with N children: split pane 0 vertically N-1 times
        // For nested: split the appropriate pane

        // Let's use a queue-based approach
        struct SplitTask {
            LayoutSpec layout;
            int tmuxPaneIndex;
        };

        QList<SplitTask> tasks;
        tasks.append({spec.layout, 0});
        int nextPaneIndex = 1;

        // Collect pane ID mapping
        QStringList paneIds;
        collectPaneIds(spec.layout, paneIds);

        // Track leaf pane indices for ID mapping
        QList<QPair<QString, int>> leafPanes; // (id, tmux pane index)

        // First leaf is always pane index 0
        if (spec.layout.type == LayoutSpec::Leaf) {
            if (!spec.layout.pane.id.isEmpty()) {
                leafPanes.append({spec.layout.pane.id, 0});
            }
        }

        while (!tasks.isEmpty()) {
            SplitTask task = tasks.takeFirst();

            if (task.layout.type == LayoutSpec::Leaf) {
                continue;
            }

            QString dir = (task.layout.type == LayoutSpec::HSplit) ? QStringLiteral("-h") : QStringLiteral("-v");

            // First child inherits the current pane index
            int firstChildPaneIndex = task.tmuxPaneIndex;
            if (task.layout.children[0].type == LayoutSpec::Leaf && !task.layout.children[0].pane.id.isEmpty()) {
                leafPanes.append({task.layout.children[0].pane.id, firstChildPaneIndex});
            }
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
                int targetPane = (i == 1) ? firstChildPaneIndex : (nextPaneIndex - 1);
                // Actually, for subsequent splits of the same parent, we should split from
                // the previously created pane to maintain proper ordering.
                // But for the first additional child, split from the parent pane.
                targetPane = firstChildPaneIndex;

                QProcess tmuxSplit;
                tmuxSplit.start(tmuxPath,
                                {QStringLiteral("-S"),
                                 ctx.socketPath,
                                 QStringLiteral("split-window"),
                                 dir,
                                 QStringLiteral("-t"),
                                 QStringLiteral("%1:%2.%3").arg(ctx.sessionName).arg(0).arg(targetPane),
                                 childCmd});
                QVERIFY2(tmuxSplit.waitForFinished(5000),
                         qPrintable(QStringLiteral("split-window timed out")));
                QCOMPARE(tmuxSplit.exitCode(), 0);

                int newPaneIndex = nextPaneIndex++;
                if (task.layout.children[i].type == LayoutSpec::Leaf && !task.layout.children[i].pane.id.isEmpty()) {
                    leafPanes.append({task.layout.children[i].pane.id, newPaneIndex});
                }
                tasks.append({task.layout.children[i], newPaneIndex});
            }
        }

        // Build ID to pane ID mapping by querying tmux for actual pane IDs
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
        QStringList paneLines = QString::fromUtf8(tmuxListPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));

        QMap<int, int> indexToId; // pane_index -> pane_id (numeric part of %N)
        for (const QString &line : paneLines) {
            QStringList parts = line.split(QLatin1Char(' '));
            if (parts.size() == 2) {
                int idx = parts[0].toInt();
                int id = parts[1].mid(1).toInt(); // strip % prefix
                indexToId[idx] = id;
            }
        }

        for (const auto &lp : leafPanes) {
            if (indexToId.contains(lp.second)) {
                ctx.idToPaneId[lp.first] = indexToId[lp.second];
            }
        }
    } else {
        // Single pane - query its ID
        if (!spec.layout.pane.id.isEmpty()) {
            QProcess tmuxListPanes;
            tmuxListPanes.start(tmuxPath,
                                {QStringLiteral("-S"),
                                 ctx.socketPath,
                                 QStringLiteral("list-panes"),
                                 QStringLiteral("-t"),
                                 ctx.sessionName,
                                 QStringLiteral("-F"),
                                 QStringLiteral("#{pane_id}")});
            QVERIFY(tmuxListPanes.waitForFinished(5000));
            QString paneId = QString::fromUtf8(tmuxListPanes.readAllStandardOutput()).trimmed();
            if (paneId.startsWith(QLatin1Char('%'))) {
                ctx.idToPaneId[spec.layout.pane.id] = paneId.mid(1).toInt();
            }
        }
    }

    // Set exact pane dimensions.
    // First try resize-pane for each pane (works for simple layouts).
    // If verification fails, fall back to select-layout (atomic, handles complex layouts).
    {
        QList<QPair<int, int>> expectedDims;
        collectPaneDimensions(spec.layout, expectedDims);

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

        int expectedPanes = countPanes(spec.layout);
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
            TmuxLayoutNode layoutNode = buildTmuxLayoutFromSpec(spec.layout, paneIds, leafIndex);
            QString layoutString = TmuxLayoutParser::serialize(layoutNode);

            auto windowSize = computeWindowSize(spec.layout);

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
        collectPaneDimensions(spec.layout, expectedDims);

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

        int expectedPanes = countPanes(spec.layout);
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

void applyKonsoleLayout(const DiagramSpec &spec, ViewManager *vm)
{
    auto *container = vm->activeContainer();
    QVERIFY(container);

    int expectedPanes = countPanes(spec.layout);
    ViewSplitter *paneSplitter = findPaneSplitter(container, expectedPanes);
    QVERIFY2(paneSplitter,
             qPrintable(QStringLiteral("Expected a ViewSplitter with %1 TerminalDisplay children").arg(expectedPanes)));

    // Get font metrics from the first TerminalDisplay
    auto *firstDisplay = paneSplitter->findChildren<TerminalDisplay *>().first();
    QVERIFY(firstDisplay);
    QVERIFY(firstDisplay->terminalFont()->fontWidth() > 0);
    QVERIFY(firstDisplay->terminalFont()->fontHeight() > 0);

    // Collect all (display, pane) pairs
    QList<QPair<TerminalDisplay *, PaneSpec>> pairs;
    if (spec.layout.type == LayoutSpec::Leaf) {
        pairs.append(qMakePair(firstDisplay, spec.layout.pane));
    } else {
        collectDisplayPanePairs(spec.layout, paneSplitter, pairs);
    }

    // Resize each display individually and send resize events.
    // This approach works even when the widget isn't shown (offscreen tests).
    for (const auto &pair : pairs) {
        auto *display = pair.first;
        int cols = pair.second.columns.value_or(80);
        int lns = pair.second.lines.value_or(24);
        QSize targetSize = displayPixelSize(display, cols, lns);
        QSize oldSize = display->size();
        display->resize(targetSize);
        QResizeEvent resizeEvent(targetSize, oldSize);
        QCoreApplication::sendEvent(display, &resizeEvent);
    }
    QCoreApplication::processEvents();

    // Handle focus
    for (const auto &pair : pairs) {
        if (pair.second.focused.has_value() && pair.second.focused.value()) {
            pair.first->setFocus();
        }
    }
}

void assertKonsoleLayout(const DiagramSpec &spec, ViewManager *vm)
{
    auto *container = vm->activeContainer();
    QVERIFY(container);

    // Find the pane tab (the one with a ViewSplitter containing TerminalDisplays, not the gateway)
    ViewSplitter *paneSplitter = nullptr;
    int expectedPanes = countPanes(spec.layout);

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

    QVERIFY2(paneSplitter,
             qPrintable(QStringLiteral("Expected a ViewSplitter with %1 TerminalDisplay children").arg(expectedPanes)));

    // Check orientation derived from layout type
    if (spec.layout.type != LayoutSpec::Leaf) {
        Qt::Orientation expected = (spec.layout.type == LayoutSpec::HSplit) ? Qt::Horizontal : Qt::Vertical;
        QCOMPARE(paneSplitter->orientation(), expected);
    }

    // Verify structure matches layout tree
    if (spec.layout.type != LayoutSpec::Leaf) {
        QVERIFY2(verifySplitterStructure(spec.layout, paneSplitter), "ViewSplitter tree structure does not match diagram");
    }

    // Collect (display, pane) pairs and verify dimensions and focus
    QList<QPair<TerminalDisplay *, PaneSpec>> pairs;
    if (spec.layout.type == LayoutSpec::Leaf) {
        // Single pane: the splitter should contain exactly one TerminalDisplay
        auto displays = paneSplitter->findChildren<TerminalDisplay *>();
        QVERIFY(!displays.isEmpty());
        pairs.append(qMakePair(displays.first(), spec.layout.pane));
    } else {
        collectDisplayPanePairs(spec.layout, paneSplitter, pairs);
    }

    // Verify dimensions for each leaf pane
    for (const auto &pair : pairs) {
        auto *display = pair.first;
        const auto &pane = pair.second;

        if (pane.columns.has_value()) {
            QVERIFY2(display->columns() == pane.columns.value(),
                     qPrintable(QStringLiteral("Display columns %1 != expected %2 (pane id: %3)")
                                    .arg(display->columns())
                                    .arg(pane.columns.value())
                                    .arg(pane.id)));
        }
        if (pane.lines.has_value()) {
            QVERIFY2(display->lines() == pane.lines.value(),
                     qPrintable(QStringLiteral("Display lines %1 != expected %2 (pane id: %3)")
                                    .arg(display->lines())
                                    .arg(pane.lines.value())
                                    .arg(pane.id)));
        }
    }

    // Verify focus
    for (const auto &pair : pairs) {
        if (pair.second.focused.has_value() && pair.second.focused.value()) {
            QVERIFY2(pair.first->hasFocus(),
                     qPrintable(QStringLiteral("Pane '%1' should have focus but doesn't").arg(pair.second.id)));
        }
    }

    // Check tab title if specified
    if (spec.tab.has_value()) {
        // Find the tab index for the pane splitter
        for (int i = 0; i < container->count(); ++i) {
            if (container->viewSplitterAt(i) == paneSplitter) {
                QString tabText = container->tabText(i);
                QVERIFY2(tabText.contains(spec.tab.value()),
                         qPrintable(QStringLiteral("Tab text '%1' does not contain '%2'").arg(tabText, spec.tab.value())));
                break;
            }
        }
    }
}

void assertTmuxLayout(const DiagramSpec &spec, const QString &tmuxPath, const SessionContext &ctx)
{
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

    QCOMPARE(paneLines.size(), countPanes(spec.layout));
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

} // namespace TmuxTestDSL
} // namespace Konsole
