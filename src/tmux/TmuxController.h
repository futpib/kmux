/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXCONTROLLER_H
#define TMUXCONTROLLER_H

#include <QKeySequence>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <functional>

#include "konsoleprivate_export.h"

namespace Konsole
{

class Session;
class TmuxGateway;
class TmuxLayoutNode;
class TmuxPaneManager;
class TmuxLayoutManager;
class TmuxResizeCoordinator;
class TmuxPaneStateRecovery;
class ViewManager;

class KONSOLEPRIVATE_EXPORT TmuxController : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Initializing, ApplyingLayout, Dragging };

    TmuxController(TmuxGateway *gateway, ViewManager *viewManager, QObject *parent = nullptr);
    ~TmuxController() override;

    void initialize();
    void cleanup();
    void sendClientSize();

    void requestNewWindow(const QString &directory = QString());
    void requestSplitPane(int paneId, Qt::Orientation orientation, const QString &directory = QString());
    void requestClosePane(int paneId);
    void requestCloseWindow(int windowId);
    void requestSwapPane(int srcPaneId, int dstPaneId);
    void requestMovePane(int srcPaneId, int dstPaneId, Qt::Orientation orientation, bool before);
    void requestClearHistory(Session *session);
    void requestClearHistoryAndReset(Session *session);
    void requestToggleZoomPane(int paneId);
    void requestBreakPane(int paneId);
    void requestDetach();
    void requestSelectWindow(int windowId);
    void requestSelectPane(int paneId);
    void requestSwitchSession(int sessionId);

    // Asynchronously query tmux for all sessions on the server.
    // Callback receives a list of {sessionId, sessionName, windows}
    // where each window has {windowId, windowName, panes (id+title)}.
    struct PaneDescriptor {
        int paneId = -1;
        QString title;
        bool active = false;
    };
    struct WindowDescriptor {
        int windowId = -1;
        QString name;
        bool active = false;
        QList<PaneDescriptor> panes;
    };
    struct SessionDescriptor {
        int sessionId = -1;
        QString name;
        bool active = false;
        QList<WindowDescriptor> windows;
    };
    using TreeCallback = std::function<void(QList<SessionDescriptor>)>;
    void queryTree(TreeCallback callback);

    bool hasPane(int paneId) const;
    int activePaneId() const;
    int paneIdForSession(Session *session) const;
    int windowIdForPane(int paneId) const;
    int windowCount() const;
    int paneCountForWindow(int windowId) const;
    QList<int> panesForWindow(int windowId) const;
    Session *sessionForPane(int paneId) const;
    int sessionId() const;
    QString sessionName() const;

    const QMap<int, int> &windowToTabIndex() const;

    // Hide a window on this controller's side: remove its tab and pane
    // sessions, and ignore future tmux events for it. The tmux window itself
    // is not touched. Used by detach-tab on the source MainWindow.
    void hideWindow(int windowId);

    // Undo a previous hideWindow: remove the window from the hidden set and
    // re-query tmux for its layout so the tab reappears. Used by merge-tab
    // on the target (canonical) MainWindow.
    void unhideWindow(int windowId);

    // Restrict this controller to display only the given tmux window.
    // All other windows are treated as hidden. Used by detach-tab on the
    // newly spawned MainWindow. Must be called before the controller
    // processes any events for the session attachment.
    void showOnlyWindow(int windowId);

    TmuxGateway *gateway() const;

    // Prefix key + bindings, populated asynchronously after attach. Emits
    // prefixBindingsChanged() when (re-)loaded so UI can bind the shortcut.
    struct PrefixBinding {
        QString keyToken;
        QString command;
    };
    QKeySequence prefixShortcut() const;
    const QList<PrefixBinding> &prefixBindings() const;

Q_SIGNALS:
    void initialWindowsOpened();
    void detached();
    void prefixBindingsChanged();

private Q_SLOTS:
    void onLayoutChanged(int windowId, const QString &layout, const QString &visibleLayout, bool zoomed);
    void onWindowAdded(int windowId);
    void onWindowClosed(int windowId);
    void onWindowRenamed(int windowId, const QString &name);
    void onWindowPaneChanged(int windowId, int paneId);
    void onSessionChanged(int sessionId, const QString &name);
    void onSessionWindowChanged(int sessionId, int windowId);
    void onExit(const QString &reason);

private:
    void setState(State newState);
    bool shouldSuppressResize() const;
    static bool parseListWindowsLine(const QString &line, int &windowId, QString &windowName, QString &layout);

    void applyWindowLayout(int windowId, const TmuxLayoutNode &layout);
    bool focusPane(int paneId);
    void maximizePaneInWindow(int windowId, int paneId);
    void clearMaximizeInWindow(int windowId);
    void setWindowTabTitle(int windowId, const QString &name);
    void refreshPaneTitles();
    void handleListWindowsResponse(bool success, const QString &response);
    void removeStaleWindowsAndPanes(const QSet<int> &newWindowIds, const QSet<int> &newPaneIds);
    void queryPrefixBindings();

    TmuxGateway *_gateway;
    ViewManager *_viewManager;

    TmuxPaneManager *_paneManager;
    TmuxLayoutManager *_layoutManager;
    TmuxResizeCoordinator *_resizeCoordinator;
    TmuxPaneStateRecovery *_stateRecovery;

    QMap<int, int> _windowToTabIndex;
    QMap<int, QList<int>> _windowPanes;
    QSet<int> _zoomedWindows;
    QSet<int> _hiddenWindows;
    int _restrictedWindowId = -1;

    bool shouldShowWindow(int windowId) const;

    QKeySequence _prefixShortcut;
    QList<PrefixBinding> _prefixBindings;

    QTimer *_paneTitleTimer;

    QString _sessionName;
    int _sessionId = -1;
    State _state = State::Idle;
    int _activePaneId = -1;
};

} // namespace Konsole

#endif // TMUXCONTROLLER_H
