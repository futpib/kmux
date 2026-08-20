/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXRESIZECOORDINATOR_H
#define TMUXRESIZECOORDINATOR_H

#include <QMap>
#include <QObject>
#include <QSize>
#include <QTimer>

namespace Konsole
{

class TmuxController;
class TmuxGateway;
class TmuxLayoutManager;
class TmuxPaneManager;
class ViewManager;
class ViewSplitter;

class TmuxResizeCoordinator : public QObject
{
    Q_OBJECT
public:
    TmuxResizeCoordinator(TmuxGateway *gateway, TmuxController *controller, TmuxPaneManager *paneManager, TmuxLayoutManager *layoutManager, ViewManager *viewManager, QObject *parent = nullptr);

    void setGateway(TmuxGateway *gateway);

    void onPaneViewSizeChanged(bool suppressResize);
    void onSplitterMoved(ViewSplitter *splitter);
    void sendClientSize();
    void stop();
    void setWindowSize(int windowId, int cols, int lines);

private:
    TmuxGateway *_gateway;
    TmuxController *_controller;
    TmuxPaneManager *_paneManager;
    TmuxLayoutManager *_layoutManager;
    ViewManager *_viewManager;

    QTimer _resizeTimer;
    QMap<int, QSize> _lastClientSizes;
    QMap<int, QSize> _tmuxWindowSizes;
};

} // namespace Konsole

#endif // TMUXRESIZECOORDINATOR_H
