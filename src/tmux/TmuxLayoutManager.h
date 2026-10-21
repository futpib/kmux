/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXLAYOUTMANAGER_H
#define TMUXLAYOUTMANAGER_H

#include <QList>
#include <QMap>
#include <QObject>
#include <QSize>

namespace Konsole
{

class TerminalDisplay;
class TmuxLayoutNode;
class TmuxPaneManager;
class ViewManager;
class ViewSplitter;

class TmuxLayoutManager : public QObject
{
    Q_OBJECT
public:
    TmuxLayoutManager(TmuxPaneManager *paneManager, ViewManager *viewManager, QObject *parent = nullptr);

    int applyLayout(int tabIndex, const TmuxLayoutNode &layout);
    static TmuxLayoutNode buildLayoutNode(ViewSplitter *splitter, TmuxPaneManager *paneManager);
    static QSize layoutSize(ViewSplitter *splitter);
    static void resizeLayout(TmuxLayoutNode &node, int width, int height);

Q_SIGNALS:
    void splitterDragStarted();
    void splitterDragFinished();
    void splitterMoved(ViewSplitter *splitter);

private:
    void collectDisplays(ViewSplitter *splitter, QMap<int, TerminalDisplay *> &displayMap);
    void buildSplitterTree(ViewSplitter *splitter, const TmuxLayoutNode &node, QMap<int, TerminalDisplay *> &existingDisplays);
    bool updateSplitterSizes(ViewSplitter *splitter, const TmuxLayoutNode &node, bool skipSizeUpdate);
    void connectSplitterSignals(ViewSplitter *splitter);

    TmuxPaneManager *_paneManager;
    ViewManager *_viewManager;
};

} // namespace Konsole

#endif // TMUXLAYOUTMANAGER_H
