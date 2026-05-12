/*
    SPDX-FileCopyrightText: 2006-2008 Robert Knight <robertknight@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef VIEWCONTAINER_H
#define VIEWCONTAINER_H

// Qt
#include <QObject>
#include <QPointer>
#include <QTabWidget>

// Konsole
#include "ViewManager.h"
#include "containers/ContainerInfo.h"
#include "session/Session.h"

// Qt
class QPoint;
class QToolButton;
class QMenu;
class QLabel;

namespace Konsole
{
class TabPageWidget;
class ViewProperties;
class ViewManager;
class TabbedViewContainer;

/**
 * An interface for container widgets which can hold one or more views.
 *
 * The container widget typically displays a list of the views which
 * it has and provides a means of switching between them.
 *
 * Subclasses should reimplement the addViewWidget() and removeViewWidget() functions
 * to actually add or remove view widgets from the container widget, as well
 * as updating any navigation aids.
 */
class KONSOLEPRIVATE_EXPORT TabbedViewContainer : public QTabWidget
{
    Q_OBJECT

public:
    /**
     * Constructs a new view container with the specified parent.
     *
     * @param connectedViewManager Connect the new view to this manager
     * @param parent The parent object of the container
     */
    TabbedViewContainer(ViewManager *connectedViewManager, QWidget *parent);

    /**
     * Called when the ViewContainer is destroyed.  When reimplementing this in
     * subclasses, use object->deleteLater() to delete any widgets or other objects
     * instead of 'delete object'.
     */
    ~TabbedViewContainer() override;

    /** Adds a new view to the container widget */
    void addView(TerminalDisplay *view);
    void addSplitter(ViewSplitter *viewSplitter, int index = -1);

    /** splits the currently focused Splitter */
    void splitView(TerminalDisplay *view, Qt::Orientation orientation);

    void setTabActivity(int index, bool activity);

    /** Sets tab title to item title if the view is active */
    void updateTitle(ViewProperties *item);
    /** Sets tab colors (regular and activity) to item colors */
    void updateColors(ViewProperties *item);
    /** Sets tab icon to item icon if the view is active */
    void updateIcon(ViewProperties *item);
    /** Sets tab activity status if the tab is not active */
    void updateActivity(ViewProperties *item);
    /** Sets tab notification */
    void updateNotification(ViewProperties *item, Konsole::Session::Notification notification, bool enabled);
    /** Sets tab special state (copy input or read-only) */
    void updateSpecialState(ViewProperties *item);
    /** Sets tab progress */
    void updateProgress(ViewProperties *item);

    /** Changes the active view to the next view. `reason` is the
     * Qt::FocusReason carried through to the new view's setFocus
     * via the activeViewChanged signal. Use ShortcutFocusReason for
     * user-driven tab switches (so the focus change is echoed to the
     * underlying tmux session via ViewManager::controllerChanged). */
    void activateNextView(Qt::FocusReason reason);

    /** Changes the active view to the previous view. See activateNextView
     * for the reason parameter. */
    void activatePreviousView(Qt::FocusReason reason);

    /** Changes the active view to the last view. See activateNextView
     * for the reason parameter. */
    void activateLastView(Qt::FocusReason reason);

    /** Switch to tab `index`, propagating `reason` through the
     * currentTabChanged → activeViewChanged → ViewManager::activateView
     * → TerminalDisplay::setFocus chain so it lands in
     * SessionController::viewFocused with the right intent. The 1-arg
     * QTabWidget::setCurrentIndex is hidden (= delete below) so that
     * forgetting the reason is a compile error: every tab switch in
     * kmux drives tmux's active-pane mirror, and silently picking
     * OtherFocusReason desynchronises the two. */
    void setCurrentIndex(int index, Qt::FocusReason reason);

    // Block the inherited 1-arg QTabWidget::setCurrentIndex(int) — the
    // reason matters and the compiler should refuse calls without one.
    // Qt's internal click → QTabBar::setCurrentIndex path does not go
    // through this method (it's on a different class), so this doesn't
    // break tab-bar interactions; eventFilter stamps the click reason
    // before Qt processes it.
    void setCurrentIndex(int) = delete;

    void setCssFromFile(const QUrl &url);

    ViewSplitter *activeViewSplitter();
    /**
     * This enum describes the directions
     * in which views can be re-arranged within the container
     * using the moveActiveView() method.
     */
    enum MoveDirection {
        /** Moves the view to the left. */
        MoveViewLeft,
        /** Moves the view to the right. */
        MoveViewRight,
    };

    /**
     * Moves the active view within the container and
     * updates the order in which the views are shown
     * in the container's navigation widget.
     *
     * The default implementation does nothing.
     */
    void moveActiveView(MoveDirection direction);

    /** Sets the menu to be shown when the new view button is clicked.
     * Only valid if the QuickNewView feature is enabled.
     * The default implementation does nothing. */
    // TODO: Re-enable this later.
    //    void setNewViewMenu(QMenu *menu);
    void renameTab(int index);
    void searchTabs();
    ViewManager *connectedViewManager();
    void currentTabChanged(int index);
    void closeCurrentTab();
    void wheelScrolled(int delta);
    void currentSessionControllerChanged(SessionController *controller);
    void tabDoubleClicked(int index);
    void openTabContextMenu(const QPoint &point);
    void setNavigationVisibility(ViewManager::NavigationVisibility navigationVisibility);
    void moveTabToWindow(int index, QWidget *window);

    void toggleMaximizeCurrentTerminal();
    void toggleZoomMaximizeCurrentTerminal();
    /* return the widget(int index) casted to TerminalDisplay*
     *
     * The only thing that this class holds are TerminalDisplays, so
     * this is the only thing that should be used to retrieve widgets.
     */
    ViewSplitter *viewSplitterAt(int index);

    /** Returns the tab index for the given splitter (looks through TabPageWidget wrappers). */
    int indexOfSplitter(ViewSplitter *splitter);

    /** Returns the TabPageWidget at the given tab index, or nullptr. */
    TabPageWidget *tabPageAt(int index);

    ViewSplitter *findSplitter(int id);

    /**
     * Returns the number of split views (i.e. TerminalDisplay widgets)
     * in this tab; if there are no split views, 1 is returned.
     */
    int currentTabViewCount();

    void connectTerminalDisplay(TerminalDisplay *display);
    void disconnectTerminalDisplay(TerminalDisplay *display);
    void moveTabLeft();
    void moveTabRight();

    /**
     * This enum describes where newly created tab should be placed.
     */
    enum NewTabBehavior {
        /** Put newly created tab at the end. */
        PutNewTabAtTheEnd = 0,
        /** Put newly created tab right after current tab. */
        PutNewTabAfterCurrentTab = 1,
    };

    void setNavigationBehavior(int behavior);
    void terminalDisplayDropped(TerminalDisplay *terminalDisplay);

    void moveToNewTab(TerminalDisplay *display);

    QSize sizeHint() const override;

Q_SIGNALS:
    /** Emitted when the container has no more children */
    void empty(TabbedViewContainer *container);

    /** Emitted when the user requests to open a new view */
    void newViewRequest();

    /** Requests creation of a new view, with the selected profile. */
    void newViewWithProfileRequest(const QExplicitlySharedDataPointer<Profile> &profile);

    /** Requests creation of a new view inside the given container. */
    void newViewInContainerRequest(const ContainerInfo &container);

    /** a terminalDisplay was dropped in a child Splitter */

    /**
     * Emitted when the user requests to move a view from another container
     * into this container.  If 'success' is set to true by a connected slot
     * then the original view will be removed.
     *
     * @param index Index at which to insert the new view in the container
     * or -1 to append it.  This index should be passed to addView() when
     * the new view has been created.
     * @param sessionControllerId The identifier of the view.
     */
    void moveViewRequest(int index, int sessionControllerId);

    /** Emitted when the active view changes. The reason is the focus
     * reason that should be propagated to the new view's setFocus() call,
     * so callers like ViewManager::activateView can distinguish
     * user-driven tab switches (Shift+Left, click on tab) from
     * programmatic ones (focusPane echoing a tmux notification). It is
     * stamped from TabbedViewContainer's pending-reason slot, set by
     * the navigation entry points before they call setCurrentIndex. */
    void activeViewChanged(TerminalDisplay *view, Qt::FocusReason reason);

    /** Emitted when a view is added to the container. */
    void viewAdded(TerminalDisplay *view);

    /** Emitted when a view is removed from container. */
    void viewRemoved();

    /** detach the specific tab */
    void detachTab(int tabIdx);

    /** set the color tab */
    void setColor(int index, const QColor &color);

    /** remove the color tab */
    void removeColor(int idx);

    /** set the tab progress */
    void setProgress(int idx, const std::optional<int> &progress);

    /** set the activity color tab */
    void setActivityColor(int index, const QColor &color);

protected:
    // close tabs and unregister
    void closeTerminalTab(int idx);

    void keyReleaseEvent(QKeyEvent *event) override;

    /** Watches the QTabBar for mouse-press events and stamps
     * MouseFocusReason on the pending-focus slot before Qt processes
     * the click — that way the click-driven setCurrentIndex (which
     * does not go through activate{Next,Previous,Last}View) carries
     * a user-initiated focus reason instead of the default
     * OtherFocusReason, and ViewManager::controllerChanged echoes
     * it to tmux. */
    bool eventFilter(QObject *watched, QEvent *event) override;
private Q_SLOTS:
    void viewDestroyed(QObject *view);
    void konsoleConfigChanged();
    void activateView(const QString &xdgActivationToken);
    void updateActiveContainerBadge();

private:
    void closeTmuxTab(const QList<TerminalDisplay *> &terminals);
    void forgetView();
    void ensureContainerBadge(TerminalDisplay *display);
    void removeContainerBadge(TerminalDisplay *display);
    void updateContainerBadgeForDisplay(TerminalDisplay *display);
    QColor effectiveTabColor(ViewSplitter *splitter) const;
    QColor effectiveTabActivityColor(ViewSplitter *splitter) const;

    struct TabIconState {
        TabIconState()
            : readOnly(false)
            , broadcast(false)
            , notification(Session::NoNotification)
        {
        }

        bool readOnly;
        bool broadcast;
        Session::Notification notification;

        bool isAnyStateActive() const
        {
            return readOnly || broadcast || (notification != Session::NoNotification);
        }
    };

    bool _stylesheetSet = false;

    QHash<const QWidget *, TabIconState> _tabIconState;
    // Stamped by tab-navigation entry points before they call
    // setCurrentIndex, then read and reset by currentTabChanged when it
    // emits activeViewChanged. Defaults to OtherFocusReason so any
    // setCurrentIndex caller that hasn't set it (initial wiring,
    // bookkeeping after addView, etc.) is treated as programmatic and
    // doesn't get echoed to tmux.
    Qt::FocusReason _pendingFocusReason = Qt::OtherFocusReason;
    ViewManager *_connectedViewManager;
    QMenu *_contextPopupMenu;
    QToolButton *_newTabButton;
    QToolButton *_searchTabsButton;
    QToolButton *_closeTabButton;
    // QPointer values so badge widgets clear themselves when their parent
    // wrapper is destroyed (e.g. applyLayout deletes the old splitter
    // tree). Without this, _containerBadgeWidgets still references freed
    // QWidgets after a tmux layout change.
    QHash<TerminalDisplay *, QPointer<QWidget>> _containerBadgeWidgets;
    QHash<TerminalDisplay *, QPointer<QLabel>> _containerBadgeColors;
    QHash<TerminalDisplay *, QPointer<QLabel>> _containerBadgeTexts;
    int _contextMenuTabIndex;
    NewTabBehavior _newTabBehavior;
};

}
#endif // VIEWCONTAINER_H
