/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxIntegrationTest.h"

#include <KActionCollection>
#include <KMessageBox>
#include <QPointer>
#include <optional>
#include <QCoreApplication>
#include <QProcess>
#include <QResizeEvent>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTabBar>
#include <QTest>

#include "../Emulation.h"
#include "../MainWindow.h"
#include "../Screen.h"
#include "../ScreenWindow.h"
#include "../ViewManager.h"
#include "../profile/ProfileManager.h"
#include "../session/Session.h"
#include "../session/SessionController.h"
#include "../session/SessionManager.h"
#include "../session/VirtualSession.h"
#include "../terminalDisplay/TerminalDisplay.h"
#include "../terminalDisplay/TerminalFonts.h"
#include "../tmux/TmuxController.h"
#include "../tmux/TmuxControllerRegistry.h"
#include "../tmux/TmuxLayoutManager.h"
#include "../tmux/TmuxLayoutParser.h"
#include "../tmux/TmuxPaneManager.h"
#include "../tmux/TmuxProcessBridge.h"
#include "../widgets/TabPageWidget.h"
#include "../widgets/ViewContainer.h"
#include "../widgets/ViewSplitter.h"

using namespace Konsole;

struct PaneSpec {
    QString id;
    QString cmd;
    QString title;
    QStringList contains;
    std::optional<bool> focused;
    std::optional<int> columns;
    std::optional<int> lines;
};

struct LayoutSpec {
    enum Type { Leaf, HSplit, VSplit };
    Type type = Leaf;
    PaneSpec pane; // Leaf only
    QList<LayoutSpec> children; // Split only
};

struct DiagramSpec {
    LayoutSpec layout;
    std::optional<QString> tab;
    std::optional<QList<int>> ratio;
};

struct SessionContext {
    QString sessionName;
    QString socketPath;
    QMap<QString, int> idToPaneId;
};

struct AttachResult {
    QPointer<MainWindow> mw;
    TmuxProcessBridge *bridge = nullptr;
    QPointer<TabbedViewContainer> container;
};

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
PaneSpec parseAnnotations(const QStringList &lines, int top, int left, int bottom, int right)
{
    PaneSpec pane;
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
LayoutSpec parseRegion(const QStringList &lines, int top, int left, int bottom, int right)
{
    // Scan top border for ┬ (U+252C) where bottom border has ┴ (U+2534) or ┼ (U+253C)
    // This indicates a vertical split (side-by-side panes = HSplit in our terminology,
    // but actually the box ┬ means a vertical divider between horizontally arranged panes)
    QList<int> vsplitCols;
    for (int col = left + 1; col < right; ++col) {
        QChar topChar = charAt(lines, top, col);
        QChar botChar = charAt(lines, bottom, col);
        if ((topChar == QChar(0x252C) || topChar == QChar(0x253C)) // ┬ or ┼
            && (botChar == QChar(0x2534) || botChar == QChar(0x253C))) { // ┴ or ┼
            // Verify the divider runs the full height
            bool fullDivider = true;
            for (int row = top + 1; row < bottom; ++row) {
                QChar ch = charAt(lines, row, col);
                if (ch != QChar(0x2502) && ch != QChar(0x253C) // │ or ┼
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
        LayoutSpec spec;
        spec.type = LayoutSpec::HSplit;

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
        if ((leftChar == QChar(0x251C) || leftChar == QChar(0x253C)) // ├ or ┼
            && (rightChar == QChar(0x2524) || rightChar == QChar(0x253C))) { // ┤ or ┼
            // Verify the divider runs the full width
            bool fullDivider = true;
            for (int col = left + 1; col < right; ++col) {
                QChar ch = charAt(lines, row, col);
                if (ch != QChar(0x2500) && ch != QChar(0x253C) // ─ or ┼
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
        LayoutSpec spec;
        spec.type = LayoutSpec::VSplit;

        int prevRow = top;
        for (int splitRow : hsplitRows) {
            spec.children.append(parseRegion(lines, prevRow, left, splitRow, right));
            prevRow = splitRow;
        }
        spec.children.append(parseRegion(lines, prevRow, left, bottom, right));
        return spec;
    }

    // Leaf pane: parse annotations from interior
    LayoutSpec spec;
    spec.type = LayoutSpec::Leaf;
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
void parseFooter(const QStringList &footerLines, DiagramSpec &spec)
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
void collectPaneDimensions(const LayoutSpec &layout, QList<QPair<int, int>> &dims)
{
    if (layout.type == LayoutSpec::Leaf) {
        dims.append(qMakePair(layout.pane.columns.value_or(80), layout.pane.lines.value_or(24)));
    } else {
        for (const auto &child : layout.children) {
            collectPaneDimensions(child, dims);
        }
    }
}

// Collect all pane IDs from a layout tree
void collectPaneIds(const LayoutSpec &layout, QStringList &ids)
{
    if (layout.type == LayoutSpec::Leaf) {
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
void collectPaneCommands(const LayoutSpec &layout, QStringList &cmds)
{
    if (layout.type == LayoutSpec::Leaf) {
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
TmuxLayoutNode buildTmuxLayoutFromSpec(const LayoutSpec &layout, const QList<int> &paneIds, int &leafIndex,
                                       int baseX = 0, int baseY = 0)
{
    TmuxLayoutNode node;

    if (layout.type == LayoutSpec::Leaf) {
        node.type = TmuxLayoutNodeType::Leaf;
        node.width = layout.pane.columns.value_or(80);
        node.height = layout.pane.lines.value_or(24);
        node.xOffset = baseX;
        node.yOffset = baseY;
        node.paneId = (leafIndex < paneIds.size()) ? paneIds[leafIndex] : leafIndex;
        ++leafIndex;
        return node;
    }

    node.type = (layout.type == LayoutSpec::HSplit) ? TmuxLayoutNodeType::HSplit : TmuxLayoutNodeType::VSplit;
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

void buildSplitSteps(const LayoutSpec &layout,
                     const QString & /*parentPaneId*/,
                     QList<SplitStep> &steps,
                     int &nextLeafIndex,
                     int totalLeaves)
{
    Q_UNUSED(totalLeaves)
    if (layout.type == LayoutSpec::Leaf) {
        ++nextLeafIndex;
        return;
    }

    // For the first child, it uses the parent pane (already exists).
    // For subsequent children, we need to split.
    // The direction for HSplit (side-by-side) is -h, for VSplit (stacked) is -v.
    QString dir = (layout.type == LayoutSpec::HSplit) ? QStringLiteral("-h") : QStringLiteral("-v");

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
void collectDisplayPanePairs(const LayoutSpec &layout,
                             ViewSplitter *splitter,
                             QList<QPair<TerminalDisplay *, PaneSpec>> &pairs)
{
    if (layout.type == LayoutSpec::Leaf) {
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

        if (child.type == LayoutSpec::Leaf) {
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
bool verifySplitterStructure(const LayoutSpec &layout, ViewSplitter *splitter)
{
    if (layout.type == LayoutSpec::Leaf) {
        // A leaf should be a single TerminalDisplay (or a ViewSplitter with one child)
        return true;
    }

    if (!splitter) {
        return false;
    }

    // Check orientation
    Qt::Orientation expectedOrientation = (layout.type == LayoutSpec::HSplit) ? Qt::Horizontal : Qt::Vertical;
    if (splitter->orientation() != expectedOrientation) {
        return false;
    }

    // Check child count
    if (splitter->count() != layout.children.size()) {
        return false;
    }

    // Recursively check children
    for (int i = 0; i < layout.children.size(); ++i) {
        if (layout.children[i].type != LayoutSpec::Leaf) {
            auto *childSplitter = qobject_cast<ViewSplitter *>(splitter->widget(i));
            if (!verifySplitterStructure(layout.children[i], childSplitter)) {
                return false;
            }
        }
    }

    return true;
}

} // anonymous namespace

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


void TmuxIntegrationTest::initTestCase()
{
    // Set the application name so isKonsolePart() and tab bar visibility
    // logic treat this test binary the same as the real kmux executable.
    QCoreApplication::setApplicationName(QStringLiteral("kmux"));
    QVERIFY(m_tmuxTmpDir.isValid());
}

void TmuxIntegrationTest::cleanupTestCase()
{
    // Each test uses its own -S socket, so there is no shared server to kill.
}

void TmuxIntegrationTest::testTmuxControlModeExitCleanup()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Use TmuxProcessBridge to start tmux -C new-session "sleep 1 && exit 0"
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    const QString socketPath = m_tmuxTmpDir.path() + QStringLiteral("/tmux-exit-cleanup");
    auto *bridge = new TmuxProcessBridge(vm, mw);
    QVERIFY(bridge->start(tmuxPath, {QStringLiteral("-S"), socketPath}, {QStringLiteral("new-session"), QStringLiteral("sleep 1 && exit 0")}));

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    // Wait for tmux control mode to create virtual pane tab(s)
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    // Wait for tmux to exit — the window may close and delete itself
    QTRY_VERIFY_WITH_TIMEOUT(!mwGuard || !container || container->count() == 0, 15000);

    // If the window is still alive, clean up
    delete mwGuard.data();
}

void TmuxIntegrationTest::testClosePaneTabThenGatewayTab()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Use TmuxProcessBridge to start tmux -C new-session "sleep 30"
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    const QString socketPath = m_tmuxTmpDir.path() + QStringLiteral("/tmux-close-pane");
    auto *bridge = new TmuxProcessBridge(vm, mw);
    QVERIFY(bridge->start(tmuxPath, {QStringLiteral("-S"), socketPath}, {QStringLiteral("new-session"), QStringLiteral("sleep 30")}));

    QPointer<TabbedViewContainer> container = vm->activeContainer();
    QVERIFY(container);

    // Wait for tmux control mode to create virtual pane tab(s)
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 1, 10000);

    // Find the pane session (all sessions are pane sessions with TmuxProcessBridge)
    Session *paneSession = nullptr;
    const auto sessions = vm->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // Close the pane tab (like clicking the tab close icon)
    paneSession->closeInNormalWay();

    // Wait for everything to tear down — the window may close and delete itself
    QTRY_VERIFY_WITH_TIMEOUT(!mwGuard, 10000);

    delete mwGuard.data();
}

void TmuxIntegrationTest::testTmuxControlModeAttach()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 30                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Close the pane tab, then destroy the bridge
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    paneSession->closeInNormalWay();

    // Wait for everything to tear down
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxTwoPaneSplitAttach()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 30                          │ cmd: sleep 30                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));

    applyKonsoleLayout(layoutSpec, attach.mw->viewManager());
    assertKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Clean up: close pane sessions, then destroy the bridge
    const auto sessions = attach.mw->viewManager()->sessions();
    for (Session *s : sessions) {
        s->closeInNormalWay();
    }

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

// Helper: read all visible text from a Session's screen
static QString readSessionScreenText(Session *session)
{
    ScreenWindow *window = session->emulation()->createWindow();
    Screen *screen = window->screen();

    int lines = screen->getLines();
    int columns = screen->getColumns();

    screen->setSelectionStart(0, 0, false);
    screen->setSelectionEnd(columns, lines - 1, false);
    return screen->selectedText(Screen::PlainText);
    // Don't delete window — Emulation::~Emulation owns it via _windows list
}

void TmuxIntegrationTest::testTmuxAttachContentRecovery()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌───────────────────────────────────┐
        │ cmd: bash --norc --noprofile      │
        │                                   │
        │                                   │
        │                                   │
        │                                   │
        └───────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Send a command with Unicode output
    QProcess sendKeys;
    sendKeys.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("send-keys"),
                    QStringLiteral("-t"),
                    ctx.sessionName,
                    QStringLiteral("echo 'MARKER_START ★ Unicode → Test ✓ MARKER_END'"),
                    QStringLiteral("Enter")});
    QVERIFY(sendKeys.waitForFinished(5000));
    QCOMPARE(sendKeys.exitCode(), 0);

    // Wait for the command to execute
    QTest::qWait(500);

    // Now attach Konsole via -CC
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // Wait a bit for capture-pane history to be injected
    QTest::qWait(2000);

    // Read the screen content
    QString screenText = readSessionScreenText(paneSession);

    QVERIFY2(screenText.contains(QStringLiteral("MARKER_START")),
             qPrintable(QStringLiteral("Pane screen should contain 'MARKER_START', got: ") + screenText));
    QVERIFY2(screenText.contains(QStringLiteral("MARKER_END")),
             qPrintable(QStringLiteral("Pane screen should contain 'MARKER_END', got: ") + screenText));

    // Cleanup: close pane sessions, then destroy the bridge
    const auto allSessions = attach.mw->viewManager()->sessions();
    for (Session *s : allSessions) {
        s->closeInNormalWay();
    }

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxAttachComplexPromptRecovery()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                                                                                                                                                                                   │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        │                                                                                                                                                                                                                                                │
        └────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Set a complex PS1 prompt with ANSI colors and Unicode
    QProcess sendPS1;
    sendPS1.start(tmuxPath,
                  {QStringLiteral("-S"),
                   ctx.socketPath,
                   QStringLiteral("send-keys"),
                   QStringLiteral("-t"),
                   ctx.sessionName,
                   QStringLiteral("PS1='\\[\\033[36m\\][\\t] [\\u@\\h \\w] "
                                  "\\[\\033[33m\\]"
                                  "────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────"
                                  "────────────────────────────── \\[\\033[35m\\]\\s \\V  \\[\\033[32m\\]→ \\[\\033[0m\\]'"),
                   QStringLiteral("Enter")});
    QVERIFY(sendPS1.waitForFinished(5000));
    QCOMPARE(sendPS1.exitCode(), 0);

    // Wait for prompt to render
    QTest::qWait(500);

    // Run a command so we have some content
    QProcess sendCmd;
    sendCmd.start(tmuxPath,
                  {QStringLiteral("-S"),
                   ctx.socketPath,
                   QStringLiteral("send-keys"),
                   QStringLiteral("-t"),
                   ctx.sessionName,
                   QStringLiteral("echo 'PROMPT_TEST_OUTPUT'"),
                   QStringLiteral("Enter")});
    QVERIFY(sendCmd.waitForFinished(5000));
    QCOMPARE(sendCmd.exitCode(), 0);

    // Wait for command to execute
    QTest::qWait(500);

    // Now attach Konsole via -CC
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // Wait for capture-pane history to be injected
    QTest::qWait(2000);

    // Read the screen content
    QString screenText = readSessionScreenText(paneSession);

    QVERIFY2(screenText.contains(QStringLiteral("PROMPT_TEST_OUTPUT")),
             qPrintable(QStringLiteral("Pane screen should contain 'PROMPT_TEST_OUTPUT', got: ") + screenText));

    QVERIFY2(screenText.contains(QStringLiteral("→")),
             qPrintable(QStringLiteral("Pane screen should contain '→' from prompt, got: ") + screenText));

    QVERIFY2(screenText.contains(QStringLiteral("────")),
             qPrintable(QStringLiteral("Pane screen should contain '────' from prompt, got: ") + screenText));

    // Cleanup: close pane sessions, then destroy the bridge
    const auto allSessions = attach.mw->viewManager()->sessions();
    for (Session *s : allSessions) {
        s->closeInNormalWay();
    }

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitterResizePropagatedToTmux()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Query initial pane sizes
    QProcess tmuxListPanes;
    tmuxListPanes.start(tmuxPath,
                        {QStringLiteral("-S"),
                         ctx.socketPath,
                         QStringLiteral("list-panes"),
                         QStringLiteral("-t"),
                         ctx.sessionName,
                         QStringLiteral("-F"),
                         QStringLiteral("#{pane_width}")});
    QVERIFY(tmuxListPanes.waitForFinished(5000));
    QCOMPARE(tmuxListPanes.exitCode(), 0);
    QStringList initialWidths = QString::fromUtf8(tmuxListPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
    QCOMPARE(initialWidths.size(), 2);
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Apply the initial layout to set Konsole widget sizes to match the diagram
    auto initialLayout = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    applyKonsoleLayout(initialLayout, attach.mw->viewManager());

    // Find the split pane splitter
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QCOMPARE(paneSplitter->orientation(), Qt::Horizontal);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    // Read current splitter sizes and display dimensions
    QList<int> sizes = paneSplitter->sizes();
    QCOMPARE(sizes.size(), 2);
    // Move the splitter: make left pane significantly larger (3/4 vs 1/4).
    int total = sizes[0] + sizes[1];
    int newLeft = total * 3 / 4;
    int newRight = total - newLeft;
    paneSplitter->setSizes({newLeft, newRight});

    // Force display widgets to the new pixel sizes and send resize events
    int displayHeight = leftDisplay->height();
    leftDisplay->resize(newLeft, displayHeight);
    rightDisplay->resize(newRight, displayHeight);
    QResizeEvent leftResizeEvent(QSize(newLeft, displayHeight), leftDisplay->size());
    QResizeEvent rightResizeEvent(QSize(newRight, displayHeight), rightDisplay->size());
    QCoreApplication::sendEvent(leftDisplay, &leftResizeEvent);
    QCoreApplication::sendEvent(rightDisplay, &rightResizeEvent);
    QCoreApplication::processEvents();

    // Verify the resize actually produced different column counts
    QVERIFY2(leftDisplay->columns() != rightDisplay->columns(),
             qPrintable(QStringLiteral("Expected different column counts but both are %1").arg(leftDisplay->columns())));

    // Trigger splitterMoved signal (setSizes doesn't emit it automatically)
    Q_EMIT paneSplitter->splitterMoved(newLeft, 1);

    // Read expected sizes from terminal displays (what buildLayoutNode will use)
    int expectedLeftWidth = leftDisplay->columns();
    int expectedRightWidth = rightDisplay->columns();
    int expectedLeftHeight = leftDisplay->lines();
    int expectedRightHeight = rightDisplay->lines();
    int expectedWindowWidth = expectedLeftWidth + 1 + expectedRightWidth; // +1 for separator
    int expectedWindowHeight = qMax(expectedLeftHeight, expectedRightHeight);
    // Wait for the command to propagate to tmux and verify exact sizes
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_width} #{pane_height}")});
        check.waitForFinished(3000);
        QStringList paneLines = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (paneLines.size() != 2) return false;
        QStringList pane0 = paneLines[0].split(QLatin1Char(' '));
        QStringList pane1 = paneLines[1].split(QLatin1Char(' '));
        if (pane0.size() != 2 || pane1.size() != 2) return false;
        int w0 = pane0[0].toInt();
        int h0 = pane0[1].toInt();
        int w1 = pane1[0].toInt();
        int h1 = pane1[1].toInt();
        return w0 == expectedLeftWidth && w1 == expectedRightWidth
            && h0 == expectedWindowHeight && h1 == expectedWindowHeight;
    }(), 10000);

    // Also verify tmux window size matches
    {
        QProcess checkWindow;
        checkWindow.start(tmuxPath,
                          {QStringLiteral("-S"),
                           ctx.socketPath,
                           QStringLiteral("list-windows"),
                           QStringLiteral("-t"),
                           ctx.sessionName,
                           QStringLiteral("-F"),
                           QStringLiteral("#{window_width} #{window_height}")});
        QVERIFY(checkWindow.waitForFinished(3000));
        QStringList windowSize = QString::fromUtf8(checkWindow.readAllStandardOutput()).trimmed().split(QLatin1Char(' '));
        QCOMPARE(windowSize.size(), 2);
        int windowWidth = windowSize[0].toInt();
        int windowHeight = windowSize[1].toInt();
        QCOMPARE(windowWidth, expectedWindowWidth);
        QCOMPARE(windowHeight, expectedWindowHeight);
    }

    // Wait for any pending layout-change callbacks to finish
    QTest::qWait(500);

    // Kill the tmux session first to avoid layout-change during teardown
    // (cleanup guard handles this, but we want it early)
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxPaneTitleInfo()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌───────────────────────────────────┐
        │ cmd: bash --norc --noprofile      │
        │                                   │
        │                                   │
        │                                   │
        │                                   │
        └───────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // cd to /tmp so we have a known directory
    QProcess sendCd;
    sendCd.start(tmuxPath,
                 {QStringLiteral("-S"),
                  ctx.socketPath,
                  QStringLiteral("send-keys"),
                  QStringLiteral("-t"),
                  ctx.sessionName,
                  QStringLiteral("cd /tmp"),
                  QStringLiteral("Enter")});
    QVERIFY(sendCd.waitForFinished(5000));
    QCOMPARE(sendCd.exitCode(), 0);
    QTest::qWait(500);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *virtualSession = qobject_cast<VirtualSession *>(paneSession);
    QVERIFY(virtualSession);

    // Wait for pane title info to be queried
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QString title = paneSession->getDynamicTitle();
        return title.contains(QStringLiteral("tmp")) || title.contains(QStringLiteral("bash"));
    }(), 10000);

    // Verify that the tab title for the tmux window is set from #{window_name}
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    int windowId = controller->windowIdForPane(paneId);
    QVERIFY(windowId >= 0);
    int tabIndex = controller->windowToTabIndex().value(windowId, -1);
    QVERIFY(tabIndex >= 0);
    QString tabText = attach.container->tabText(tabIndex);
    QVERIFY2(!tabText.isEmpty(), "Tab text should not be empty for tmux window");

    // Cleanup
    const auto allSessions = attach.mw->viewManager()->sessions();
    for (Session *s : allSessions) {
        s->closeInNormalWay();
    }

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testWindowNameWithSpaces()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Rename the window to something adversarial: spaces, hex-like tokens, commas, braces
    QString evilName = QStringLiteral("htop lol abc0,80x24,0,0 {evil} [nasty]");
    QProcess renameProc;
    renameProc.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("rename-window"), QStringLiteral("-t"), ctx.sessionName, evilName});
    QVERIFY(renameProc.waitForFinished(5000));
    QCOMPARE(renameProc.exitCode(), 0);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY2(!sessions.isEmpty(), "Expected a tmux pane session to be created despite spaces in window name");
    paneSession = sessions.first();

    // Verify the tab title matches the evil name
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    int windowId = controller->windowIdForPane(paneId);
    QVERIFY(windowId >= 0);
    int tabIndex = controller->windowToTabIndex().value(windowId, -1);
    QVERIFY(tabIndex >= 0);
    QString tabText = attach.container->tabText(tabIndex);
    QCOMPARE(tabText, evilName);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitPaneFocusesNewPane()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session and its controller (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    QVERIFY(paneId >= 0);

    // Record the original pane's display
    auto originalDisplays = paneSession->views();
    QVERIFY(!originalDisplays.isEmpty());
    auto *originalDisplay = originalDisplays.first();

    // Show and activate the window so setFocus() works
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    // Request a horizontal split from within Konsole
    controller->requestSplitPane(paneId, Qt::Horizontal);

    // Wait for the split to appear: a ViewSplitter with 2 TerminalDisplay children
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find the new pane's display (the one that isn't the original)
    auto terminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(terminals.size(), 2);
    TerminalDisplay *newDisplay = nullptr;
    for (auto *td : terminals) {
        if (td != originalDisplay) {
            newDisplay = td;
            break;
        }
    }
    QVERIFY2(newDisplay, "Expected to find a new TerminalDisplay after split");

    // The new pane should have focus
    QTRY_VERIFY_WITH_TIMEOUT(newDisplay->hasFocus(), 5000);

    // Kill the tmux session first to avoid layout-change during teardown
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitPaneFocusesNewPaneComplexLayout()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Create 3 horizontal panes, select pane 0, then split it vertically from Konsole
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        │                                        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Select the first pane so we know which one is active before attaching
    QProcess tmuxSelect;
    tmuxSelect.start(tmuxPath,
                     {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("select-pane"), QStringLiteral("-t"), ctx.sessionName + QStringLiteral(":0.0")});
    QVERIFY(tmuxSelect.waitForFinished(5000));
    QCOMPARE(tmuxSelect.exitCode(), 0);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for all 3 panes to appear
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 3) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find the pane sessions (all sessions are pane sessions)
    QList<Session *> paneSessions = attach.mw->viewManager()->sessions();
    QVERIFY(paneSessions.size() >= 3);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSessions.first());
    QVERIFY(controller);

    int firstPaneId = controller->paneIdForSession(paneSessions.first());
    QVERIFY(firstPaneId >= 0);

    // Record all existing displays before the split
    auto existingTerminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(existingTerminals.size(), 3);

    // Show and activate the window so setFocus() works
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    // Request a vertical split on the first pane
    controller->requestSplitPane(firstPaneId, Qt::Vertical);

    // Wait for 4 panes to appear
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 4) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find the new display (the one not in existingTerminals)
    auto allTerminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(allTerminals.size(), 4);
    TerminalDisplay *newDisplay = nullptr;
    for (auto *td : allTerminals) {
        if (!existingTerminals.contains(td)) {
            newDisplay = td;
            break;
        }
    }
    QVERIFY2(newDisplay, "Expected to find a new TerminalDisplay after split");

    // The new pane should have focus
    QTRY_VERIFY_WITH_TIMEOUT(newDisplay->hasFocus(), 5000);

    // Kill the tmux session first to avoid layout-change during teardown
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitPaneFocusesNewPaneNestedLayout()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Create nested layout: [ pane0 | [ pane1 / pane2 ] ]
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        ├────────────────────────────────────────┤
        │                                        │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Select the first pane (pane0) so that's what we'll split from Konsole
    QProcess tmuxSelect;
    tmuxSelect.start(tmuxPath,
                     {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("select-pane"), QStringLiteral("-t"), ctx.sessionName + QStringLiteral(":0.0")});
    QVERIFY(tmuxSelect.waitForFinished(5000));
    QCOMPARE(tmuxSelect.exitCode(), 0);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for all 3 panes to appear
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 3) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find pane0's session (all sessions are pane sessions)
    QVERIFY(!attach.mw->viewManager()->sessions().isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    // Find pane0: query tmux for pane IDs to find the first one
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
    QCOMPARE(tmuxListPanes.exitCode(), 0);
    QStringList paneIdStrs = QString::fromUtf8(tmuxListPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
    QVERIFY(paneIdStrs.size() >= 3);
    // Pane IDs look like %42 — strip the % prefix
    int firstPaneId = paneIdStrs[0].mid(1).toInt();

    // Record all existing displays
    auto existingTerminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(existingTerminals.size(), 3);

    // Show and activate the window
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    // Split pane0 vertically from Konsole
    controller->requestSplitPane(firstPaneId, Qt::Vertical);

    // Wait for 4 panes to appear
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        paneSplitter = nullptr;
        auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
        if (!container)
            return false;
        for (int i = 0; i < container->count(); ++i) {
            auto *splitter = container->viewSplitterAt(i);
            if (splitter) {
                auto terminals = splitter->findChildren<TerminalDisplay *>();
                if (terminals.size() == 4) {
                    paneSplitter = splitter;
                    return true;
                }
            }
        }
        return false;
    }(), 10000);
    QVERIFY(paneSplitter);

    // Find the new display
    auto allTerminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(allTerminals.size(), 4);
    TerminalDisplay *newDisplay = nullptr;
    for (auto *td : allTerminals) {
        if (!existingTerminals.contains(td)) {
            newDisplay = td;
            break;
        }
    }
    QVERIFY2(newDisplay, "Expected to find a new TerminalDisplay after split");

    // The new pane should have focus
    QTRY_VERIFY_WITH_TIMEOUT(newDisplay->hasFocus(), 5000);

    // Kill the tmux session first
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testResizePropagatedToPty()
{
    const QString tmuxPath = findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // 1. Setup tmux session with a two-pane horizontal split running bash
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: bash                              │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);

    // 2. Attach Konsole
    auto initialLayout = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: bash                              │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);
    applyKonsoleLayout(initialLayout, attach.mw->viewManager());

    // Find the two-pane splitter
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QCOMPARE(paneSplitter->orientation(), Qt::Horizontal);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    // 3. Resize the splitter: make left pane significantly larger (3/4 vs 1/4)
    QList<int> sizes = paneSplitter->sizes();
    int total = sizes[0] + sizes[1];
    int newLeft = total * 3 / 4;
    int newRight = total - newLeft;
    paneSplitter->setSizes({newLeft, newRight});

    // Force display widgets to the new pixel sizes and send resize events
    int displayHeight = leftDisplay->height();
    leftDisplay->resize(newLeft, displayHeight);
    rightDisplay->resize(newRight, displayHeight);
    QResizeEvent leftResizeEvent(QSize(newLeft, displayHeight), leftDisplay->size());
    QResizeEvent rightResizeEvent(QSize(newRight, displayHeight), rightDisplay->size());
    QCoreApplication::sendEvent(leftDisplay, &leftResizeEvent);
    QCoreApplication::sendEvent(rightDisplay, &rightResizeEvent);
    QCoreApplication::processEvents();

    // Trigger splitterMoved signal (setSizes doesn't emit it automatically)
    Q_EMIT paneSplitter->splitterMoved(newLeft, 1);

    int expectedLeftCols = leftDisplay->columns();
    int expectedRightCols = rightDisplay->columns();

    // Verify the resize actually produced different column counts
    QVERIFY2(expectedLeftCols != expectedRightCols,
             qPrintable(QStringLiteral("Expected different column counts but both are %1").arg(expectedLeftCols)));

    // 4. Wait for tmux to process the layout change (metadata)
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_width}")});
        check.waitForFinished(3000);
        QStringList paneWidths = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (paneWidths.size() != 2) return false;
        return paneWidths[0].toInt() == expectedLeftCols && paneWidths[1].toInt() == expectedRightCols;
    }(), 10000);

    // 5. Run 'stty size' in each pane and verify PTY dimensions match.
    // tmux defers TIOCSWINSZ (PTY resize) through its server loop, so we
    // poll: send 'stty size', capture output, and re-send if needed.
    int expectedLeftLines = leftDisplay->lines();
    int expectedRightLines = rightDisplay->lines();
    auto runSttyAndCheck = [&](const QString &paneTarget, int expectedLines, int expectedCols) -> bool {
        // Send stty size
        QProcess sendKeys;
        sendKeys.start(tmuxPath,
                       {QStringLiteral("-S"),
                        ctx.socketPath,
                        QStringLiteral("send-keys"),
                        QStringLiteral("-t"),
                        paneTarget,
                        QStringLiteral("-l"),
                        QStringLiteral("stty size\n")});
        if (!sendKeys.waitForFinished(3000)) return false;
        QTest::qWait(300);

        // Capture and check
        QProcess capture;
        capture.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-t"), paneTarget, QStringLiteral("-p")});
        capture.waitForFinished(3000);
        QString output = QString::fromUtf8(capture.readAllStandardOutput());
        QString expected = QString::number(expectedLines) + QStringLiteral(" ") + QString::number(expectedCols);
        return output.contains(expected);
    };

    QTRY_VERIFY_WITH_TIMEOUT(
        runSttyAndCheck(ctx.sessionName + QStringLiteral(":0.0"), expectedLeftLines, expectedLeftCols),
        10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        runSttyAndCheck(ctx.sessionName + QStringLiteral(":0.1"), expectedRightLines, expectedRightCols),
        10000);

    // Wait for any pending callbacks
    QTest::qWait(500);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNestedResizePropagatedToPty()
{
    const QString tmuxPath = findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // 1. Setup tmux session with a nested layout: left pane | [top-right / bottom-right]
    //    All panes run bash so we can check stty size.
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: bash                              │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        ├────────────────────────────────────────┤
        │                                        │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);

    // 2. Attach Konsole and apply the same layout
    auto initialLayout = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: bash                              │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        ├────────────────────────────────────────┤
        │                                        │ cmd: bash                              │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);
    applyKonsoleLayout(initialLayout, attach.mw->viewManager());

    // 3. Find the top-level splitter (horizontal: left | right-sub-splitter)
    ViewSplitter *topSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 3) {
                topSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(topSplitter, "Expected a ViewSplitter with 3 TerminalDisplay descendants");
    QCOMPARE(topSplitter->orientation(), Qt::Horizontal);
    QCOMPARE(topSplitter->count(), 2);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(topSplitter->widget(0));
    QVERIFY(leftDisplay);

    // The right child should be a nested vertical splitter
    auto *rightSplitter = qobject_cast<ViewSplitter *>(topSplitter->widget(1));
    QVERIFY2(rightSplitter, "Expected right child to be a ViewSplitter");
    QCOMPARE(rightSplitter->orientation(), Qt::Vertical);
    QCOMPARE(rightSplitter->count(), 2);

    auto *topRightDisplay = qobject_cast<TerminalDisplay *>(rightSplitter->widget(0));
    auto *bottomRightDisplay = qobject_cast<TerminalDisplay *>(rightSplitter->widget(1));
    QVERIFY(topRightDisplay);
    QVERIFY(bottomRightDisplay);

    // 4. Resize the NESTED (vertical) splitter: make top-right much larger
    QList<int> sizes = rightSplitter->sizes();
    int total = sizes[0] + sizes[1];
    int newTop = total * 3 / 4;
    int newBottom = total - newTop;
    rightSplitter->setSizes({newTop, newBottom});

    // Force display widgets to the new pixel sizes and send resize events
    int displayWidth = topRightDisplay->width();
    topRightDisplay->resize(displayWidth, newTop);
    bottomRightDisplay->resize(displayWidth, newBottom);
    QResizeEvent topResizeEvent(QSize(displayWidth, newTop), topRightDisplay->size());
    QResizeEvent bottomResizeEvent(QSize(displayWidth, newBottom), bottomRightDisplay->size());
    QCoreApplication::sendEvent(topRightDisplay, &topResizeEvent);
    QCoreApplication::sendEvent(bottomRightDisplay, &bottomResizeEvent);
    QCoreApplication::processEvents();

    // Trigger splitterMoved signal on the nested splitter
    Q_EMIT rightSplitter->splitterMoved(newTop, 1);

    int expectedTopRightLines = topRightDisplay->lines();
    int expectedBottomRightLines = bottomRightDisplay->lines();
    int expectedTopRightCols = topRightDisplay->columns();
    int expectedBottomRightCols = bottomRightDisplay->columns();
    // Verify the resize actually produced different line counts
    QVERIFY2(expectedTopRightLines != expectedBottomRightLines,
             qPrintable(QStringLiteral("Expected different line counts but both are %1").arg(expectedTopRightLines)));

    // 5. Wait for tmux to process the layout change (metadata)
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_height}")});
        check.waitForFinished(3000);
        QStringList paneHeights = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (paneHeights.size() != 3) return false;
        // Pane order: %0 (left), %1 (top-right), %2 (bottom-right)
        return paneHeights[1].toInt() == expectedTopRightLines && paneHeights[2].toInt() == expectedBottomRightLines;
    }(), 10000);

    // 6. Run 'stty size' in each nested pane and verify PTY dimensions match
    auto runSttyAndCheck = [&](const QString &paneTarget, int expectedLines, int expectedCols) -> bool {
        QProcess sendKeys;
        sendKeys.start(tmuxPath,
                       {QStringLiteral("-S"),
                        ctx.socketPath,
                        QStringLiteral("send-keys"),
                        QStringLiteral("-t"),
                        paneTarget,
                        QStringLiteral("-l"),
                        QStringLiteral("stty size\n")});
        if (!sendKeys.waitForFinished(3000)) return false;
        QTest::qWait(300);

        QProcess capture;
        capture.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-t"), paneTarget, QStringLiteral("-p")});
        capture.waitForFinished(3000);
        QString output = QString::fromUtf8(capture.readAllStandardOutput());
        QString expected = QString::number(expectedLines) + QStringLiteral(" ") + QString::number(expectedCols);
        return output.contains(expected);
    };

    // Check top-right pane (pane index 1)
    QTRY_VERIFY_WITH_TIMEOUT(
        runSttyAndCheck(ctx.sessionName + QStringLiteral(":0.1"), expectedTopRightLines, expectedTopRightCols),
        10000);
    // Check bottom-right pane (pane index 2)
    QTRY_VERIFY_WITH_TIMEOUT(
        runSttyAndCheck(ctx.sessionName + QStringLiteral(":0.2"), expectedBottomRightLines, expectedBottomRightCols),
        10000);

    // Wait for any pending callbacks
    QTest::qWait(500);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}


void TmuxIntegrationTest::testTopLevelResizeWithNestedChild()
{
    const QString tmuxPath = findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Minimal 4-pane layout: left | center | [top-right / bottom-right]
    // 3-child top-level HSplit where the rightmost child is a nested VSplit.
    // Resizing the handle between center and the right column must propagate
    // correct absolute offsets and cross-axis dimensions to tmux.
    SessionContext ctx;
    auto diagram = parse(QStringLiteral(R"(
        ┌──────────────────────────┬──────────────────────────┬──────────────────────────┐
        │ cmd: bash                │ cmd: bash                │ cmd: bash                │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          ├──────────────────────────┤
        │                          │                          │ cmd: bash                │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          │                          │
        └──────────────────────────┴──────────────────────────┴──────────────────────────┘
    )"));
    setupTmuxSession(diagram, tmuxPath, m_tmuxTmpDir.path(), ctx);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);
    applyKonsoleLayout(diagram, attach.mw->viewManager());

    // Find the splitter with 4 displays
    ViewSplitter *topSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 4) {
                topSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(topSplitter, "Expected a ViewSplitter with 4 TerminalDisplay descendants");
    QCOMPARE(topSplitter->orientation(), Qt::Horizontal);
    QCOMPARE(topSplitter->count(), 3);

    // Record initial tmux pane widths
    QProcess initialCheck;
    initialCheck.start(tmuxPath,
                       {QStringLiteral("-S"),
                        ctx.socketPath,
                        QStringLiteral("list-panes"),
                        QStringLiteral("-t"),
                        ctx.sessionName,
                        QStringLiteral("-F"),
                        QStringLiteral("#{pane_id} #{pane_width} #{pane_height}")});
    initialCheck.waitForFinished(3000);
    QString initialPanesStr = QString::fromUtf8(initialCheck.readAllStandardOutput()).trimmed();

    // Parse initial widths per pane ID
    QMap<QString, int> initialWidths;
    for (const auto &line : initialPanesStr.split(QLatin1Char('\n'))) {
        QStringList parts = line.split(QLatin1Char(' '));
        if (parts.size() == 3) {
            initialWidths[parts[0]] = parts[1].toInt();
        }
    }

    // Resize: shift space from right column to center
    QList<int> sizes = topSplitter->sizes();
    QCOMPARE(sizes.size(), 3);
    int shift = sizes[2] / 3;
    sizes[1] += shift;
    sizes[2] -= shift;
    topSplitter->setSizes(sizes);

    // Force resize events on all displays
    auto allDisplays = topSplitter->findChildren<TerminalDisplay *>();
    for (auto *d : allDisplays) {
        QResizeEvent ev(d->size(), d->size());
        QCoreApplication::sendEvent(d, &ev);
    }
    QCoreApplication::processEvents();

    Q_EMIT topSplitter->splitterMoved(sizes[0] + sizes[1], 2);

    // The key assertion: after the splitter drag, tmux pane widths should change.
    // With the bug (wrong offsets/cross-axis), tmux rejects or ignores the layout.
    // Wait for tmux to accept the new layout and verify widths changed.
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id} #{pane_width} #{pane_height}")});
        check.waitForFinished(3000);
        QStringList panes = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (panes.size() != 4) return false;

        // Check that at least one pane's width changed from initial
        bool anyChanged = false;
        for (const auto &line : panes) {
            QStringList parts = line.split(QLatin1Char(' '));
            if (parts.size() != 3) return false;
            QString paneId = parts[0];
            int width = parts[1].toInt();
            if (initialWidths.contains(paneId) && width != initialWidths[paneId]) {
                anyChanged = true;
            }
        }
        return anyChanged;
    }(), 10000);

    // Now verify the dimensions match the layout we sent.
    // Query tmux for the window layout string and verify it parses correctly.
    QProcess layoutCheck;
    layoutCheck.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctx.socketPath,
                       QStringLiteral("display-message"),
                       QStringLiteral("-t"),
                       ctx.sessionName,
                       QStringLiteral("-p"),
                       QStringLiteral("#{window_layout}")});
    layoutCheck.waitForFinished(3000);
    QString tmuxLayout = QString::fromUtf8(layoutCheck.readAllStandardOutput()).trimmed();
    QVERIFY2(!tmuxLayout.isEmpty(), "tmux should report a valid window layout");

    QTest::qWait(500);
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNestedResizeSurvivesFocusCycle()
{
    const QString tmuxPath = findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    const QString scriptPath = QStandardPaths::findExecutable(QStringLiteral("script"));
    if (scriptPath.isEmpty()) {
        QSKIP("script command not found.");
    }

    // 4-pane nested layout: left | center | [top-right / bottom-right]
    // Resize, then cycle through smaller-client attach/detach,
    // verify the resized layout is preserved after recovery.
    SessionContext ctx;
    auto diagram = parse(QStringLiteral(R"(
        ┌──────────────────────────┬──────────────────────────┬──────────────────────────┐
        │ cmd: bash                │ cmd: bash                │ cmd: bash                │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          ├──────────────────────────┤
        │                          │                          │ cmd: bash                │
        │                          │                          │                          │
        │                          │                          │                          │
        │                          │                          │                          │
        └──────────────────────────┴──────────────────────────┴──────────────────────────┘
    )"));
    setupTmuxSession(diagram, tmuxPath, m_tmuxTmpDir.path(), ctx);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);
    applyKonsoleLayout(diagram, attach.mw->viewManager());

    // Find the splitter with 4 displays
    ViewSplitter *topSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 4) {
                topSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(topSplitter, "Expected a ViewSplitter with 4 TerminalDisplay descendants");
    QCOMPARE(topSplitter->orientation(), Qt::Horizontal);
    QCOMPARE(topSplitter->count(), 3);

    // 1. Resize: shift space from right column to center
    QList<int> sizes = topSplitter->sizes();
    QCOMPARE(sizes.size(), 3);
    int shift = sizes[2] / 3;
    sizes[1] += shift;
    sizes[2] -= shift;
    topSplitter->setSizes(sizes);

    auto allDisplays = topSplitter->findChildren<TerminalDisplay *>();
    for (auto *d : allDisplays) {
        QResizeEvent ev(d->size(), d->size());
        QCoreApplication::sendEvent(d, &ev);
    }
    QCoreApplication::processEvents();

    Q_EMIT topSplitter->splitterMoved(sizes[0] + sizes[1], 2);

    // Wait for tmux to accept the resized layout
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_width}")});
        check.waitForFinished(3000);
        QStringList widths = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (widths.size() != 4) return false;
        // Initially all panes were 26 wide; after resize at least one should differ
        for (const auto &w : widths) {
            if (w.toInt() != 26) return true;
        }
        return false;
    }(), 10000);

    // Record the post-resize layout from tmux
    QProcess layoutCheck;
    layoutCheck.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctx.socketPath,
                       QStringLiteral("display-message"),
                       QStringLiteral("-t"),
                       ctx.sessionName,
                       QStringLiteral("-p"),
                       QStringLiteral("#{window_layout}")});
    layoutCheck.waitForFinished(3000);
    QString postResizeLayout = QString::fromUtf8(layoutCheck.readAllStandardOutput()).trimmed();
    QVERIFY(!postResizeLayout.isEmpty());

    // Record post-resize pane dimensions
    QProcess dimsCheck;
    dimsCheck.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id} #{pane_width} #{pane_height}")});
    dimsCheck.waitForFinished(3000);
    QString postResizeDims = QString::fromUtf8(dimsCheck.readAllStandardOutput()).trimmed();

    // 2. Attach a smaller client to constrain the layout
    QProcess scriptProc;
    scriptProc.start(
        scriptPath,
        {
            QStringLiteral("-q"),
            QStringLiteral("-c"),
            QStringLiteral("stty cols 40 rows 12; ") + tmuxPath + QStringLiteral(" -S ") + ctx.socketPath + QStringLiteral(" attach -t ") + ctx.sessionName,
            QStringLiteral("/dev/null"),
        });
    QVERIFY(scriptProc.waitForStarted(5000));

    // Wait for the smaller client to be visible
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-clients"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{client_width}x#{client_height}")});
        check.waitForFinished(3000);
        QStringList clients = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return clients.size() >= 2;
    }(), 10000);

    // Wait for layout to shrink
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{window_layout}")});
        check.waitForFinished(3000);
        QString layout = QString::fromUtf8(check.readAllStandardOutput()).trimmed();
        return layout != postResizeLayout;
    }(), 10000);

    QProcess constrainedCheck;
    constrainedCheck.start(tmuxPath,
                           {QStringLiteral("-S"),
                            ctx.socketPath,
                            QStringLiteral("display-message"),
                            QStringLiteral("-t"),
                            ctx.sessionName,
                            QStringLiteral("-p"),
                            QStringLiteral("#{window_layout}")});
    constrainedCheck.waitForFinished(3000);
    QString constrainedLayout = QString::fromUtf8(constrainedCheck.readAllStandardOutput()).trimmed();

    // 3. Kill the smaller client — layout should recover
    scriptProc.terminate();
    scriptProc.waitForFinished(5000);
    if (scriptProc.state() != QProcess::NotRunning) {
        scriptProc.kill();
        scriptProc.waitForFinished(3000);
    }

    // Wait for only one client to remain
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-clients"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{client_name}")});
        check.waitForFinished(3000);
        QStringList clients = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return clients.size() == 1;
    }(), 10000);

    // Process events so Konsole reacts to %client-detached → refreshClientCount
    QTest::qWait(500);
    QCoreApplication::processEvents();

    // Simulate Konsole regaining focus: in offscreen mode isActiveWindow() is
    // always false, so constraints are never cleared automatically.  Manually
    // clear constraints on the TabPageWidget and emit focusChanged to trigger
    // sendClientSize, mimicking what happens when the user clicks the window.
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *page = attach.container->tabPageAt(i);
        if (page && page->isConstrained()) {
            page->clearConstrainedSize();
        }
    }
    Q_EMIT qApp->focusChanged(nullptr, nullptr);
    QTest::qWait(200);
    QCoreApplication::processEvents();

    // Now do the resize again on the recovered layout.
    // The widget sizes may differ from the initial run (offscreen doesn't
    // resize widgets back to original proportions), but the point is that
    // buildLayoutNode produces a valid layout string and tmux accepts it.
    topSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 4) {
                topSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(topSplitter, "Expected splitter with 4 displays after focus cycle");

    sizes = topSplitter->sizes();
    QCOMPARE(sizes.size(), 3);
    shift = sizes[2] / 3;
    sizes[1] += shift;
    sizes[2] -= shift;
    topSplitter->setSizes(sizes);

    allDisplays = topSplitter->findChildren<TerminalDisplay *>();
    for (auto *d : allDisplays) {
        QResizeEvent ev(d->size(), d->size());
        QCoreApplication::sendEvent(d, &ev);
    }
    QCoreApplication::processEvents();

    Q_EMIT topSplitter->splitterMoved(sizes[0] + sizes[1], 2);

    // 4. Verify tmux accepts the post-focus-cycle resize.
    // The constrained layout shrank pane widths; after recovery and re-resize,
    // at least one pane should have a width different from the constrained state.
    QProcess constrainedDimsCheck;
    constrainedDimsCheck.start(tmuxPath,
                               {QStringLiteral("-S"),
                                ctx.socketPath,
                                QStringLiteral("list-panes"),
                                QStringLiteral("-t"),
                                ctx.sessionName,
                                QStringLiteral("-F"),
                                QStringLiteral("#{pane_id} #{pane_width}")});
    constrainedDimsCheck.waitForFinished(3000);
    QString constrainedDimsStr = QString::fromUtf8(constrainedDimsCheck.readAllStandardOutput()).trimmed();

    // Parse constrained widths
    QMap<QString, int> constrainedWidths;
    for (const auto &line : constrainedDimsStr.split(QLatin1Char('\n'))) {
        QStringList parts = line.split(QLatin1Char(' '));
        if (parts.size() == 2) {
            constrainedWidths[parts[0]] = parts[1].toInt();
        }
    }

    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id} #{pane_width} #{pane_height}")});
        check.waitForFinished(3000);
        QStringList panes = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'));
        if (panes.size() != 4) return false;

        bool anyChanged = false;
        for (const auto &line : panes) {
            QStringList parts = line.split(QLatin1Char(' '));
            if (parts.size() != 3) return false;
            QString paneId = parts[0];
            int width = parts[1].toInt();
            if (constrainedWidths.contains(paneId) && width != constrainedWidths[paneId]) {
                anyChanged = true;
            }
        }
        return anyChanged;
    }(), 15000);

    // Verify the layout string is valid and accepted by tmux
    QProcess recoveredCheck;
    recoveredCheck.start(tmuxPath,
                         {QStringLiteral("-S"),
                          ctx.socketPath,
                          QStringLiteral("display-message"),
                          QStringLiteral("-t"),
                          ctx.sessionName,
                          QStringLiteral("-p"),
                          QStringLiteral("#{window_layout}")});
    recoveredCheck.waitForFinished(3000);
    QString recoveredLayout = QString::fromUtf8(recoveredCheck.readAllStandardOutput()).trimmed();
    QVERIFY2(recoveredLayout != constrainedLayout, "Layout should differ from constrained state after focus recovery");

    QTest::qWait(500);
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testForcedSizeFromSmallerClient()
{
    const QString tmuxPath = findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    const QString scriptPath = QStandardPaths::findExecutable(QStringLiteral("script"));
    if (scriptPath.isEmpty()) {
        QSKIP("script command not found.");
    }

    // 1. Setup tmux session with a single pane at 80x24
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // 2. Attach Konsole via control mode
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // 3. Apply large layout so widgets are sized generously
    auto layoutSpec = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )"));
    applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // 4. Find the pane display and verify initial state (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto paneViews = paneSession->views();
    QVERIFY(!paneViews.isEmpty());
    auto *display = qobject_cast<TerminalDisplay *>(paneViews.first());
    QVERIFY(display);

    int initialColumns = display->columns();
    int initialLines = display->lines();
    QVERIFY2(initialColumns >= 40, qPrintable(QStringLiteral("Expected initial columns >= 40 but got %1").arg(initialColumns)));
    QVERIFY2(initialLines >= 12, qPrintable(QStringLiteral("Expected initial lines >= 12 but got %1").arg(initialLines)));

    // Record the widget pixel size before the smaller client attaches
    QSize originalPixelSize = display->size();
    // 5. Attach a second smaller tmux client using script to provide a pty
    QProcess scriptProc;
    scriptProc.start(
        scriptPath,
        {
            QStringLiteral("-q"),
            QStringLiteral("-c"),
            QStringLiteral("stty cols 40 rows 12; ") + tmuxPath + QStringLiteral(" -S ") + ctx.socketPath + QStringLiteral(" attach -t ") + ctx.sessionName,
            QStringLiteral("/dev/null"),
        });
    QVERIFY(scriptProc.waitForStarted(5000));

    // Wait for the second client to actually appear in tmux
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-clients"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{client_width}x#{client_height}")});
        check.waitForFinished(3000);
        QStringList clients = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return clients.size() >= 2;
    }(), 10000);

    // 6. Wait for %layout-change to propagate — poll display->columns() until it shrinks
    QTRY_VERIFY_WITH_TIMEOUT(display->columns() < initialColumns, 15000);

    // 7. Assert grid size matches the smaller client (40x12 minus status bar)
    QVERIFY2(display->columns() <= 40,
             qPrintable(QStringLiteral("Expected columns <= 40 but got %1").arg(display->columns())));
    QVERIFY2(display->lines() <= 12,
             qPrintable(QStringLiteral("Expected lines <= 12 but got %1").arg(display->lines())));

    // 8. Assert the TabPageWidget is constrained (the whole layout moves to top-left)
    auto *topSplitter = qobject_cast<ViewSplitter *>(display->parentWidget());
    QVERIFY(topSplitter);
    while (auto *parentSplitter = qobject_cast<ViewSplitter *>(topSplitter->parentWidget())) {
        topSplitter = parentSplitter;
    }
    auto *page = qobject_cast<TabPageWidget *>(topSplitter->parentWidget());
    QVERIFY2(page, "Expected top-level splitter to be inside a TabPageWidget");
    QVERIFY2(page->isConstrained(), "Expected TabPageWidget to be constrained");
    QSize constrained = page->constrainedSize();
    QVERIFY2(constrained.width() < originalPixelSize.width()
                 || constrained.height() < originalPixelSize.height(),
             qPrintable(QStringLiteral("Expected constrained size smaller than %1x%2, got %3x%4")
                            .arg(originalPixelSize.width()).arg(originalPixelSize.height())
                            .arg(constrained.width()).arg(constrained.height())));

    // 9. Cleanup: kill the background script process
    scriptProc.terminate();
    scriptProc.waitForFinished(5000);
    if (scriptProc.state() != QProcess::NotRunning) {
        scriptProc.kill();
        scriptProc.waitForFinished(3000);
    }

    // Kill tmux session early to avoid layout-change during teardown
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testForcedSizeFromSmallerClientMultiPane()
{
    const QString tmuxPath = findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    const QString scriptPath = QStandardPaths::findExecutable(QStringLiteral("script"));
    if (scriptPath.isEmpty()) {
        QSKIP("script command not found.");
    }

    // 1. Setup tmux session with two horizontal panes (40+1+39 = 80 wide, 24 tall)
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬───────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                         │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        └────────────────────────────────────────┴───────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // 2. Attach Konsole via control mode
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // 3. Apply large layout so widgets are sized generously
    auto layoutSpec = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬───────────────────────────────────────┐
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        │                                        │                                       │
        └────────────────────────────────────────┴───────────────────────────────────────┘
    )"));
    applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // 4. Find the splitter with 2 TerminalDisplay children
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    int initialLeftCols = leftDisplay->columns();
    int initialRightCols = rightDisplay->columns();
    QSize originalLeftPixelSize = leftDisplay->size();
    QSize originalRightPixelSize = rightDisplay->size();

    QVERIFY2(initialLeftCols >= 20, qPrintable(QStringLiteral("Expected left columns >= 20 but got %1").arg(initialLeftCols)));
    QVERIFY2(initialRightCols >= 20, qPrintable(QStringLiteral("Expected right columns >= 20 but got %1").arg(initialRightCols)));

    // 5. Attach a second smaller tmux client using script to provide a pty
    QProcess scriptProc;
    scriptProc.start(
        scriptPath,
        {
            QStringLiteral("-q"),
            QStringLiteral("-c"),
            QStringLiteral("stty cols 40 rows 12; ") + tmuxPath + QStringLiteral(" -S ") + ctx.socketPath + QStringLiteral(" attach -t ") + ctx.sessionName,
            QStringLiteral("/dev/null"),
        });
    QVERIFY(scriptProc.waitForStarted(5000));

    // Wait for the second client to actually appear in tmux
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-clients"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{client_width}x#{client_height}")});
        check.waitForFinished(3000);
        QStringList clients = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return clients.size() >= 2;
    }(), 10000);

    // 6. Wait for %layout-change to propagate — both panes should shrink
    QTRY_VERIFY_WITH_TIMEOUT(leftDisplay->columns() < initialLeftCols || rightDisplay->columns() < initialRightCols, 15000);

    // 7. Assert forced grid sizes are smaller — total width should be <= 40
    int totalCols = leftDisplay->columns() + 1 + rightDisplay->columns(); // +1 for separator
    QVERIFY2(totalCols <= 40,
             qPrintable(QStringLiteral("Expected total columns <= 40 but got %1 (%2 + 1 + %3)")
                            .arg(totalCols).arg(leftDisplay->columns()).arg(rightDisplay->columns())));
    QVERIFY2(leftDisplay->lines() <= 12,
             qPrintable(QStringLiteral("Expected left lines <= 12 but got %1").arg(leftDisplay->lines())));
    QVERIFY2(rightDisplay->lines() <= 12,
             qPrintable(QStringLiteral("Expected right lines <= 12 but got %1").arg(rightDisplay->lines())));

    // 8. Assert the TabPageWidget is constrained (the whole layout moves to top-left)
    auto *page = qobject_cast<TabPageWidget *>(paneSplitter->parentWidget());
    QVERIFY2(page, "Expected splitter to be inside a TabPageWidget");
    QVERIFY2(page->isConstrained(), "Expected TabPageWidget to be constrained");
    QSize constrained = page->constrainedSize();
    QVERIFY2(constrained.width() < originalLeftPixelSize.width() + originalRightPixelSize.width()
                 || constrained.height() < originalLeftPixelSize.height(),
             qPrintable(QStringLiteral("Expected constrained size to shrink, got %1x%2")
                            .arg(constrained.width()).arg(constrained.height())));

    // 9. Cleanup: kill the background script process
    scriptProc.terminate();
    scriptProc.waitForFinished(5000);
    if (scriptProc.state() != QProcess::NotRunning) {
        scriptProc.kill();
        scriptProc.waitForFinished(3000);
    }

    // Kill tmux session early to avoid layout-change during teardown
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testClearScrollbackSyncToTmux()
{
    const QString tmuxPath = findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // 1. Setup tmux session with a single pane running bash
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌───────────────────────────────────┐
        │ cmd: bash --norc --noprofile      │
        │                                   │
        │                                   │
        │                                   │
        │                                   │
        └───────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // 2. Generate scrollback content
    QProcess sendKeys;
    sendKeys.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("send-keys"),
                    QStringLiteral("-t"),
                    ctx.sessionName,
                    QStringLiteral("for i in $(seq 1 200); do echo \"SCROLLBACK_LINE_$i\"; done"),
                    QStringLiteral("Enter")});
    QVERIFY(sendKeys.waitForFinished(5000));
    QCOMPARE(sendKeys.exitCode(), 0);

    QTest::qWait(500);

    // 3. Check tmux server-side scrollback size
    auto getTmuxHistorySize = [&]() -> int {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{history_size}")});
        check.waitForFinished(3000);
        return QString::fromUtf8(check.readAllStandardOutput()).trimmed().toInt();
    };

    QVERIFY2(getTmuxHistorySize() > 0, "Expected tmux history_size > 0 before attach");

    // 4. Attach Konsole
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    QTest::qWait(2000);

    QVERIFY2(getTmuxHistorySize() > 0, "Expected history_size > 0 after attach");

    // 5. requestClearHistory clears scrollback only, visible content remains
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);

    controller->requestClearHistory(paneSession);

    QTRY_VERIFY_WITH_TIMEOUT(getTmuxHistorySize() == 0, 5000);

    // Visible content should still show recent output
    auto captureTmuxPane = [&]() -> QString {
        QProcess capture;
        capture.start(tmuxPath,
                      {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("-p")});
        capture.waitForFinished(3000);
        return QString::fromUtf8(capture.readAllStandardOutput());
    };

    QString visible = captureTmuxPane();
    QVERIFY2(visible.contains(QStringLiteral("SCROLLBACK_LINE_200")),
             qPrintable(QStringLiteral("Expected visible pane to still contain recent output, got: ") + visible));

    // Cleanup
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testClearScrollbackAndResetSyncToTmux()
{
    const QString tmuxPath = findTmuxOrSkip();
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // 1. Setup tmux session with a single pane running bash
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌───────────────────────────────────┐
        │ cmd: bash --norc --noprofile      │
        │                                   │
        │                                   │
        │                                   │
        │                                   │
        └───────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // 2. Generate scrollback content
    QProcess sendKeys;
    sendKeys.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("send-keys"),
                    QStringLiteral("-t"),
                    ctx.sessionName,
                    QStringLiteral("for i in $(seq 1 200); do echo \"SCROLLBACK_LINE_$i\"; done"),
                    QStringLiteral("Enter")});
    QVERIFY(sendKeys.waitForFinished(5000));
    QCOMPARE(sendKeys.exitCode(), 0);

    QTest::qWait(500);

    auto getTmuxHistorySize = [&]() -> int {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{history_size}")});
        check.waitForFinished(3000);
        return QString::fromUtf8(check.readAllStandardOutput()).trimmed().toInt();
    };

    auto captureTmuxPane = [&]() -> QString {
        QProcess capture;
        capture.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctx.socketPath,
                       QStringLiteral("capture-pane"),
                       QStringLiteral("-t"),
                       ctx.sessionName,
                       QStringLiteral("-p"),
                       QStringLiteral("-S"),
                       QStringLiteral("-")});
        capture.waitForFinished(3000);
        return QString::fromUtf8(capture.readAllStandardOutput());
    };

    QVERIFY2(getTmuxHistorySize() > 0, "Expected tmux history_size > 0 before attach");

    // 3. Attach Konsole
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    QTest::qWait(2000);

    QVERIFY2(getTmuxHistorySize() > 0, "Expected history_size > 0 after attach");

    // 4. requestClearHistoryAndReset clears visible screen AND scrollback
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);

    controller->requestClearHistoryAndReset(paneSession);

    // Wait for both commands to take effect
    QTRY_VERIFY_WITH_TIMEOUT(getTmuxHistorySize() == 0, 5000);

    // Visible content should no longer contain the output lines
    QString allContent = captureTmuxPane();
    QVERIFY2(!allContent.contains(QStringLiteral("SCROLLBACK_LINE_")),
             qPrintable(QStringLiteral("Expected all SCROLLBACK_LINE content to be cleared, got: ") + allContent));

    // Cleanup
    killTmuxSession(tmuxPath, ctx);

    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxZoomFromKonsole()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Setup 2-pane tmux session
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Find the splitter with 2 displays
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QVERIFY(!paneSplitter->terminalMaximized());

    // Find a pane session and its controller (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    QVERIFY(paneId >= 0);

    // Trigger zoom via requestToggleZoomPane (simulates Konsole's maximize action)
    controller->requestToggleZoomPane(paneId);

    // Wait for tmux to report zoomed state
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{window_zoomed_flag}")});
        check.waitForFinished(3000);
        return QString::fromUtf8(check.readAllStandardOutput()).trimmed() == QStringLiteral("1");
    }(), 10000);

    // Verify Konsole splitter is maximized
    QTRY_VERIFY_WITH_TIMEOUT(paneSplitter->terminalMaximized(), 5000);

    // Trigger unzoom
    controller->requestToggleZoomPane(paneId);

    // Wait for tmux to report unzoomed state
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("display-message"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-p"),
                     QStringLiteral("#{window_zoomed_flag}")});
        check.waitForFinished(3000);
        return QString::fromUtf8(check.readAllStandardOutput()).trimmed() == QStringLiteral("0");
    }(), 10000);

    // Verify Konsole splitter is no longer maximized
    QTRY_VERIFY_WITH_TIMEOUT(!paneSplitter->terminalMaximized(), 5000);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxZoomFromTmux()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Setup 2-pane tmux session
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Find the splitter with 2 displays
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");
    QVERIFY(!paneSplitter->terminalMaximized());

    // Zoom from tmux externally
    QProcess zoomProc;
    zoomProc.start(tmuxPath,
                   {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("resize-pane"), QStringLiteral("-Z"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(zoomProc.waitForFinished(5000));
    QCOMPARE(zoomProc.exitCode(), 0);

    // Wait for Konsole to show maximized state
    QTRY_VERIFY_WITH_TIMEOUT(paneSplitter->terminalMaximized(), 10000);

    // Unzoom from tmux
    QProcess unzoomProc;
    unzoomProc.start(tmuxPath,
                     {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("resize-pane"), QStringLiteral("-Z"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(unzoomProc.waitForFinished(5000));
    QCOMPARE(unzoomProc.exitCode(), 0);

    // Wait for Konsole to restore all panes
    QTRY_VERIFY_WITH_TIMEOUT(!paneSplitter->terminalMaximized(), 10000);

    // Re-find the splitter (layout apply may have replaced it)
    paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children after unzoom");

    // Verify both displays are not explicitly hidden (isHidden() checks the widget's
    // own visibility flag, unlike isVisible() which also checks all ancestors).
    // In the offscreen test the pane tab may not be the active tab, so isVisible()
    // can return false even though the displays are not hidden.
    auto terminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(terminals.size(), 2);
    for (auto *td : terminals) {
        QVERIFY2(!td->isHidden(), "Expected both terminal displays to not be hidden after unzoom");
    }

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxZoomSurvivesLayoutChanges()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Small 2-pane layout — each pane is only ~20 columns wide, so the zoomed
    // display should clearly expand beyond that when maximized.
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │ cmd: sleep 60      │ cmd: sleep 60      │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │                    │                    │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )"));
    applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Find the splitter with 2 displays
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");

    // Find a pane session and record its pre-zoom display width (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto paneViews = paneSession->views();
    QVERIFY(!paneViews.isEmpty());
    auto *zoomedDisplay = qobject_cast<TerminalDisplay *>(paneViews.first());
    QVERIFY(zoomedDisplay);

    int preZoomColumns = zoomedDisplay->columns();

    // Zoom from tmux
    QProcess zoomProc;
    zoomProc.start(tmuxPath,
                   {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("resize-pane"), QStringLiteral("-Z"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(zoomProc.waitForFinished(5000));
    QCOMPARE(zoomProc.exitCode(), 0);

    // Wait for Konsole to enter maximized state
    QTRY_VERIFY_WITH_TIMEOUT(paneSplitter->terminalMaximized(), 10000);

    // Record the zoomed display's grid size right after maximize is applied
    int zoomedColumns = zoomedDisplay->columns();
    int zoomedLines = zoomedDisplay->lines();

    // Wait for several %layout-change notifications to arrive (the title refresh
    // timer fires every 2 seconds and can trigger layout-change echo-backs).
    QTest::qWait(5000);
    QCoreApplication::processEvents();

    // The key assertion: the zoomed display's grid size must not have been
    // shrunk by setForcedSize from a layout-change while zoomed.
    QVERIFY2(paneSplitter->terminalMaximized(), "Expected splitter to still be maximized after layout changes");
    QVERIFY2(zoomedDisplay->columns() == zoomedColumns,
             qPrintable(QStringLiteral("Expected zoomed columns to remain %1 but got %2 (pre-zoom was %3)")
                            .arg(zoomedColumns).arg(zoomedDisplay->columns()).arg(preZoomColumns)));
    QVERIFY2(zoomedDisplay->lines() == zoomedLines,
             qPrintable(QStringLiteral("Expected zoomed lines to remain %1 but got %2")
                            .arg(zoomedLines).arg(zoomedDisplay->lines())));

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testBreakPane()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Setup 2-pane tmux session
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto layoutSpec = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )"));
    applyKonsoleLayout(layoutSpec, attach.mw->viewManager());

    // Find the splitter with 2 displays
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < attach.container->count(); ++i) {
        auto *splitter = attach.container->viewSplitterAt(i);
        if (splitter) {
            auto terminals = splitter->findChildren<TerminalDisplay *>();
            if (terminals.size() == 2) {
                paneSplitter = splitter;
                break;
            }
        }
    }
    QVERIFY2(paneSplitter, "Expected a ViewSplitter with 2 TerminalDisplay children");

    int initialTabCount = attach.mw->viewManager()->activeContainer()->count();

    // Find a pane session and its controller (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    QVERIFY(paneId >= 0);

    // Break the pane out into a new tmux window
    controller->requestBreakPane(paneId);

    // Wait for tab count to increase (new tmux window → new tab)
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == initialTabCount + 1;
        }(),
        10000);

    // Verify the controller now has 2 windows, each with 1 pane
    QCOMPARE(controller->windowCount(), 2);
    const auto &windowTabs = controller->windowToTabIndex();
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        QCOMPARE(controller->paneCountForWindow(it.key()), 1);
        auto *splitter = attach.container->viewSplitterAt(it.value());
        QVERIFY(splitter);
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
    }

    // Verify tmux confirms 2 windows exist
    QProcess listWindows;
    listWindows.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("list-windows"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(listWindows.waitForFinished(5000));
    QString windowOutput = QString::fromUtf8(listWindows.readAllStandardOutput()).trimmed();
    int windowCount = windowOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
    QCOMPARE(windowCount, 2);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSplitPaneInheritsWorkingDirectory()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                   │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // cd to /tmp so we have a known directory
    QProcess sendCd;
    sendCd.start(tmuxPath,
                 {QStringLiteral("-S"),
                  ctx.socketPath,
                  QStringLiteral("send-keys"),
                  QStringLiteral("-t"),
                  ctx.sessionName,
                  QStringLiteral("cd /tmp"),
                  QStringLiteral("Enter")});
    QVERIFY(sendCd.waitForFinished(5000));
    QCOMPARE(sendCd.exitCode(), 0);
    QTest::qWait(500);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session and its controller (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(paneSession);
    QVERIFY(controller);
    int paneId = controller->paneIdForSession(paneSession);
    QVERIFY(paneId >= 0);

    // Wait for the working directory to propagate to the VirtualSession
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QString dir = paneSession->currentWorkingDirectory();
        return dir.contains(QStringLiteral("tmp"));
    }(), 10000);

    // Request a horizontal split, passing the working directory
    controller->requestSplitPane(paneId, Qt::Horizontal, QStringLiteral("/tmp"));

    // Wait for the split to appear: a ViewSplitter with 2 TerminalDisplay children
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!container)
                return false;
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter) {
                    auto terminals = splitter->findChildren<TerminalDisplay *>();
                    if (terminals.size() == 2) {
                        return true;
                    }
                }
            }
            return false;
        }(),
        10000);

    // Verify the new pane started in /tmp by querying tmux
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_current_path}")});
        check.waitForFinished(3000);
        QStringList paths = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (paths.size() != 2) return false;
        // Both the original pane and the new pane should be in /tmp
        return paths[0].contains(QStringLiteral("tmp")) && paths[1].contains(QStringLiteral("tmp"));
    }(), 10000);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNewWindowInheritsWorkingDirectory()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                   │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // cd to /tmp so we have a known directory
    QProcess sendCd;
    sendCd.start(tmuxPath,
                 {QStringLiteral("-S"),
                  ctx.socketPath,
                  QStringLiteral("send-keys"),
                  QStringLiteral("-t"),
                  ctx.sessionName,
                  QStringLiteral("cd /tmp"),
                  QStringLiteral("Enter")});
    QVERIFY(sendCd.waitForFinished(5000));
    QCOMPARE(sendCd.exitCode(), 0);
    QTest::qWait(500);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);

    int initialTabCount = attach.mw->viewManager()->activeContainer()->count();

    // Request a new tmux window with /tmp as working directory
    controller->requestNewWindow(QStringLiteral("/tmp"));

    // Wait for the new tab to appear
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == initialTabCount + 1;
        }(),
        10000);

    // Verify the new window's pane started in /tmp by querying tmux
    QTRY_VERIFY_WITH_TIMEOUT([&]() {
        QProcess check;
        check.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-a"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_current_path}")});
        check.waitForFinished(3000);
        QStringList paths = QString::fromUtf8(check.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (paths.size() != 2) return false;
        // The new window's pane should be in /tmp
        return paths[1].contains(QStringLiteral("tmp"));
    }(), 10000);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testOscColorQueryNotLeakedAsKeystrokes()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                   │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // Spy on data sent back from the emulation (this becomes send-keys in tmux mode)
    QSignalSpy sendSpy(paneSession->emulation(), &Emulation::sendData);

    // Send an OSC 10 foreground color query into the pane from the tmux side.
    // This simulates what happens when a program like bat sends "\033]10;?\007"
    // — tmux forwards the pane output as %output to Konsole's emulation.
    QProcess sendQuery;
    sendQuery.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("send-keys"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-l"),
                     QStringLiteral("printf '\\033]10;?\\007'")});
    QVERIFY(sendQuery.waitForFinished(5000));
    QCOMPARE(sendQuery.exitCode(), 0);
    // Execute the printf command
    QProcess sendEnter;
    sendEnter.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("send-keys"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("Enter")});
    QVERIFY(sendEnter.waitForFinished(5000));
    QCOMPARE(sendEnter.exitCode(), 0);

    // Wait for the output to propagate through tmux %output → Konsole emulation
    QTest::qWait(3000);

    // Check if any response containing "rgb:" was sent back via sendData.
    // This is the bug: the OSC color response should NOT be sent back as
    // keystrokes to the tmux pane, because it will appear as visible text.
    bool leaked = false;
    for (const auto &call : sendSpy) {
        QByteArray data = call.at(0).toByteArray();
        if (data.contains("rgb:")) {
            leaked = true;
            break;
        }
    }
    QVERIFY2(!leaked, "OSC color query response was leaked back as keystrokes to tmux pane");

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testCyrillicInputPreservesUtf8()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: bash --norc --noprofile                                                   │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the pane session (all sessions are pane sessions)
    Session *paneSession = nullptr;
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    paneSession = sessions.first();
    QVERIFY(paneSession);

    // TODO: adapt for TmuxProcessBridge — no emulation to spy on for the gateway.
    // With TmuxProcessBridge, tmux commands are sent via QProcess stdin, not
    // through a gateway Session's emulation. We verify the end-to-end result
    // by checking what tmux actually received instead.

    // Simulate typing Cyrillic text into the pane.
    // This goes through: sendText → Vt102Emulation → sendData signal →
    //   TmuxPaneManager lambda → TmuxGateway::sendKeys → QProcess stdin
    const QString cyrillicText = QStringLiteral("слоп");
    paneSession->emulation()->sendText(cyrillicText);

    // Let the event loop process and tmux receive the command
    QTest::qWait(1000);

    // Verify end-to-end: capture the tmux pane and check that the Cyrillic text
    // was received correctly (not garbled by hex encoding).
    QProcess capture;
    capture.start(tmuxPath,
                  {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("capture-pane"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("-p")});
    QVERIFY(capture.waitForFinished(5000));
    QString paneContent = QString::fromUtf8(capture.readAllStandardOutput());

    // The pane should contain the Cyrillic text as-is (it appears on the command line)
    bool containsLiteralCyrillic = paneContent.contains(cyrillicText);

    // Also check for the broken hex encoding pattern appearing in the pane.
    // If broken, we'd see something like: 0xd1 0x81 0xd0 0xbb ...
    bool containsHexEncoded = paneContent.contains(QStringLiteral("0xd0"));

    QVERIFY2(containsLiteralCyrillic,
             qPrintable(QStringLiteral("Cyrillic text should be sent as literal UTF-8 via send-keys -l, "
                                       "but the pane contains: %1")
                            .arg(paneContent)));
    QVERIFY2(!containsHexEncoded,
             qPrintable(QStringLiteral("Cyrillic bytes should NOT be hex-encoded as individual bytes, "
                                       "but the pane contains: %1")
                            .arg(paneContent)));

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testTmuxAttachNoSessions()
{
    const QString tmuxPath = QStandardPaths::findExecutable(QStringLiteral("tmux"));
    if (tmuxPath.isEmpty()) {
        QSKIP("tmux command not found.");
    }

    // Use TmuxProcessBridge to attempt "tmux -C attach" when there are no
    // tmux sessions. The bridge should handle the immediate exit gracefully
    // without crashing or leaking commands.
    auto *mw = new MainWindow();
    QPointer<MainWindow> mwGuard(mw);
    ViewManager *vm = mw->viewManager();

    // Use a fresh socket so there is guaranteed no server running on it
    const QString socketPath = m_tmuxTmpDir.path() + QStringLiteral("/tmux-no-sessions");

    auto *bridge = new TmuxProcessBridge(vm, mw);
    QSignalSpy disconnectedSpy(bridge, &TmuxProcessBridge::disconnected);

    // Start with "attach" which should fail immediately (no sessions)
    bridge->start(tmuxPath, {QStringLiteral("-S"), socketPath}, {QStringLiteral("attach")});

    // Wait for the bridge to report disconnection (tmux exits with error)
    QTRY_VERIFY_WITH_TIMEOUT(disconnectedSpy.count() >= 1, 10000);

    // The ViewManager should have no sessions (no pane tabs created)
    QVERIFY2(vm->sessions().isEmpty(), "No pane sessions should be created when tmux attach fails");

    delete mwGuard.data();
}

void TmuxIntegrationTest::testAttachMultipleWindows()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a tmux session with 1 window, then add a second window via tmux command
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Create a second tmux window before attaching
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    // Attach Konsole
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for both tmux windows to appear as tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    // Verify the controller sees 2 windows
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // Each window should have 1 pane
    const auto &windowTabs = controller->windowToTabIndex();
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        QCOMPARE(controller->paneCountForWindow(it.key()), 1);
    }

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNewWindowCreatesTab()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    int initialTabCount = container->count();

    // Get the controller
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);

    // Request a new window via the controller
    controller->requestNewWindow(QString());

    // Wait for the new tab to appear
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == initialTabCount + 1;
        }(),
        10000);

    QCOMPARE(controller->windowCount(), 2);

    // Verify tmux also has 2 windows
    QProcess listWindows;
    listWindows.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("list-windows"), QStringLiteral("-t"), ctx.sessionName});
    QVERIFY(listWindows.waitForFinished(5000));
    QString windowOutput = QString::fromUtf8(listWindows.readAllStandardOutput()).trimmed();
    int windowCount = windowOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
    QCOMPARE(windowCount, 2);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testCloseWindowFromTmuxRemovesTab()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a session, then add a second window
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    // Attach
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    // Kill the second tmux window from outside
    QProcess killWindow;
    killWindow.start(tmuxPath,
                     {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("kill-window"), QStringLiteral("-t"), QStringLiteral("%1:1").arg(ctx.sessionName)});
    QVERIFY(killWindow.waitForFinished(5000));
    QCOMPARE(killWindow.exitCode(), 0);

    // Wait for the tab to be removed
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == 1;
        }(),
        10000);

    // Verify the controller sees 1 window
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 1);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testCloseWindowTabFromKonsole()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a session with 1 window, then add a second
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    // Attach
    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    // Find a pane session from the second window and close it
    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(sessions.size() >= 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);

    // Find a session that belongs to a different window than the first session
    int firstPaneId = controller->paneIdForSession(sessions.first());
    int firstWindowId = controller->windowIdForPane(firstPaneId);
    Session *secondWindowSession = nullptr;
    for (Session *s : sessions) {
        int paneId = controller->paneIdForSession(s);
        if (paneId >= 0 && controller->windowIdForPane(paneId) != firstWindowId) {
            secondWindowSession = s;
            break;
        }
    }
    QVERIFY2(secondWindowSession, "Should find a session belonging to the second window");

    // Close the second window's pane session from Konsole.
    // This exercises the fix: VirtualSession::closeInNormalWay() should
    // send kill-pane to tmux, which destroys the window (single-pane window).
    secondWindowSession->closeInNormalWay();

    // Wait for the tab to be removed
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() == 1;
        }(),
        10000);

    // Verify tmux also has only 1 window
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            QProcess listWindows;
            listWindows.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("list-windows"), QStringLiteral("-t"), ctx.sessionName});
            listWindows.waitForFinished(3000);
            QString windowOutput = QString::fromUtf8(listWindows.readAllStandardOutput()).trimmed();
            int windowCount = windowOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size();
            return windowCount == 1;
        }(),
        10000);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testRenameWindowFromTmuxUpdatesTab()
{
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    QVERIFY(container->count() >= 1);

    // Rename the window from tmux
    QProcess renameWindow;
    renameWindow.start(tmuxPath,
                       {QStringLiteral("-S"),
                        ctx.socketPath,
                        QStringLiteral("rename-window"),
                        QStringLiteral("-t"),
                        QStringLiteral("%1:0").arg(ctx.sessionName),
                        QStringLiteral("my-custom-name")});
    QVERIFY(renameWindow.waitForFinished(5000));
    QCOMPARE(renameWindow.exitCode(), 0);

    // Wait for the tab title to update
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!c || c->count() < 1)
                return false;
            // Check all tabs for the renamed title
            for (int i = 0; i < c->count(); ++i) {
                if (c->tabText(i) == QStringLiteral("my-custom-name")) {
                    return true;
                }
            }
            return false;
        }(),
        10000);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSwapPaneFromTmux()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a 2-pane horizontal split
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the splitter with 2 displays
    auto *container = attach.mw->viewManager()->activeContainer();
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
            return false;
        }(),
        10000);

    // Record which pane is on the left and which is on the right
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    int leftPaneId = controller->paneIdForSession(leftDisplay->sessionController()->session());
    int rightPaneId = controller->paneIdForSession(rightDisplay->sessionController()->session());
    QVERIFY(leftPaneId >= 0);
    QVERIFY(rightPaneId >= 0);
    QVERIFY(leftPaneId != rightPaneId);

    // Swap panes from tmux
    QProcess swapPane;
    swapPane.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("swap-pane"),
                    QStringLiteral("-s"),
                    QLatin1Char('%') + QString::number(leftPaneId),
                    QStringLiteral("-t"),
                    QLatin1Char('%') + QString::number(rightPaneId)});
    QVERIFY(swapPane.waitForFinished(5000));
    QCOMPARE(swapPane.exitCode(), 0);

    // Wait for the layout change to be applied — the pane IDs should swap positions
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            // Re-find the splitter (it may have been rebuilt)
            paneSplitter = nullptr;
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    break;
                }
            }
            if (!paneSplitter)
                return false;
            auto *newLeft = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
            auto *newRight = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
            if (!newLeft || !newRight)
                return false;
            int newLeftPaneId = controller->paneIdForSession(newLeft->sessionController()->session());
            int newRightPaneId = controller->paneIdForSession(newRight->sessionController()->session());
            // After swap: the originally-left pane should now be on the right and vice versa
            return newLeftPaneId == rightPaneId && newRightPaneId == leftPaneId;
        }(),
        10000);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testSwapPaneFromKonsole()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a 2-pane horizontal split
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Find the splitter with 2 displays
    auto *container = attach.mw->viewManager()->activeContainer();
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
            return false;
        }(),
        10000);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    auto *leftDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
    auto *rightDisplay = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
    QVERIFY(leftDisplay);
    QVERIFY(rightDisplay);

    int leftPaneId = controller->paneIdForSession(leftDisplay->sessionController()->session());
    int rightPaneId = controller->paneIdForSession(rightDisplay->sessionController()->session());
    QVERIFY(leftPaneId >= 0);
    QVERIFY(rightPaneId >= 0);
    QVERIFY(leftPaneId != rightPaneId);

    // Swap panes from Konsole
    controller->requestSwapPane(leftPaneId, rightPaneId);

    // Wait for the layout change — pane positions should swap
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            paneSplitter = nullptr;
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    break;
                }
            }
            if (!paneSplitter)
                return false;
            auto *newLeft = qobject_cast<TerminalDisplay *>(paneSplitter->widget(0));
            auto *newRight = qobject_cast<TerminalDisplay *>(paneSplitter->widget(1));
            if (!newLeft || !newRight)
                return false;
            int newLeftPaneId = controller->paneIdForSession(newLeft->sessionController()->session());
            int newRightPaneId = controller->paneIdForSession(newRight->sessionController()->session());
            return newLeftPaneId == rightPaneId && newRightPaneId == leftPaneId;
        }(),
        10000);

    // Verify tmux also reflects the swap
    QProcess listPanes;
    listPanes.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id}")});
    QVERIFY(listPanes.waitForFinished(5000));
    QCOMPARE(listPanes.exitCode(), 0);
    QStringList paneOrder = QString::fromUtf8(listPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(paneOrder.size(), 2);
    // After swap: first pane in tmux list should be the originally-right pane
    QCOMPARE(paneOrder[0], QLatin1Char('%') + QString::number(rightPaneId));
    QCOMPARE(paneOrder[1], QLatin1Char('%') + QString::number(leftPaneId));

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

void TmuxIntegrationTest::testMovePaneFromTmux()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a session with 2 windows, each with 1 pane
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // Get pane IDs from each window
    const auto &windowTabs = controller->windowToTabIndex();
    QList<int> windowIds = windowTabs.keys();
    QCOMPARE(windowIds.size(), 2);

    // Get the pane ID from window 1 (the second window)
    int window1PaneId = -1;
    {
        int tabIndex = windowTabs.value(windowIds[1]);
        auto *splitter = container->viewSplitterAt(tabIndex);
        QVERIFY(splitter);
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
        window1PaneId = controller->paneIdForSession(terminals.first()->sessionController()->session());
        QVERIFY(window1PaneId >= 0);
    }

    // Get the pane ID from window 0
    int window0PaneId = -1;
    {
        int tabIndex = windowTabs.value(windowIds[0]);
        auto *splitter = container->viewSplitterAt(tabIndex);
        QVERIFY(splitter);
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
        window0PaneId = controller->paneIdForSession(terminals.first()->sessionController()->session());
        QVERIFY(window0PaneId >= 0);
    }

    // Move pane from window 1 into window 0 (horizontal split) via tmux
    QProcess movePane;
    movePane.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("move-pane"),
                    QStringLiteral("-h"),
                    QStringLiteral("-s"),
                    QLatin1Char('%') + QString::number(window1PaneId),
                    QStringLiteral("-t"),
                    QLatin1Char('%') + QString::number(window0PaneId)});
    QVERIFY(movePane.waitForFinished(5000));
    QCOMPARE(movePane.exitCode(), 0);

    // Window 1 should disappear (it had only 1 pane), leaving 1 tab with 2 panes
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!c || c->count() != 1)
                return false;
            auto *splitter = c->viewSplitterAt(0);
            return splitter && splitter->findChildren<TerminalDisplay *>().size() == 2;
        }(),
        10000);

    QCOMPARE(controller->windowCount(), 1);

    // Cleanup
    delete attach.mw.data();
}

void TmuxIntegrationTest::testMovePaneFromKonsole()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a session with 2 windows, each with 1 pane
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            return c && c->count() >= 2;
        }(),
        10000);

    auto *container = attach.mw->viewManager()->activeContainer();
    QCOMPARE(container->count(), 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // Get pane IDs from each window
    const auto &windowTabs = controller->windowToTabIndex();
    QList<int> windowIds = windowTabs.keys();
    QCOMPARE(windowIds.size(), 2);

    int window0PaneId = -1;
    int window1PaneId = -1;
    {
        auto *splitter0 = container->viewSplitterAt(windowTabs.value(windowIds[0]));
        auto *splitter1 = container->viewSplitterAt(windowTabs.value(windowIds[1]));
        QVERIFY(splitter0);
        QVERIFY(splitter1);
        window0PaneId = controller->paneIdForSession(splitter0->findChildren<TerminalDisplay *>().first()->sessionController()->session());
        window1PaneId = controller->paneIdForSession(splitter1->findChildren<TerminalDisplay *>().first()->sessionController()->session());
        QVERIFY(window0PaneId >= 0);
        QVERIFY(window1PaneId >= 0);
    }

    // Move pane from window 1 into window 0 via Konsole
    controller->requestMovePane(window1PaneId, window0PaneId, Qt::Horizontal, false);

    // Window 1 should disappear, leaving 1 tab with 2 panes
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!c || c->count() != 1)
                return false;
            auto *splitter = c->viewSplitterAt(0);
            return splitter && splitter->findChildren<TerminalDisplay *>().size() == 2;
        }(),
        10000);

    QCOMPARE(controller->windowCount(), 1);

    // Verify tmux also has 1 window with 2 panes
    QProcess listPanes;
    listPanes.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id}")});
    QVERIFY(listPanes.waitForFinished(5000));
    QCOMPARE(listPanes.exitCode(), 0);
    QStringList paneLines = QString::fromUtf8(listPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(paneLines.size(), 2);

    // Cleanup
    delete attach.mw.data();
}

void TmuxIntegrationTest::testMovePaneFromTwoToOneFromTmux()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a 2-pane window + a 1-pane window
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window with 1 pane
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    auto *container = attach.mw->viewManager()->activeContainer();
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 2, 10000);
    QCOMPARE(container->count(), 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    // Find the 2-pane window and the 1-pane window
    const auto &windowTabs = controller->windowToTabIndex();
    int twoPaneWindowId = -1;
    int onePaneWindowId = -1;
    int movePaneId = -1; // pane to move from the 2-pane window
    int targetPaneId = -1; // pane in the 1-pane window
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        if (controller->paneCountForWindow(it.key()) == 2) {
            twoPaneWindowId = it.key();
        } else if (controller->paneCountForWindow(it.key()) == 1) {
            onePaneWindowId = it.key();
        }
    }
    QVERIFY(twoPaneWindowId >= 0);
    QVERIFY(onePaneWindowId >= 0);

    // Get a pane ID from the 2-pane window (the right one)
    {
        auto *splitter = container->viewSplitterAt(windowTabs.value(twoPaneWindowId));
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 2);
        movePaneId = controller->paneIdForSession(terminals.last()->sessionController()->session());
    }
    // Get the pane ID from the 1-pane window
    {
        auto *splitter = container->viewSplitterAt(windowTabs.value(onePaneWindowId));
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
        targetPaneId = controller->paneIdForSession(terminals.first()->sessionController()->session());
    }
    QVERIFY(movePaneId >= 0);
    QVERIFY(targetPaneId >= 0);

    // Move one pane from the 2-pane window to the 1-pane window via tmux
    QProcess movePane;
    movePane.start(tmuxPath,
                   {QStringLiteral("-S"),
                    ctx.socketPath,
                    QStringLiteral("move-pane"),
                    QStringLiteral("-h"),
                    QStringLiteral("-s"),
                    QLatin1Char('%') + QString::number(movePaneId),
                    QStringLiteral("-t"),
                    QLatin1Char('%') + QString::number(targetPaneId)});
    QVERIFY(movePane.waitForFinished(5000));
    QCOMPARE(movePane.exitCode(), 0);

    // Both windows should now have 1 and 2 panes (moved from first to second)
    // Still 2 tabs, but the pane counts should have changed
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            return controller->paneCountForWindow(twoPaneWindowId) == 1 && controller->paneCountForWindow(onePaneWindowId) == 2;
        }(),
        10000);

    // Verify Konsole splitter tree matches
    {
        auto *splitter1 = container->viewSplitterAt(windowTabs.value(twoPaneWindowId));
        auto *splitter2 = container->viewSplitterAt(windowTabs.value(onePaneWindowId));
        QVERIFY(splitter1);
        QVERIFY(splitter2);
        QCOMPARE(splitter1->findChildren<TerminalDisplay *>().size(), 1);
        QCOMPARE(splitter2->findChildren<TerminalDisplay *>().size(), 2);
    }

    // Cleanup
    delete attach.mw.data();
}

void TmuxIntegrationTest::testMovePaneFromTwoToOneFromKonsole()
{
    const QString tmuxPath = findTmuxOrSkip();

    // Create a 2-pane window + a 1-pane window
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Add a second window with 1 pane
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Wait for 2 tabs
    auto *container = attach.mw->viewManager()->activeContainer();
    QTRY_VERIFY_WITH_TIMEOUT(container && container->count() >= 2, 10000);
    QCOMPARE(container->count(), 2);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);

    const auto &windowTabs = controller->windowToTabIndex();
    int twoPaneWindowId = -1;
    int onePaneWindowId = -1;
    int movePaneId = -1;
    int targetPaneId = -1;
    for (auto it = windowTabs.constBegin(); it != windowTabs.constEnd(); ++it) {
        if (controller->paneCountForWindow(it.key()) == 2) {
            twoPaneWindowId = it.key();
        } else if (controller->paneCountForWindow(it.key()) == 1) {
            onePaneWindowId = it.key();
        }
    }
    QVERIFY(twoPaneWindowId >= 0);
    QVERIFY(onePaneWindowId >= 0);

    {
        auto *splitter = container->viewSplitterAt(windowTabs.value(twoPaneWindowId));
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 2);
        movePaneId = controller->paneIdForSession(terminals.last()->sessionController()->session());
    }
    {
        auto *splitter = container->viewSplitterAt(windowTabs.value(onePaneWindowId));
        auto terminals = splitter->findChildren<TerminalDisplay *>();
        QCOMPARE(terminals.size(), 1);
        targetPaneId = controller->paneIdForSession(terminals.first()->sessionController()->session());
    }
    QVERIFY(movePaneId >= 0);
    QVERIFY(targetPaneId >= 0);

    // Move pane from 2-pane window to 1-pane window via Konsole
    controller->requestMovePane(movePaneId, targetPaneId, Qt::Horizontal, false);

    // Pane counts should change: 2→1, 1→2
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            return controller->paneCountForWindow(twoPaneWindowId) == 1 && controller->paneCountForWindow(onePaneWindowId) == 2;
        }(),
        10000);

    // Verify splitter trees
    {
        auto *splitter1 = container->viewSplitterAt(windowTabs.value(twoPaneWindowId));
        auto *splitter2 = container->viewSplitterAt(windowTabs.value(onePaneWindowId));
        QVERIFY(splitter1);
        QVERIFY(splitter2);
        QCOMPARE(splitter1->findChildren<TerminalDisplay *>().size(), 1);
        QCOMPARE(splitter2->findChildren<TerminalDisplay *>().size(), 2);
    }

    // Verify tmux agrees
    QProcess listWindows;
    listWindows.start(tmuxPath,
                      {QStringLiteral("-S"),
                       ctx.socketPath,
                       QStringLiteral("list-panes"),
                       QStringLiteral("-a"),
                       QStringLiteral("-t"),
                       ctx.sessionName,
                       QStringLiteral("-F"),
                       QStringLiteral("#{window_id} #{pane_id}")});
    QVERIFY(listWindows.waitForFinished(5000));
    QCOMPARE(listWindows.exitCode(), 0);
    QStringList lines = QString::fromUtf8(listWindows.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 3); // 3 total panes across 2 windows

    // Cleanup
    delete attach.mw.data();
}

void TmuxIntegrationTest::testNewTabFromTmuxPane()
{
    // When the user invokes New Tab (Ctrl+T) while focused on a tmux pane,
    // a new tmux window should be created without any confirmation dialog.
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto *container = attach.mw->viewManager()->activeContainer();
    QVERIFY(container);
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 1, 10000);

    // Trigger the New Tab action (Ctrl+Shift+T)
    QAction *newTabAction = attach.mw->actionCollection()->action(QStringLiteral("new-tab"));
    QVERIFY2(newTabAction, "new-tab action not found");
    newTabAction->trigger();

    // A new tmux tab should appear without any dialog
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 2, 10000);

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // The tab bar should be visible now that there are 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(container->tabBar()->isVisible(), 5000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testDetachViewBreaksPane()
{
    // Ctrl+Shift+H (detach-view action) on a tmux pane should break the pane
    // out into a new tmux window (= new Konsole tab).
    const QString tmuxPath = findTmuxOrSkip();

    // Start with a 2-pane window so there's something to break out
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    auto *container = attach.mw->viewManager()->activeContainer();
    QVERIFY(container);

    // Wait for the 2-pane splitter to appear
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *s = container->viewSplitterAt(0);
            return s && s->findChildren<TerminalDisplay *>().size() == 2;
        }(),
        10000);
    QCOMPARE(container->count(), 1);

    // Trigger the detach-view action (Ctrl+Shift+H)
    QAction *detachAction = attach.mw->actionCollection()->action(QStringLiteral("detach-view"));
    QVERIFY2(detachAction, "detach-view action not found");
    detachAction->trigger();

    // A new tab should appear with the broken-out pane
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 2, 10000);

    // Each tab should have exactly 1 pane
    for (int i = 0; i < container->count(); ++i) {
        auto *s = container->viewSplitterAt(i);
        QVERIFY(s);
        QCOMPARE(s->findChildren<TerminalDisplay *>().size(), 1);
    }

    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(attach.mw->viewManager()->sessions().first());
    QVERIFY(controller);
    QCOMPARE(controller->windowCount(), 2);

    // Tab bar should be visible with 2 tabs
    QTRY_VERIFY_WITH_TIMEOUT(container->tabBar()->isVisible(), 5000);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testDetachFromTmuxAction()
{
    // The detach-from-tmux action should exist in the action collection
    // and trigger a tmux detach — the tmux subprocess disconnects but the
    // tmux server keeps running.
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    QAction *detachAction = attach.mw->actionCollection()->action(QStringLiteral("detach-from-tmux"));
    QVERIFY2(detachAction, "detach-from-tmux action not found");

    // Watch for the bridge disconnecting after detach
    QSignalSpy disconnectSpy(attach.bridge, &TmuxProcessBridge::disconnected);

    detachAction->trigger();

    // The bridge should disconnect (tmux control client exits on detach)
    QTRY_VERIFY_WITH_TIMEOUT(disconnectSpy.count() >= 1, 10000);

    // The tmux server itself should still be running — we can still query it
    QProcess listSessions;
    listSessions.start(tmuxPath, {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("list-sessions")});
    QVERIFY(listSessions.waitForFinished(5000));
    QCOMPARE(listSessions.exitCode(), 0);

    delete attach.mw.data();
}

void TmuxIntegrationTest::testClosePaneFromSessionControllerConfirmed()
{
    // Closing a single pane in a multi-pane window via SessionController::closeSession
    // should show a confirmation dialog. Preset "don't ask again" = PrimaryAction
    // so the close proceeds without blocking the test.
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
            return false;
        }(),
        10000);

    // Preset "don't ask again" to Close Pane (PrimaryAction)
    KMessageBox::saveDontShowAgainTwoActions(QStringLiteral("ConfirmCloseTmuxPane"), KMessageBox::PrimaryAction);

    // Close one of the panes via its SessionController
    auto terminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(terminals.size(), 2);
    auto *controller = terminals.first()->sessionController();
    QVERIFY(controller);
    controller->closeSession();

    // Pane count should drop to 1 (single pane remaining, the window is not closed)
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            auto *c = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
            if (!c || c->count() != 1)
                return false;
            auto *s = c->viewSplitterAt(0);
            return s && s->findChildren<TerminalDisplay *>().size() == 1;
        }(),
        10000);

    // Reset the setting so other tests aren't affected
    KMessageBox::enableAllMessages();

    delete attach.mw.data();
}

void TmuxIntegrationTest::testClosePaneFromSessionControllerCancelled()
{
    // When "don't ask again" = SecondaryAction (Cancel), closeSession should
    // do nothing — the pane stays.
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┬────────────────────────────────────────┐
        │ cmd: sleep 60                          │ cmd: sleep 60                          │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        │                                        │                                        │
        └────────────────────────────────────────┴────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    ViewSplitter *paneSplitter = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT(
        [&]() {
            for (int i = 0; i < container->count(); ++i) {
                auto *splitter = container->viewSplitterAt(i);
                if (splitter && splitter->findChildren<TerminalDisplay *>().size() == 2) {
                    paneSplitter = splitter;
                    return true;
                }
            }
            return false;
        }(),
        10000);

    // Preset "don't ask again" = Cancel (SecondaryAction)
    KMessageBox::saveDontShowAgainTwoActions(QStringLiteral("ConfirmCloseTmuxPane"), KMessageBox::SecondaryAction);

    auto terminals = paneSplitter->findChildren<TerminalDisplay *>();
    QCOMPARE(terminals.size(), 2);
    auto *sc = terminals.first()->sessionController();
    sc->closeSession();

    // Give async tmux work a moment to potentially produce layout changes
    QTest::qWait(500);

    // Still 2 panes — close was cancelled
    QCOMPARE(paneSplitter->findChildren<TerminalDisplay *>().size(), 2);

    KMessageBox::enableAllMessages();

    delete attach.mw.data();
}

void TmuxIntegrationTest::testCloseTabFromContainerConfirmed()
{
    // When the user clicks the X on a tab (closeTerminalTab path) or uses Ctrl+W,
    // a confirmation dialog appears. Preset "don't ask again" so the close proceeds.
    const QString tmuxPath = findTmuxOrSkip();

    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    // Add a second tmux window so we have 2 tabs — closing one shouldn't tear down kmux
    QProcess newWindow;
    newWindow.start(tmuxPath,
                    {QStringLiteral("-S"), ctx.socketPath, QStringLiteral("new-window"), QStringLiteral("-t"), ctx.sessionName, QStringLiteral("sleep 60")});
    QVERIFY(newWindow.waitForFinished(5000));
    QCOMPARE(newWindow.exitCode(), 0);

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    auto *container = attach.mw->viewManager()->activeContainer();
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 2, 10000);

    // Preset "don't ask again" to Close Tab (PrimaryAction)
    KMessageBox::saveDontShowAgainTwoActions(QStringLiteral("ConfirmCloseTmuxWindow"), KMessageBox::PrimaryAction);

    // Trigger tab close via the tab bar's tabCloseRequested signal —
    // this is what clicking the X invokes.
    QTabBar *tabBar = container->tabBar();
    QVERIFY(tabBar);
    QMetaObject::invokeMethod(tabBar, "tabCloseRequested", Qt::DirectConnection, Q_ARG(int, 0));

    // One tab should be gone
    QTRY_COMPARE_WITH_TIMEOUT(container->count(), 1, 10000);

    KMessageBox::enableAllMessages();

    delete attach.mw.data();
}

void TmuxIntegrationTest::testFractalSplitDownRight8()
{
    const int depth = 8;
    const QString tmuxPath = findTmuxOrSkip();

    // Use a large window so deep splits don't hit tmux minimum pane size
    SessionContext ctx;
    setupTmuxSession(parse(QStringLiteral(R"(
        ┌────────────────────────────────────────────────────────────────────────────────┐
        │ cmd: sleep 60                                                                  │
        │ columns: 256                                                                   │
        │ lines: 64                                                                      │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        │                                                                                │
        └────────────────────────────────────────────────────────────────────────────────┘
    )")),
                                  tmuxPath,
                                  m_tmuxTmpDir.path(),
                                  ctx);
    auto cleanup = qScopeGuard([&] {
        killTmuxSession(tmuxPath, ctx);
    });

    AttachResult attach;
    attachKonsole(tmuxPath, ctx, attach);

    // Show and activate the window so setFocus() works
    attach.mw->show();
    QVERIFY(QTest::qWaitForWindowActive(attach.mw));

    const auto sessions = attach.mw->viewManager()->sessions();
    QVERIFY(!sessions.isEmpty());
    auto *controller = TmuxControllerRegistry::instance()->controllerForSession(sessions.first());
    QVERIFY(controller);

    // Perform alternating horizontal/vertical splits: )()()...
    const int expectedPanes = depth + 1;

    // Look up the split actions — these are the actual hotkey handlers
    // Ctrl+( = split-view-left-right (horizontal), Ctrl+) = split-view-top-bottom (vertical)
    QAction *splitH = attach.mw->actionCollection()->action(QStringLiteral("split-view-left-right"));
    QAction *splitV = attach.mw->actionCollection()->action(QStringLiteral("split-view-top-bottom"));
    QVERIFY2(splitH, "split-view-left-right action not found");
    QVERIFY2(splitV, "split-view-top-bottom action not found");

    for (int i = 0; i < depth; ++i) {
        // Alternate: ( ) ( ) ...
        QAction *action = (i % 2 == 0) ? splitH : splitV;
        action->trigger();

        // Wait for pane count to increase
        int expectedCount = i + 2;
        QTRY_VERIFY_WITH_TIMEOUT(
            [&]() {
                auto *container = attach.mw ? attach.mw->viewManager()->activeContainer() : nullptr;
                if (!container)
                    return false;
                for (int t = 0; t < container->count(); ++t) {
                    auto *splitter = container->viewSplitterAt(t);
                    if (splitter && splitter->findChildren<TerminalDisplay *>().size() == expectedCount) {
                        return true;
                    }
                }
                return false;
            }(),
            10000);
    }

    // Verify tmux has the expected number of panes
    QProcess listPanes;
    listPanes.start(tmuxPath,
                    {QStringLiteral("-S"),
                     ctx.socketPath,
                     QStringLiteral("list-panes"),
                     QStringLiteral("-t"),
                     ctx.sessionName,
                     QStringLiteral("-F"),
                     QStringLiteral("#{pane_id}")});
    QVERIFY(listPanes.waitForFinished(5000));
    QCOMPARE(listPanes.exitCode(), 0);
    QStringList paneLines = QString::fromUtf8(listPanes.readAllStandardOutput()).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(paneLines.size(), expectedPanes);

    // Verify the fractal splitter structure:
    // HSplit[Leaf, VSplit[Leaf, HSplit[Leaf, ...]]]
    // Alternating H/V, nested child always second (right/bottom).
    auto *container = attach.mw->viewManager()->activeContainer();
    QVERIFY(container);
    ViewSplitter *paneSplitter = nullptr;
    for (int i = 0; i < container->count(); ++i) {
        auto *splitter = container->viewSplitterAt(i);
        if (splitter && splitter->findChildren<TerminalDisplay *>().size() == expectedPanes) {
            paneSplitter = splitter;
            break;
        }
    }
    QVERIFY2(paneSplitter, qPrintable(QStringLiteral("Expected a ViewSplitter with %1 TerminalDisplay children").arg(expectedPanes)));

    // Verify the fractal structure: walk the tree always taking the last child.
    // At each level the orientation should alternate H, V, H, V, ...
    if (depth >= 1) {
        ViewSplitter *current = paneSplitter;
        for (int d = 0; d < depth; ++d) {
            Qt::Orientation expectedOrientation = (d % 2 == 0) ? Qt::Horizontal : Qt::Vertical;
            QVERIFY2(current->orientation() == expectedOrientation,
                     qPrintable(QStringLiteral("Depth %1: expected %2 but got %3")
                                    .arg(d)
                                    .arg(expectedOrientation == Qt::Horizontal ? QStringLiteral("Horizontal") : QStringLiteral("Vertical"))
                                    .arg(current->orientation() == Qt::Horizontal ? QStringLiteral("Horizontal") : QStringLiteral("Vertical"))));
            QVERIFY2(current->count() >= 2, qPrintable(QStringLiteral("Depth %1: expected at least 2 children but got %2").arg(d).arg(current->count())));

            // The last child should be a ViewSplitter (except at deepest level)
            auto *lastChild = current->widget(current->count() - 1);
            if (d < depth - 1) {
                auto *nextSplitter = qobject_cast<ViewSplitter *>(lastChild);
                QVERIFY2(nextSplitter, qPrintable(QStringLiteral("Depth %1: last child should be a ViewSplitter").arg(d)));
                current = nextSplitter;
            }
        }
    }

    // Verify the final bottom-right pane has focus.
    // Walk down always taking the last child to find the deepest bottom-right display.
    QWidget *node = paneSplitter;
    while (auto *splitter = qobject_cast<ViewSplitter *>(node)) {
        node = splitter->widget(splitter->count() - 1);
    }
    auto *deepestBottomRight = qobject_cast<TerminalDisplay *>(node);
    QVERIFY(deepestBottomRight);
    QTRY_VERIFY_WITH_TIMEOUT(deepestBottomRight->hasFocus(), 5000);

    // Cleanup
    killTmuxSession(tmuxPath, ctx);
    QTRY_VERIFY_WITH_TIMEOUT(!attach.mw, 10000);
    delete attach.mw.data();
}

QTEST_MAIN(TmuxIntegrationTest)


void TmuxIntegrationTest::testParseSinglePane()
{
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┐
        │ id: A                                  │
        │ cmd:                                   │
        │ sleep 30                               │
        │                                        │
        │                                        │
        └────────────────────────────────────────┘
    )"));

    QCOMPARE(spec.layout.type, LayoutSpec::Leaf);
    QCOMPARE(spec.layout.pane.id, QStringLiteral("A"));
    QCOMPARE(spec.layout.pane.cmd, QStringLiteral("sleep 30"));
    QVERIFY(spec.layout.pane.columns.has_value());
    QCOMPARE(spec.layout.pane.columns.value(), 40);
    QVERIFY(spec.layout.pane.lines.has_value());
    QCOMPARE(spec.layout.pane.lines.value(), 5);
}

void TmuxIntegrationTest::testParseTwoHorizontalPanes()
{
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │ id: L              │ id: R              │
        │ cmd:               │ cmd:               │
        │ sleep 30           │ sleep 30           │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )"));

    QCOMPARE(spec.layout.type, LayoutSpec::HSplit);
    QCOMPARE(spec.layout.children.size(), 2);

    QCOMPARE(spec.layout.children[0].type, LayoutSpec::Leaf);
    QCOMPARE(spec.layout.children[0].pane.id, QStringLiteral("L"));
    QCOMPARE(spec.layout.children[0].pane.cmd, QStringLiteral("sleep 30"));
    QCOMPARE(spec.layout.children[0].pane.columns.value(), 20);
    QCOMPARE(spec.layout.children[0].pane.lines.value(), 5);

    QCOMPARE(spec.layout.children[1].type, LayoutSpec::Leaf);
    QCOMPARE(spec.layout.children[1].pane.id, QStringLiteral("R"));
    QCOMPARE(spec.layout.children[1].pane.cmd, QStringLiteral("sleep 30"));
    QCOMPARE(spec.layout.children[1].pane.columns.value(), 20);
    QCOMPARE(spec.layout.children[1].pane.lines.value(), 5);
}

void TmuxIntegrationTest::testParseTwoVerticalPanes()
{
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┐
        │ id: T              │
        │ cmd:               │
        │ sleep 30           │
        │                    │
        │                    │
        ├────────────────────┤
        │ id: B              │
        │ cmd:               │
        │ sleep 30           │
        │                    │
        │                    │
        └────────────────────┘
    )"));

    QCOMPARE(spec.layout.type, LayoutSpec::VSplit);
    QCOMPARE(spec.layout.children.size(), 2);

    QCOMPARE(spec.layout.children[0].type, LayoutSpec::Leaf);
    QCOMPARE(spec.layout.children[0].pane.id, QStringLiteral("T"));
    QCOMPARE(spec.layout.children[0].pane.columns.value(), 20);
    QCOMPARE(spec.layout.children[0].pane.lines.value(), 5);

    QCOMPARE(spec.layout.children[1].type, LayoutSpec::Leaf);
    QCOMPARE(spec.layout.children[1].pane.id, QStringLiteral("B"));
    QCOMPARE(spec.layout.children[1].pane.columns.value(), 20);
    QCOMPARE(spec.layout.children[1].pane.lines.value(), 5);
}

void TmuxIntegrationTest::testParseNestedLayout()
{
    // [ L | [ RT / RB ] ]
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │ id: L              │ id: RT             │
        │ cmd:               │ cmd:               │
        │ sleep 60           │ sleep 60           │
        │                    │                    │
        │                    │                    │
        │                    ├────────────────────┤
        │                    │ id: RB             │
        │                    │ cmd:               │
        │                    │ sleep 60           │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )"));

    QCOMPARE(spec.layout.type, LayoutSpec::HSplit);
    QCOMPARE(spec.layout.children.size(), 2);

    // Left child is a leaf with full height (11 = 5 + 1 + 5)
    QCOMPARE(spec.layout.children[0].type, LayoutSpec::Leaf);
    QCOMPARE(spec.layout.children[0].pane.id, QStringLiteral("L"));
    QCOMPARE(spec.layout.children[0].pane.columns.value(), 20);
    QCOMPARE(spec.layout.children[0].pane.lines.value(), 11);

    // Right child is a VSplit
    QCOMPARE(spec.layout.children[1].type, LayoutSpec::VSplit);
    QCOMPARE(spec.layout.children[1].children.size(), 2);
    QCOMPARE(spec.layout.children[1].children[0].pane.id, QStringLiteral("RT"));
    QCOMPARE(spec.layout.children[1].children[0].pane.columns.value(), 20);
    QCOMPARE(spec.layout.children[1].children[0].pane.lines.value(), 5);
    QCOMPARE(spec.layout.children[1].children[1].pane.id, QStringLiteral("RB"));
    QCOMPARE(spec.layout.children[1].children[1].pane.columns.value(), 20);
    QCOMPARE(spec.layout.children[1].children[1].pane.lines.value(), 5);

    // Computed window size should be 20+1+20 = 41 x 11
    auto windowSize = computeWindowSize(spec.layout);
    QCOMPARE(windowSize.first, 41);
    QCOMPARE(windowSize.second, 11);
}

void TmuxIntegrationTest::testParseFooterMetadata()
{
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
        tab: bash
        ratio: 3:1
    )"));

    QVERIFY(spec.tab.has_value());
    QCOMPARE(spec.tab.value(), QStringLiteral("bash"));

    QVERIFY(spec.ratio.has_value());
    QCOMPARE(spec.ratio->size(), 2);
    QCOMPARE(spec.ratio->at(0), 3);
    QCOMPARE(spec.ratio->at(1), 1);
}

void TmuxIntegrationTest::testParsePaneAnnotations()
{
    // Explicit columns/lines annotations override box geometry
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┐
        │ id: main           │
        │ cmd: sleep 30      │
        │ contains: MARKER   │
        │ focused: true      │
        │ columns: 80        │
        │ lines: 24          │
        │ title: bash        │
        └────────────────────┘
    )"));

    QCOMPARE(spec.layout.type, LayoutSpec::Leaf);
    auto &pane = spec.layout.pane;
    QCOMPARE(pane.id, QStringLiteral("main"));
    QCOMPARE(pane.cmd, QStringLiteral("sleep 30"));
    QCOMPARE(pane.contains.size(), 1);
    QCOMPARE(pane.contains[0], QStringLiteral("MARKER"));
    QVERIFY(pane.focused.has_value());
    QCOMPARE(pane.focused.value(), true);
    // Explicit annotations override box geometry (box is 20x7)
    QVERIFY(pane.columns.has_value());
    QCOMPARE(pane.columns.value(), 80);
    QVERIFY(pane.lines.has_value());
    QCOMPARE(pane.lines.value(), 24);
    QCOMPARE(pane.title, QStringLiteral("bash"));
}

void TmuxIntegrationTest::testParseMultilineCommand()
{
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────────────────────────┐
        │ id: A                                  │
        │ cmd:                                   │
        │ sleep 30                               │
        │                                        │
        │                                        │
        └────────────────────────────────────────┘
    )"));

    QCOMPARE(spec.layout.pane.cmd, QStringLiteral("sleep 30"));
}

void TmuxIntegrationTest::testParseFourPaneGrid()
{
    // [ [ TL / BL ] | [ TR / BR ] ]
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │ id: TL             │ id: TR             │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        ├────────────────────┼────────────────────┤
        │ id: BL             │ id: BR             │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )"));

    QCOMPARE(spec.layout.type, LayoutSpec::HSplit);
    QCOMPARE(spec.layout.children.size(), 2);

    // Left column is VSplit
    QCOMPARE(spec.layout.children[0].type, LayoutSpec::VSplit);
    QCOMPARE(spec.layout.children[0].children.size(), 2);
    QCOMPARE(spec.layout.children[0].children[0].pane.id, QStringLiteral("TL"));
    QCOMPARE(spec.layout.children[0].children[1].pane.id, QStringLiteral("BL"));

    // Right column is VSplit
    QCOMPARE(spec.layout.children[1].type, LayoutSpec::VSplit);
    QCOMPARE(spec.layout.children[1].children.size(), 2);
    QCOMPARE(spec.layout.children[1].children[0].pane.id, QStringLiteral("TR"));
    QCOMPARE(spec.layout.children[1].children[1].pane.id, QStringLiteral("BR"));
}

void TmuxIntegrationTest::testParseThreeHorizontalPanes()
{
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┬────────────────────┐
        │ id: A              │ id: B              │ id: C              │
        │                    │                    │                    │
        │                    │                    │                    │
        │                    │                    │                    │
        │                    │                    │                    │
        └────────────────────┴────────────────────┴────────────────────┘
    )"));

    QCOMPARE(spec.layout.type, LayoutSpec::HSplit);
    QCOMPARE(spec.layout.children.size(), 3);
    QCOMPARE(spec.layout.children[0].pane.id, QStringLiteral("A"));
    QCOMPARE(spec.layout.children[1].pane.id, QStringLiteral("B"));
    QCOMPARE(spec.layout.children[2].pane.id, QStringLiteral("C"));
}

void TmuxIntegrationTest::testParseEmptyPanes()
{
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )"));

    QCOMPARE(spec.layout.type, LayoutSpec::HSplit);
    QCOMPARE(spec.layout.children.size(), 2);
    QCOMPARE(spec.layout.children[0].type, LayoutSpec::Leaf);
    QCOMPARE(spec.layout.children[1].type, LayoutSpec::Leaf);
    // Panes should have empty annotations (but columns/lines auto-populated)
    QVERIFY(spec.layout.children[0].pane.id.isEmpty());
    QVERIFY(spec.layout.children[1].pane.id.isEmpty());
    QCOMPARE(spec.layout.children[0].pane.columns.value(), 20);
    QCOMPARE(spec.layout.children[0].pane.lines.value(), 5);
}

void TmuxIntegrationTest::testCountPanes()
{
    auto spec = parse(QStringLiteral(R"(
        ┌────────────────────┬────────────────────┐
        │ id: L              │ id: RT             │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    ├────────────────────┤
        │                    │ id: RB             │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        │                    │                    │
        └────────────────────┴────────────────────┘
    )"));

    QCOMPARE(countPanes(spec.layout), 3);
}

#include "moc_TmuxIntegrationTest.cpp"
