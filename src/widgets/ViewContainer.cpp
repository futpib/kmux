/*
    SPDX-FileCopyrightText: 2006-2008 Robert Knight <robertknight@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "widgets/ViewContainer.h"
#include "config-konsole.h"

// Qt
#include <QBoxLayout>
#include <QFile>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QTabBar>

// KDE
#include <KActionCollection>
#include <KColorScheme>
#include <KColorUtils>
#include <KLocalizedString>

// Konsole
#include "DetachableTabBar.h"
#include "KonsoleSettings.h"
#include "ViewProperties.h"
#include "containers/ContainerList.h"
#include "containers/ContainerRegistry.h"
#include "containers/ContainerSessionState.h"
#include "containers/IContainerDetector.h"
#include "profile/ProfileList.h"
#include "searchtabs/SearchTabs.h"
#include "session/Session.h"
#include "session/SessionController.h"
#include "session/SessionManager.h"
#include "terminalDisplay/TerminalDisplay.h"
#include "tmux/TmuxController.h"
#include "tmux/TmuxControllerRegistry.h"
#include "widgets/IncrementalSearchBar.h"
#include "widgets/TabPageWidget.h"
#include "widgets/ViewSplitter.h"

#include <KMessageBox>

// TODO Perhaps move everything which is Konsole-specific into different files

using namespace Konsole;

static QString containerBadgeColorStyle(const QColor &color)
{
    return QStringLiteral("background-color: %1; border: 1px solid palette(mid); border-radius: 5px;").arg(color.name(QColor::HexRgb));
}

static QString containerBadgeBackgroundStyle(const QWidget *widget)
{
    const auto colorGroup = widget != nullptr ? widget->palette().currentColorGroup() : QPalette::Active;
    const KColorScheme viewScheme(colorGroup, KColorScheme::View);
    const QColor background = viewScheme.background(KColorScheme::AlternateBackground).color();
    return QStringLiteral("background-color: %1; border-top: 1px solid palette(mid);").arg(background.name(QColor::HexRgb));
}

static ViewSplitter *topLevelSplitterForDisplay(TerminalDisplay *display)
{
    Q_ASSERT(display != nullptr);
    auto *splitter = ViewSplitter::parentSplitterForDisplay(display);
    Q_ASSERT(splitter != nullptr);
    return splitter->getToplevelSplitter();
}

TabbedViewContainer::TabbedViewContainer(ViewManager *connectedViewManager, QWidget *parent)
    : QTabWidget(parent)
    , _connectedViewManager(connectedViewManager)
    , _newTabButton(new QToolButton(this))
    , _searchTabsButton(new QToolButton(this))
    , _closeTabButton(new QToolButton(this))
    , _contextMenuTabIndex(-1)
    , _newTabBehavior(PutNewTabAtTheEnd)
{
    setAcceptDrops(true);

    auto tabBarWidget = new DetachableTabBar(this);
    setTabBar(tabBarWidget);
    setDocumentMode(true);
    setMovable(true);
    // Watch for mouse presses so the resulting setCurrentIndex carries
    // MouseFocusReason — see eventFilter().
    tabBar()->installEventFilter(this);
    connect(tabBarWidget, &DetachableTabBar::moveTabToWindow, this, &TabbedViewContainer::moveTabToWindow);
    tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    _newTabButton->setIcon(QIcon::fromTheme(QStringLiteral("tab-new")));
    _newTabButton->setAutoRaise(true);
    _newTabButton->setToolTip(i18nc("@info:tooltip", "Open a new tab"));
    connect(_newTabButton, &QToolButton::clicked, this, &TabbedViewContainer::newViewRequest);

    _searchTabsButton->setIcon(QIcon::fromTheme(QStringLiteral("quickopen")));
    _searchTabsButton->setAutoRaise(true);
    _searchTabsButton->setToolTip(i18nc("@info:tooltip", "Search Tabs"));
    connect(_searchTabsButton, &QToolButton::clicked, this, &TabbedViewContainer::searchTabs);

    _closeTabButton->setIcon(QIcon::fromTheme(QStringLiteral("tab-close")));
    _closeTabButton->setAutoRaise(true);
    _closeTabButton->setToolTip(i18nc("@info:tooltip", "Close this tab"));
    connect(_closeTabButton, &QToolButton::clicked, this, [this] {
        closeCurrentTab();
    });

    connect(tabBar(), &QTabBar::tabBarDoubleClicked, this, &Konsole::TabbedViewContainer::tabDoubleClicked);
    connect(tabBar(), &QTabBar::customContextMenuRequested, this, &Konsole::TabbedViewContainer::openTabContextMenu);
    connect(tabBarWidget, &DetachableTabBar::detachTab, this, [this](int idx) {
        Q_EMIT detachTab(idx);
    });
    connect(tabBarWidget, &DetachableTabBar::closeTab, this, &TabbedViewContainer::closeTerminalTab);
    connect(tabBarWidget, &DetachableTabBar::newTabRequest, this, [this] {
        Q_EMIT newViewRequest();
    });
    connect(this, &TabbedViewContainer::currentChanged, this, &TabbedViewContainer::currentTabChanged);

    connect(this, &TabbedViewContainer::setColor, tabBarWidget, &DetachableTabBar::setColor);
    connect(this, &TabbedViewContainer::setActivityColor, tabBarWidget, &DetachableTabBar::setActivityColor);
    connect(this, &TabbedViewContainer::removeColor, tabBarWidget, &DetachableTabBar::removeColor);

    connect(this, &TabbedViewContainer::setProgress, tabBarWidget, &DetachableTabBar::setProgress);

    // The context menu of tab bar
    _contextPopupMenu = new QMenu(tabBar());
    connect(_contextPopupMenu, &QMenu::aboutToHide, this, [this]() {
        // Remove the read-only action when the popup closes
        for (auto &action : _contextPopupMenu->actions()) {
            if (action->objectName() == QStringLiteral("view-readonly")) {
                _contextPopupMenu->removeAction(action);
                break;
            }
        }
    });

    connect(tabBar(), &QTabBar::tabCloseRequested, this, &TabbedViewContainer::closeTerminalTab);

    auto detachAction = _contextPopupMenu->addAction(QIcon::fromTheme(QStringLiteral("tab-detach")), i18nc("@action:inmenu", "&Detach Tab"), this, [this] {
        Q_EMIT detachTab(_contextMenuTabIndex);
    });
    detachAction->setObjectName(QStringLiteral("tab-detach"));

    auto editAction =
        _contextPopupMenu->addAction(QIcon::fromTheme(QStringLiteral("edit-rename")), i18nc("@action:inmenu", "&Configure or Rename Tab..."), this, [this] {
            renameTab(_contextMenuTabIndex);
        });
    editAction->setObjectName(QStringLiteral("edit-rename"));

    auto closeAction = _contextPopupMenu->addAction(QIcon::fromTheme(QStringLiteral("tab-close")), i18nc("@action:inmenu", "Close Tab"), this, [this] {
        closeTerminalTab(_contextMenuTabIndex);
    });
    closeAction->setObjectName(QStringLiteral("tab-close"));

    auto profileMenu = new QMenu(this);
    auto profileList = new ProfileList(false, profileMenu);
    connect(profileList, &Konsole::ProfileList::profileSelected, this, &TabbedViewContainer::newViewWithProfileRequest);

    auto containerList = new Konsole::ContainerList(profileMenu);
    connect(containerList, &Konsole::ContainerList::containerSelected, this, &TabbedViewContainer::newViewInContainerRequest);

    auto rebuildProfileMenu = [profileMenu, profileList, containerList]() {
        profileMenu->clear();
        containerList->refreshContainers();

        const auto actions = profileList->actions();
        for (QAction *action : actions) {
            profileMenu->addAction(action);
        }

        containerList->addContainerSections(profileMenu);
    };

    connect(profileMenu, &QMenu::aboutToShow, profileMenu, rebuildProfileMenu);
    rebuildProfileMenu();

    _newTabButton->setMenu(profileMenu);

    konsoleConfigChanged();
    connect(KonsoleSettings::self(), &KonsoleSettings::configChanged, this, &TabbedViewContainer::konsoleConfigChanged);
    updateActiveContainerBadge();
}

TabbedViewContainer::~TabbedViewContainer()
{
    // Disconnect any remaining badge lambdas before member variables are destroyed.
    // removeContainerBadge() disconnects when a badge is removed normally, but
    // displays still in the hash at teardown need explicit disconnection here.
    // The lambda in ensureContainerBadge accesses _containerBadgeWidgets; without
    // this, it fires when QObject::~QObject() deletes children after members are
    // already gone, causing a use-after-free.
    const auto badgeDisplays = _containerBadgeWidgets.keys();
    for (TerminalDisplay *display : badgeDisplays) {
        disconnect(display, &QObject::destroyed, this, nullptr);
    }

    for (int i = 0, end = count(); i < end; i++) {
        auto view = widget(i);
        disconnect(view, &QObject::destroyed, this, &Konsole::TabbedViewContainer::viewDestroyed);
    }
}

ViewSplitter *TabbedViewContainer::activeViewSplitter()
{
    return viewSplitterAt(currentIndex());
}

ViewSplitter *TabbedViewContainer::viewSplitterAt(int index)
{
    if (auto *page = qobject_cast<TabPageWidget *>(widget(index))) {
        return page->splitter();
    }
    return qobject_cast<ViewSplitter *>(widget(index));
}

int TabbedViewContainer::indexOfSplitter(ViewSplitter *splitter)
{
    // The splitter's parent is TabPageWidget, which is the actual tab page
    if (auto *page = qobject_cast<TabPageWidget *>(splitter->parentWidget())) {
        return indexOf(page);
    }
    return indexOf(splitter);
}

TabPageWidget *TabbedViewContainer::tabPageAt(int index)
{
    return qobject_cast<TabPageWidget *>(widget(index));
}

ViewSplitter *TabbedViewContainer::findSplitter(int id)
{
    for (int i = 0; i < count(); ++i) {
        auto toplevelSplitter = viewSplitterAt(i);

        if (toplevelSplitter->id() == id)
            return toplevelSplitter;

        if (auto result = toplevelSplitter->getChildSplitter(id))
            return result;
    }

    return nullptr;
}

int TabbedViewContainer::currentTabViewCount()
{
    if (auto *splitter = activeViewSplitter()) {
        return splitter->findChildren<TerminalDisplay *>().count();
    }

    return 1;
}

void TabbedViewContainer::moveTabToWindow(int index, QWidget *window)
{
    auto splitter = viewSplitterAt(index);
    auto manager = window->findChild<ViewManager *>();

    QHash<TerminalDisplay *, Session *> sessionsMap = _connectedViewManager->forgetAll(splitter);

    const QList<TerminalDisplay *> displays = splitter->findChildren<TerminalDisplay *>();
    for (TerminalDisplay *terminal : displays) {
        manager->attachView(terminal, sessionsMap[terminal]);
    }
    auto container = manager->activeContainer();
    container->addSplitter(splitter);

    auto controller = splitter->activeTerminalDisplay()->sessionController();
    container->currentSessionControllerChanged(controller);

    forgetView();
}

void TabbedViewContainer::konsoleConfigChanged()
{
    // don't show tabs if we are in KParts mode.
    // This is a hack, and this needs to be rewritten.
    // The container should not be part of the KParts, perhaps just the
    // TerminalDisplay should.

    // ASAN issue if using sessionController->isKonsolePart(), just
    // duplicate code for now
    if (qApp->applicationName() != QLatin1String("kmux")) {
        tabBar()->setVisible(false);
    } else {
        // if we start with --show-tabbar or --hide-tabbar we ignore the preferences.
        setTabBarAutoHide(KonsoleSettings::tabBarVisibility() == KonsoleSettings::EnumTabBarVisibility::ShowTabBarWhenNeeded);
        if (KonsoleSettings::tabBarVisibility() == KonsoleSettings::EnumTabBarVisibility::AlwaysShowTabBar) {
            tabBar()->setVisible(true);
        } else if (KonsoleSettings::tabBarVisibility() == KonsoleSettings::EnumTabBarVisibility::AlwaysHideTabBar) {
            tabBar()->setVisible(false);
        }
    }

    setTabPosition((QTabWidget::TabPosition)KonsoleSettings::tabBarPosition());

    setCornerWidget(KonsoleSettings::newTabButton() ? _newTabButton : nullptr, Qt::TopLeftCorner);
    _newTabButton->setVisible(KonsoleSettings::newTabButton());

    // Add Layout for right corner tool buttons
    auto layout = new QHBoxLayout();
    layout->setStretch(0, 10);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    if (KonsoleSettings::searchTabsButton() == 0) {
        layout->addWidget(_searchTabsButton);
    }
    _searchTabsButton->setVisible(KonsoleSettings::searchTabsButton() == 0);

    if (KonsoleSettings::closeTabButton() == 1) {
        layout->addWidget(_closeTabButton);
    }
    _closeTabButton->setVisible(KonsoleSettings::closeTabButton() == 1);

    QWidget *rightCornerWidget = new QWidget();
    rightCornerWidget->setLayout(layout);
    setCornerWidget(rightCornerWidget, Qt::TopRightCorner);
    rightCornerWidget->setVisible(true);

    tabBar()->setTabsClosable(KonsoleSettings::closeTabButton() == 0);

    tabBar()->setExpanding(KonsoleSettings::expandTabWidth());
    tabBar()->update();

    if (KonsoleSettings::tabBarUseUserStyleSheet()) {
        setCssFromFile(KonsoleSettings::tabBarUserStyleSheetFile());
        _stylesheetSet = true;
    } else {
        if (_stylesheetSet) {
            setStyleSheet(QString());
            _stylesheetSet = false;
        }
    }
}

void TabbedViewContainer::setCssFromFile(const QUrl &url)
{
    // Let's only deal w/ local files for now
    if (!url.isLocalFile()) {
        setStyleSheet(KonsoleSettings::tabBarStyleSheet());
    }

    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        setStyleSheet(KonsoleSettings::tabBarStyleSheet());
    }

    QTextStream in(&file);
    setStyleSheet(in.readAll());
}

void TabbedViewContainer::moveActiveView(MoveDirection direction)
{
    if (count() < 2) { // return if only one view
        return;
    }
    const int currentIndex = indexOf(currentWidget());
    int newIndex = direction == MoveViewLeft ? qMax(currentIndex - 1, 0) : qMin(currentIndex + 1, count() - 1);

    auto swappedPage = widget(newIndex);
    auto swappedTitle = tabBar()->tabText(newIndex);
    auto swappedIcon = tabBar()->tabIcon(newIndex);

    auto currentPage = widget(currentIndex);
    auto currentTitle = tabBar()->tabText(currentIndex);
    auto currentIcon = tabBar()->tabIcon(currentIndex);

    if (newIndex < currentIndex) {
        insertTab(newIndex, currentPage, currentIcon, currentTitle);
        insertTab(currentIndex, swappedPage, swappedIcon, swappedTitle);
    } else {
        insertTab(currentIndex, swappedPage, swappedIcon, swappedTitle);
        insertTab(newIndex, currentPage, currentIcon, currentTitle);
    }
    // moveActiveView is bound to a keyboard shortcut ("Move Tab Left/Right"),
    // so the post-swap focus on the moved tab is user-initiated.
    setCurrentIndex(newIndex, Qt::ShortcutFocusReason);
}

void TabbedViewContainer::terminalDisplayDropped(TerminalDisplay *terminalDisplay)
{
    auto *controller = terminalDisplay->sessionController();
    if (controller->parent() != connectedViewManager()) {
        // Terminal from another window - recreate SessionController for current ViewManager
        disconnectTerminalDisplay(terminalDisplay);
        Session *terminalSession = controller->session();
        Q_EMIT controller->viewDragAndDropped(controller);
        connectedViewManager()->attachView(terminalDisplay, terminalSession);
        connectTerminalDisplay(terminalDisplay);
    }
}

QSize TabbedViewContainer::sizeHint() const
{
    // QTabWidget::sizeHint() contains some margins added by widgets
    // style, which were making the initial window size too big.
    const auto tabsSize = tabBar()->sizeHint();
    const auto *leftWidget = cornerWidget(Qt::TopLeftCorner);
    const auto *rightWidget = cornerWidget(Qt::TopRightCorner);
    const auto leftSize = leftWidget != nullptr ? leftWidget->sizeHint() : QSize(0, 0);
    const auto rightSize = rightWidget != nullptr ? rightWidget->sizeHint() : QSize(0, 0);

    auto tabBarSize = QSize(0, 0);
    // isVisible() won't work; this is called when the window is not yet visible
    if (tabBar()->isVisibleTo(this)) {
        tabBarSize.setWidth(leftSize.width() + tabsSize.width() + rightSize.width());
        tabBarSize.setHeight(qMax(tabsSize.height(), qMax(leftSize.height(), rightSize.height())));
    }

    const auto terminalSize = currentWidget() != nullptr ? currentWidget()->sizeHint() : QSize(0, 0);

    //        width
    // ├──────────────────┤
    //
    // ┌──────────────────┐  ┬
    // │                  │  │
    // │     Terminal     │  │
    // │                  │  │ height
    // ├───┬──────────┬───┤  │  ┬
    // │ L │   Tabs   │ R │  │  │ tab bar height
    // └───┴──────────┴───┘  ┴  ┴
    //
    // L/R = left/right widget

    return {qMax(terminalSize.width(), tabBarSize.width()), tabBarSize.height() + terminalSize.height()};
}

void TabbedViewContainer::addSplitter(ViewSplitter *viewSplitter, int index)
{
    auto *page = new TabPageWidget(viewSplitter);
    index = insertTab(index, page, QString());
    connect(page, &QObject::destroyed, this, &TabbedViewContainer::viewDestroyed);

    disconnect(viewSplitter, &ViewSplitter::terminalDisplayDropped, nullptr, nullptr);
    connect(viewSplitter, &ViewSplitter::terminalDisplayDropped, this, &TabbedViewContainer::terminalDisplayDropped);

    const auto terminalDisplays = viewSplitter->findChildren<TerminalDisplay *>();
    for (TerminalDisplay *terminal : terminalDisplays) {
        connectTerminalDisplay(terminal);
    }
    if (terminalDisplays.count() > 0) {
        updateTitle(qobject_cast<ViewProperties *>(terminalDisplays.at(0)->sessionController()));
        updateColors(qobject_cast<ViewProperties *>(terminalDisplays.at(0)->sessionController()));
    }
    // addSplitter is plumbing — a freshly-created tab being placed in
    // the container, not a user picking it. OtherFocusReason marks it
    // as programmatic so the tmux echo is suppressed.
    setCurrentIndex(index, Qt::OtherFocusReason);
}

void TabbedViewContainer::addView(TerminalDisplay *view)
{
    auto viewSplitter = new ViewSplitter();
    viewSplitter->addTerminalDisplay(view, Qt::Horizontal);
    auto item = view->sessionController();
    int index = _newTabBehavior == PutNewTabAfterCurrentTab ? currentIndex() + 1 : -1;
    auto *page = new TabPageWidget(viewSplitter);
    index = insertTab(index, page, item->icon(), item->title());

    connectTerminalDisplay(view);
    connect(page, &QObject::destroyed, this, &TabbedViewContainer::viewDestroyed);
    connect(viewSplitter, &ViewSplitter::terminalDisplayDropped, this, &TabbedViewContainer::terminalDisplayDropped);

    // Put this view on the foreground if it requests so, eg. on bell activity
    connect(view, &TerminalDisplay::activationRequest, this, &Konsole::TabbedViewContainer::activateView);

    // addView is plumbing — focusing the just-inserted tab is part of
    // building the UI, not a user gesture. OtherFocusReason keeps the
    // tmux echo path quiet for the synthesized switch.
    setCurrentIndex(index, Qt::OtherFocusReason);
    Q_EMIT viewAdded(view);
}

void TabbedViewContainer::splitView(TerminalDisplay *view, Qt::Orientation orientation)
{
    auto viewSplitter = activeViewSplitter();
    viewSplitter->clearMaximized();
    viewSplitter->addTerminalDisplay(view, orientation);
    connectTerminalDisplay(view);
    // Put this view on the foreground if it requests so, eg. on bell activity
    connect(view, &TerminalDisplay::activationRequest, this, &Konsole::TabbedViewContainer::activateView);
}

void TabbedViewContainer::connectTerminalDisplay(TerminalDisplay *display)
{
    auto item = display->sessionController();
    connect(item, &Konsole::SessionController::viewFocused, this, &Konsole::TabbedViewContainer::currentSessionControllerChanged);
    if (const auto session = item->session(); !session.isNull()) {
        connect(session, &Konsole::Session::containerContextChanged, this, &Konsole::TabbedViewContainer::updateActiveContainerBadge, Qt::UniqueConnection);
    }

    connect(item, &Konsole::ViewProperties::titleChanged, this, &Konsole::TabbedViewContainer::updateTitle);

    connect(item, &Konsole::ViewProperties::colorChanged, this, &Konsole::TabbedViewContainer::updateColors);

    connect(item, &Konsole::ViewProperties::activityColorChanged, this, &Konsole::TabbedViewContainer::updateColors);

    connect(item, &Konsole::ViewProperties::iconChanged, this, &Konsole::TabbedViewContainer::updateIcon);

    connect(item, &Konsole::ViewProperties::activity, this, &Konsole::TabbedViewContainer::updateActivity);

    connect(item, &Konsole::ViewProperties::notificationChanged, this, &Konsole::TabbedViewContainer::updateNotification);

    connect(item, &Konsole::ViewProperties::readOnlyChanged, this, &Konsole::TabbedViewContainer::updateSpecialState);

    connect(item, &Konsole::ViewProperties::copyInputChanged, this, &Konsole::TabbedViewContainer::updateSpecialState);

    connect(item, &Konsole::ViewProperties::progressChanged, this, &Konsole::TabbedViewContainer::updateProgress);
}

void TabbedViewContainer::disconnectTerminalDisplay(TerminalDisplay *display)
{
    auto item = display->sessionController();
    item->disconnect(this);
    if (const auto session = item->session(); !session.isNull()) {
        disconnect(session, &Konsole::Session::containerContextChanged, this, &Konsole::TabbedViewContainer::updateActiveContainerBadge);
    }
    removeContainerBadge(display);
}

void TabbedViewContainer::viewDestroyed(QObject *view)
{
    QWidget *w = static_cast<QWidget *>(view);
    Q_ASSERT(w);
    const int idx = indexOf(w);

    // Remove icon state keyed by this widget (cleanup any matching entry)
    _tabIconState.remove(w);

    removeTab(idx);
    forgetView();

    Q_EMIT viewRemoved();
}

void TabbedViewContainer::forgetView()
{
    if (count() == 0) {
        Q_EMIT empty(this);
    }
}

void TabbedViewContainer::activateView(const QString & /*xdgActivationToken*/)
{
    if (QWidget *widget = qobject_cast<QWidget *>(sender())) {
<<<<<<< HEAD
        auto topLevelSplitter = qobject_cast<ViewSplitter *>(widget->parentWidget())->getToplevelSplitter();
        // The tab page may be a TabPageWidget wrapping the splitter
        QWidget *tabPage = topLevelSplitter->parentWidget();
        if (tabPage) {
            setCurrentWidget(tabPage);
        }
        // XDG activation request: external app is asking us to focus this
        // terminal in response to user action elsewhere — treat as shortcut-
        // initiated so tmux's select-pane echo fires.
        widget->setFocus(Qt::ShortcutFocusReason);
=======
        auto *display = qobject_cast<TerminalDisplay *>(widget);
        auto *splitter = ViewSplitter::parentSplitterForDisplay(display);
        if (splitter == nullptr) {
            return;
        }
        auto topLevelSplitter = splitter->getToplevelSplitter();
        setCurrentWidget(topLevelSplitter);
        widget->setFocus();
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112
    }
}

void TabbedViewContainer::activateNextView(Qt::FocusReason reason)
{
    QWidget *active = currentWidget();
    int index = indexOf(active);
    setCurrentIndex(index == count() - 1 ? 0 : index + 1, reason);
}

void TabbedViewContainer::activateLastView(Qt::FocusReason reason)
{
    setCurrentIndex(count() - 1, reason);
}

void TabbedViewContainer::activatePreviousView(Qt::FocusReason reason)
{
    QWidget *active = currentWidget();
    int index = indexOf(active);
    setCurrentIndex(index == 0 ? count() - 1 : index - 1, reason);
}

void TabbedViewContainer::setCurrentIndex(int index, Qt::FocusReason reason)
{
    _pendingFocusReason = reason;
    QTabWidget::setCurrentIndex(index);
}

bool TabbedViewContainer::eventFilter(QObject *watched, QEvent *event)
{
    // Stamp MouseFocusReason for actual tab-area clicks (mouse buttons
    // 1-3 on a tab). Drag-and-drop and modifier-clicks come through
    // the same event but we want them all treated as user-initiated.
    if (watched == tabBar() && event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (tabBar()->tabAt(mouseEvent->position().toPoint()) >= 0) {
            _pendingFocusReason = Qt::MouseFocusReason;
        }
    }
    return QTabWidget::eventFilter(watched, event);
}

void TabbedViewContainer::keyReleaseEvent(QKeyEvent *event)
{
    if (event->modifiers() == Qt::NoModifier) {
        _connectedViewManager->updateTerminalDisplayHistory();
    }
}

void TabbedViewContainer::closeCurrentTab()
{
    if (currentIndex() != -1) {
        closeTerminalTab(currentIndex());
    }
}

void TabbedViewContainer::tabDoubleClicked(int index)
{
    if (index >= 0) {
        renameTab(index);
    } else {
        Q_EMIT newViewRequest();
    }
}

void TabbedViewContainer::renameTab(int index)
{
    if (index != -1) {
        // Renaming is reached via right-click → "Rename Tab", so the
        // tab the user is renaming is necessarily the one they just
        // clicked on — hence MouseFocusReason for the implied switch.
        setCurrentIndex(index, Qt::MouseFocusReason);
        viewSplitterAt(index)->activeTerminalDisplay()->sessionController()->rename();
    }
}

void TabbedViewContainer::searchTabs()
{
    /**
     * show tab search and pass focus to it
     */
    SearchTabs *searchTabs = new SearchTabs(this->connectedViewManager());
    setFocusProxy(searchTabs);
    searchTabs->raise();
    searchTabs->show();
}

void TabbedViewContainer::openTabContextMenu(const QPoint &point)
{
    if (point.isNull()) {
        return;
    }

    _contextMenuTabIndex = tabBar()->tabAt(point);
    if (_contextMenuTabIndex < 0) {
        return;
    }

    // TODO: add a countChanged signal so we can remove this for.
    // Detaching in mac causes crashes.
    const auto actions = _contextPopupMenu->actions();
    for (auto action : actions) {
        if (action->objectName() == QStringLiteral("tab-detach")) {
            action->setEnabled(count() > 1);
        }
    }

    _contextPopupMenu->exec(tabBar()->mapToGlobal(point));
}

void TabbedViewContainer::currentTabChanged(int index)
{
    // Consume the pending reason now and reset, so callers that didn't
    // set one (e.g. setCurrentIndex from addSplitter) get the default
    // "programmatic" treatment on the next tick.
    const Qt::FocusReason reason = _pendingFocusReason;
    _pendingFocusReason = Qt::OtherFocusReason;
    if (index != -1) {
        auto splitview = viewSplitterAt(index);
        if (!splitview) {
            return;
        }
        auto view = splitview->activeTerminalDisplay();
        if (view != nullptr) {
<<<<<<< HEAD
            Q_EMIT activeViewChanged(view, reason);
=======
            setTabActivity(index, false);
            _tabIconState[splitview].notification = Session::NoNotification;
            Q_EMIT activeViewChanged(view);
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112
            updateIcon(view->sessionController());
            updateActiveContainerBadge();
        }
    } else {
        deleteLater();
    }
}

void TabbedViewContainer::wheelScrolled(int delta)
{
    // Mouse-wheel over the tab bar is a user gesture, so propagate it
    // as a Mouse focus change — controllerChanged treats that as
    // user-initiated and echoes the new active pane to tmux.
    if (delta < 0) {
        activateNextView(Qt::MouseFocusReason);
    } else {
        activatePreviousView(Qt::MouseFocusReason);
    }
}

void TabbedViewContainer::setTabActivity(int index, bool activity)
{
    auto *splitter = viewSplitterAt(index);
    if (!splitter) {
        return;
    }
    auto *display = splitter->activeTerminalDisplay();
    if (!display) {
        return;
    }
    auto controller = display->sessionController();
    auto session = controller->session();
    QColor activityColor = session->activityColor();
    if (activityColor == QColor::Invalid) {
        const QPalette &palette = tabBar()->palette();
        KColorScheme colorScheme(palette.currentColorGroup());
        const QColor colorSchemeActive = colorScheme.foreground(KColorScheme::ActiveText).color();
        const QColor normalColor = palette.text().color();
        activityColor = KColorUtils::mix(normalColor, colorSchemeActive);
    }

    QColor color = activity ? activityColor : QColor();

    if (color != tabBar()->tabTextColor(index)) {
        tabBar()->setTabTextColor(index, color);
    }
}

void TabbedViewContainer::updateTitle(ViewProperties *item)
{
    auto controller = qobject_cast<SessionController *>(item);
    auto *topLevelSplitter = topLevelSplitterForDisplay(controller->view());
    if (controller->view() != topLevelSplitter->activeTerminalDisplay()) {
        return;
    }
    const int index = indexOfSplitter(topLevelSplitter);
    QString tabText = item->title();

    setTabToolTip(index, tabText);

    // To avoid having & replaced with _ (shortcut indicator)
    tabText.replace(QLatin1Char('&'), QLatin1String("&&"));
    setTabText(index, tabText);
}

void TabbedViewContainer::updateColors(ViewProperties *item)
{
    auto controller = qobject_cast<SessionController *>(item);
<<<<<<< HEAD
    auto topLevelSplitter = qobject_cast<ViewSplitter *>(controller->view()->parentWidget())->getToplevelSplitter();
    const int index = indexOfSplitter(topLevelSplitter);
=======
    auto topLevelSplitter = topLevelSplitterForDisplay(controller->view());
    const int index = indexOf(topLevelSplitter);
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112

    Q_EMIT setColor(index, effectiveTabColor(topLevelSplitter));
    Q_EMIT setActivityColor(index, effectiveTabActivityColor(topLevelSplitter));
}

QColor TabbedViewContainer::effectiveTabColor(ViewSplitter *splitter) const
{
<<<<<<< HEAD
    auto controller = qobject_cast<SessionController *>(item);
    auto topLevelSplitter = qobject_cast<ViewSplitter *>(controller->view()->parentWidget())->getToplevelSplitter();
    const int index = indexOfSplitter(topLevelSplitter);
=======
    if (splitter == nullptr) {
        return QColor();
    }
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112

    const auto displays = splitter->findChildren<TerminalDisplay *>();
    QColor chosen;
    bool hasChosen = false;
    bool mixed = false;

    for (TerminalDisplay *display : displays) {
        auto *controller = display != nullptr ? display->sessionController() : nullptr;
        auto session = controller != nullptr ? controller->session() : QPointer<Session>{};
        const QColor color = session != nullptr ? session->color() : QColor();

        if (!hasChosen) {
            chosen = color;
            hasChosen = true;
            continue;
        }

        if (chosen != color) {
            mixed = true;
            break;
        }
    }

    return mixed ? QColor() : chosen;
}

QColor TabbedViewContainer::effectiveTabActivityColor(ViewSplitter *splitter) const
{
    if (splitter == nullptr) {
        return QColor();
    }

    const auto displays = splitter->findChildren<TerminalDisplay *>();
    QColor chosen;
    bool hasChosen = false;
    bool mixed = false;

    for (TerminalDisplay *display : displays) {
        auto *controller = display != nullptr ? display->sessionController() : nullptr;
        auto session = controller != nullptr ? controller->session() : QPointer<Session>{};
        const QColor color = session != nullptr ? session->activityColor() : QColor();

        if (!hasChosen) {
            chosen = color;
            hasChosen = true;
            continue;
        }

        if (chosen != color) {
            mixed = true;
            break;
        }
    }

    return mixed ? QColor() : chosen;
}

void TabbedViewContainer::updateIcon(ViewProperties *item)
{
    auto controller = qobject_cast<SessionController *>(item);
<<<<<<< HEAD
    auto topLevelSplitter = qobject_cast<ViewSplitter *>(controller->view()->parentWidget())->getToplevelSplitter();
    const int index = indexOfSplitter(topLevelSplitter);
=======
    auto topLevelSplitter = topLevelSplitterForDisplay(controller->view());
    const int index = indexOf(topLevelSplitter);
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112
    const auto &state = _tabIconState[topLevelSplitter];

    // Tab icon priority (from highest to lowest):
    //
    // 1. Latest Notification
    //    - Inactive tab: Latest notification from any view in a tab. Removed
    //      when tab is activated.
    //    - Active tab: Latest notification from focused view. Removed when
    //      focus changes or when the Session clears its notifications
    // 2. Copy input or read-only indicator when all views in the tab have
    //    the status
    // 3. Active view icon

    QIcon icon = item->icon();
    if (state.notification != Session::NoNotification) {
        switch (state.notification) {
        case Session::Bell: {
            auto session = controller->session();
            auto profilePtr = SessionManager::instance()->sessionProfile(session);
            if (profilePtr->property<int>(Profile::BellMode) != Enum::NoBell) {
                icon = QIcon::fromTheme(QLatin1String("notifications"));
            }
        } break;
        case Session::Activity:
            icon = QIcon::fromTheme(QLatin1String("dialog-information"));
            break;
        case Session::Silence:
            icon = QIcon::fromTheme(QLatin1String("system-suspend"));
            break;
        default:
            break;
        }
    } else if (state.broadcast) {
        icon = QIcon::fromTheme(QLatin1String("irc-voice"));
    } else if (state.readOnly) {
        icon = QIcon::fromTheme(QLatin1String("object-locked"));
    }

    if (tabIcon(index).name() != icon.name()) {
        setTabIcon(index, icon);
    }
}

void TabbedViewContainer::updateActivity(ViewProperties *item)
{
    auto controller = qobject_cast<SessionController *>(item);
<<<<<<< HEAD
    if (!controller || !controller->view()) {
        return;
    }
    auto *parentSplitter = qobject_cast<ViewSplitter *>(controller->view()->parentWidget());
    if (!parentSplitter) {
        return;
    }
    auto *topLevelSplitter = parentSplitter->getToplevelSplitter();
=======
    auto topLevelSplitter = topLevelSplitterForDisplay(controller->view());
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112

    const int index = indexOfSplitter(topLevelSplitter);
    if (index != currentIndex()) {
        setTabActivity(index, true);
    }
}

void TabbedViewContainer::updateNotification(ViewProperties *item, Session::Notification notification, bool enabled)
{
    auto controller = qobject_cast<SessionController *>(item);
<<<<<<< HEAD
    auto topLevelSplitter = qobject_cast<ViewSplitter *>(controller->view()->parentWidget())->getToplevelSplitter();
    const int index = indexOfSplitter(topLevelSplitter);
=======
    auto topLevelSplitter = topLevelSplitterForDisplay(controller->view());
    const int index = indexOf(topLevelSplitter);
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112
    auto &state = _tabIconState[topLevelSplitter];

    if (enabled && (index != currentIndex() || controller->view()->hasCompositeFocus())) {
        state.notification = notification;
        updateIcon(item);
    } else if (!enabled && controller->view()->hasCompositeFocus()) {
        state.notification = Session::NoNotification;
        updateIcon(item);
    }
}

void TabbedViewContainer::updateSpecialState(ViewProperties *item)
{
    auto controller = qobject_cast<SessionController *>(item);
    auto topLevelSplitter = topLevelSplitterForDisplay(controller->view());

    auto &state = _tabIconState[topLevelSplitter];
    state.readOnly = true;
    state.broadcast = true;
    const auto displays = topLevelSplitter->findChildren<TerminalDisplay *>();
    for (const auto display : displays) {
        if (!display->sessionController()->isReadOnly()) {
            state.readOnly = false;
        }
        if (!display->sessionController()->isCopyInputActive()) {
            state.broadcast = false;
        }
    }
    updateIcon(item);
}

void TabbedViewContainer::updateProgress(ViewProperties *item)
{
    auto controller = qobject_cast<SessionController *>(item);
<<<<<<< HEAD
    auto topLevelSplitter = qobject_cast<ViewSplitter *>(controller->view()->parentWidget())->getToplevelSplitter();
    const int index = indexOfSplitter(topLevelSplitter);
=======
    auto topLevelSplitter = topLevelSplitterForDisplay(controller->view());
    const int index = indexOf(topLevelSplitter);
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112

    Q_EMIT setProgress(index, item->progress());
}

void TabbedViewContainer::currentSessionControllerChanged(SessionController *controller)
{
<<<<<<< HEAD
    auto topLevelSplitter = qobject_cast<ViewSplitter *>(controller->view()->parentWidget())->getToplevelSplitter();
    const int index = indexOfSplitter(topLevelSplitter);
=======
    auto topLevelSplitter = topLevelSplitterForDisplay(controller->view());
    const int index = indexOf(topLevelSplitter);
>>>>>>> b1dba85d5d5ee45c8ee99f971d621963ad8c9112

    if (index == currentIndex()) {
        // Active view changed in current tab - clear notifications
        auto &state = _tabIconState[topLevelSplitter];
        state.notification = Session::NoNotification;
    }

    updateTitle(qobject_cast<ViewProperties *>(controller));
    updateColors(qobject_cast<ViewProperties *>(controller));
    updateActivity(qobject_cast<ViewProperties *>(controller));
    updateSpecialState(qobject_cast<ViewProperties *>(controller));
    updateActiveContainerBadge();
}

void TabbedViewContainer::updateActiveContainerBadge()
{
    auto *splitter = activeViewSplitter();
    if (splitter == nullptr) {
        return;
    }

    const auto displays = splitter->findChildren<TerminalDisplay *>();
    for (TerminalDisplay *display : displays) {
        updateContainerBadgeForDisplay(display);
    }

    const auto knownDisplays = _containerBadgeWidgets.keys();
    for (TerminalDisplay *display : knownDisplays) {
        if (!displays.contains(display)) {
            removeContainerBadge(display);
        }
    }
}

void TabbedViewContainer::ensureContainerBadge(TerminalDisplay *display)
{
    if (display == nullptr || _containerBadgeWidgets.contains(display)) {
        return;
    }

    auto *parentWidget = ViewSplitter::containerWidgetForDisplay(display);
    if (parentWidget == nullptr) {
        parentWidget = display;
    }
    auto *badgeWidget = new QWidget(parentWidget);
    auto *badgeColor = new QLabel(badgeWidget);
    auto *badgeText = new QLabel(badgeWidget);
    auto *badgeLayout = new QHBoxLayout(badgeWidget);
    badgeLayout->setContentsMargins(8, 0, 6, 0);
    badgeLayout->setSpacing(6);

    badgeColor->setFixedSize(10, 10);
    badgeColor->setAlignment(Qt::AlignCenter);
    badgeColor->setStyleSheet(QStringLiteral("border: 1px solid palette(mid); border-radius: 5px;"));
    badgeText->setTextFormat(Qt::PlainText);
    badgeText->setStyleSheet(QStringLiteral("font-weight: 600;"));
    badgeLayout->addWidget(badgeColor);
    badgeLayout->addWidget(badgeText);

    badgeWidget->setStyleSheet(containerBadgeBackgroundStyle(badgeWidget));
    badgeWidget->setVisible(false);

    _containerBadgeWidgets.insert(display, badgeWidget);
    _containerBadgeColors.insert(display, badgeColor);
    _containerBadgeTexts.insert(display, badgeText);

    if (auto *layout = qobject_cast<QBoxLayout *>(parentWidget->layout())) {
        layout->addWidget(badgeWidget);
    }

    connect(display, &QObject::destroyed, this, [this, display]() {
        removeContainerBadge(display);
    });
}

void TabbedViewContainer::removeContainerBadge(TerminalDisplay *display)
{
    if (display == nullptr) {
        return;
    }

    disconnect(display, &QObject::destroyed, this, nullptr);

    if (auto *badgeWidget = _containerBadgeWidgets.take(display)) {
        badgeWidget->deleteLater();
    }

    _containerBadgeColors.remove(display);
    _containerBadgeTexts.remove(display);
}

void TabbedViewContainer::updateContainerBadgeForDisplay(TerminalDisplay *display)
{
    if (display == nullptr) {
        return;
    }

    ensureContainerBadge(display);

    auto *badgeWidget = _containerBadgeWidgets.value(display, nullptr);
    auto *badgeColor = _containerBadgeColors.value(display, nullptr);
    auto *badgeText = _containerBadgeTexts.value(display, nullptr);
    if (badgeWidget == nullptr || badgeColor == nullptr || badgeText == nullptr) {
        return;
    }

    const auto controller = display->sessionController();
    const auto session = controller != nullptr ? controller->session() : QPointer<Session>{};
    if (session.isNull()) {
        badgeWidget->setVisible(false);
        return;
    }

    const ContainerInfo container = session->containerContext();
    QString containerName;
    QString containerType;
    QString containerKey;
    if (container.isValid()) {
        containerName = container.displayName.isEmpty() ? container.name : container.displayName;
        containerType = container.detector != nullptr ? container.detector->displayName() : i18n("Container");
        containerKey = ContainerRegistry::keyFromContainerInfo(container);
    } else {
        const auto pending = ContainerSessionState::pendingContainerInfo(session);
        if (pending.isActive()) {
            containerName = pending.name;
            containerType = pending.type;
            containerKey = pending.key;
            if (containerType.isEmpty()) {
                containerType = i18n("Container");
            }
        }
    }

    if (containerName.isEmpty()) {
        badgeWidget->setVisible(false);
        return;
    }

    badgeText->setText(i18n("%1: %2", containerType, containerName));
    const QString colorKey = !containerKey.isEmpty() ? containerKey : containerName;
    badgeColor->setStyleSheet(containerBadgeColorStyle(ContainerSessionState::colorForContainerKey(colorKey)));
    const int h = qMax(22, badgeWidget->sizeHint().height() + 4);
    badgeWidget->setStyleSheet(containerBadgeBackgroundStyle(badgeWidget));
    badgeWidget->setFixedHeight(h);

    badgeWidget->setVisible(true);
}

void TabbedViewContainer::closeTerminalTab(int idx)
{
    Q_EMIT removeColor(idx);

    // Check if this is a tmux tab by looking at the first terminal's session
    const auto terminals = viewSplitterAt(idx)->findChildren<TerminalDisplay *>();
    if (!terminals.isEmpty()) {
        auto firstSession = terminals.first()->sessionController()->session();
        if (firstSession && firstSession->paneSyncPolicy() == Session::PaneSyncPolicy::SyncWithSiblings) {
            closeTmuxTab(terminals);
            return;
        }
    }

    // Normal (non-tmux) close: close each session individually
    for (auto terminal : terminals) {
        terminal->sessionController()->closeSession();
    }
}

void TabbedViewContainer::closeTmuxTab(const QList<TerminalDisplay *> &terminals)
{
    // Find the tmux controller and window ID from the first pane
    auto firstSession = terminals.first()->sessionController()->session();
    TmuxController *controller = nullptr;
    int paneId = -1;
    int windowId = -1;

    controller = TmuxControllerRegistry::instance()->controllerForSession(firstSession);
    if (controller) {
        paneId = controller->paneIdForSession(firstSession);
        windowId = controller->windowIdForPane(paneId);
    }

    if (!controller || windowId < 0) {
        return;
    }

    int result = KMessageBox::warningTwoActions(window(),
                                                i18n("Close this tab? The processes running in it will be terminated."),
                                                i18n("Confirm Close"),
                                                KGuiItem(i18nc("@action:button", "Close Tab"), QStringLiteral("application-exit")),
                                                KStandardGuiItem::cancel(),
                                                QStringLiteral("ConfirmCloseTmuxWindow"));

    if (result == KMessageBox::PrimaryAction) {
        controller->requestCloseWindow(windowId);
    }
}

ViewManager *TabbedViewContainer::connectedViewManager()
{
    return _connectedViewManager;
}

void TabbedViewContainer::setNavigationVisibility(ViewManager::NavigationVisibility navigationVisibility)
{
    if (navigationVisibility == ViewManager::NavigationNotSet) {
        return;
    }

    setTabBarAutoHide(navigationVisibility == ViewManager::ShowNavigationAsNeeded);
    if (navigationVisibility == ViewManager::AlwaysShowNavigation) {
        tabBar()->setVisible(true);
    } else if (navigationVisibility == ViewManager::AlwaysHideNavigation) {
        tabBar()->setVisible(false);
    }
}

void TabbedViewContainer::toggleMaximizeCurrentTerminal()
{
    if (auto *terminal = qobject_cast<TerminalDisplay *>(sender())) {
        terminal->setFocus(Qt::FocusReason::OtherFocusReason);
    }

    auto *display = activeViewSplitter()->activeTerminalDisplay();
    if (display && display->sessionController()) {
        auto *controller = TmuxControllerRegistry::instance()->controllerForSession(display->sessionController()->session());
        if (controller) {
            int paneId = controller->paneIdForSession(display->sessionController()->session());
            if (paneId >= 0) {
                controller->requestToggleZoomPane(paneId);
                return;
            }
        }
    }

    activeViewSplitter()->toggleMaximizeCurrentTerminal();
}


void TabbedViewContainer::toggleZoomMaximizeCurrentTerminal()
{
    if (auto *terminal = qobject_cast<TerminalDisplay *>(sender())) {
        terminal->setFocus(Qt::FocusReason::OtherFocusReason);
    }

    auto *display = activeViewSplitter()->activeTerminalDisplay();
    if (display && display->sessionController()) {
        auto *controller = TmuxControllerRegistry::instance()->controllerForSession(display->sessionController()->session());
        if (controller) {
            int paneId = controller->paneIdForSession(display->sessionController()->session());
            if (paneId >= 0) {
                controller->requestToggleZoomPane(paneId);
                return;
            }
        }
    }

    activeViewSplitter()->toggleZoomMaximizeCurrentTerminal();
}

void TabbedViewContainer::moveTabLeft()
{
    if (currentIndex() == 0) {
        return;
    }
    tabBar()->moveTab(currentIndex(), currentIndex() - 1);
}

void TabbedViewContainer::moveTabRight()
{
    if (currentIndex() == count() - 1) {
        return;
    }
    tabBar()->moveTab(currentIndex(), currentIndex() + 1);
}

void TabbedViewContainer::setNavigationBehavior(int behavior)
{
    _newTabBehavior = static_cast<NewTabBehavior>(behavior);
}

void TabbedViewContainer::moveToNewTab(TerminalDisplay *display)
{
    // For tmux panes, break-pane moves the pane to a new tmux window (new tab)
    Session *session = display->sessionController() ? display->sessionController()->session().data() : nullptr;
    if (session && session->paneSyncPolicy() == Session::PaneSyncPolicy::SyncWithSiblings) {
        auto *ctrl = TmuxControllerRegistry::instance()->controllerForSession(session);
        if (ctrl) {
            ctrl->requestBreakPane(ctrl->paneIdForSession(session));
            return;
        }
    }

    // Ensure that the current terminal is not maximized so that the other views will be shown properly
    activeViewSplitter()->clearMaximized();
    addView(display);
}

#include "moc_ViewContainer.cpp"
