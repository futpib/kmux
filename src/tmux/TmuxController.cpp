/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxController.h"

#include "TmuxCommand.h"
#include "TmuxGateway.h"
#include "TmuxLayoutManager.h"
#include "TmuxLayoutParser.h"
#include "TmuxPaneManager.h"
#include "TmuxPaneStateRecovery.h"
#include "TmuxResizeCoordinator.h"

#include "ViewManager.h"
#include "session/Session.h"
#include "terminalDisplay/TerminalDisplay.h"
#include "widgets/ViewContainer.h"
#include "widgets/ViewSplitter.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(KonsoleTmuxController, "konsole.tmux.controller", QtWarningMsg)

namespace Konsole
{

TmuxController::TmuxController(TmuxGateway *gateway, ViewManager *viewManager, QObject *parent)
    : QObject(parent)
    , _gateway(gateway)
    , _viewManager(viewManager)
    , _paneManager(new TmuxPaneManager(gateway, this))
    , _layoutManager(new TmuxLayoutManager(_paneManager, viewManager, this))
    , _resizeCoordinator(new TmuxResizeCoordinator(gateway, this, _paneManager, _layoutManager, viewManager, this))
    , _stateRecovery(new TmuxPaneStateRecovery(gateway, _paneManager, this))
{
    // Gateway → controller slots
    connect(_gateway, &TmuxGateway::outputReceived, _paneManager, &TmuxPaneManager::deliverOutput);
    connect(_gateway, &TmuxGateway::layoutChanged, this, &TmuxController::onLayoutChanged);
    connect(_gateway, &TmuxGateway::windowAdded, this, &TmuxController::onWindowAdded);
    connect(_gateway, &TmuxGateway::windowClosed, this, &TmuxController::onWindowClosed);
    connect(_gateway, &TmuxGateway::windowRenamed, this, &TmuxController::onWindowRenamed);
    connect(_gateway, &TmuxGateway::windowPaneChanged, this, &TmuxController::onWindowPaneChanged);
    connect(_gateway, &TmuxGateway::sessionChanged, this, &TmuxController::onSessionChanged);
    connect(_gateway, &TmuxGateway::exitReceived, this, &TmuxController::onExit);
    connect(_gateway, &TmuxGateway::panePaused, _paneManager, &TmuxPaneManager::pausePane);
    connect(_gateway, &TmuxGateway::paneContinued, _paneManager, &TmuxPaneManager::continuePane);
    // Unsuppress output when pane state recovery completes
    connect(_stateRecovery, &TmuxPaneStateRecovery::paneRecoveryComplete, _paneManager, &TmuxPaneManager::unsuppressOutput);

    // Pane view size changes → resize coordinator
    connect(_paneManager, &TmuxPaneManager::paneViewSizeChanged, this, [this]() {
        _resizeCoordinator->onPaneViewSizeChanged(shouldSuppressResize());
    });

    // Splitter drag state management
    connect(_layoutManager, &TmuxLayoutManager::splitterDragStarted, this, [this]() {
        qCDebug(KonsoleTmuxController) << "splitterDragStarted signal received";
        setState(State::Dragging);
    });
    connect(_layoutManager, &TmuxLayoutManager::splitterDragFinished, this, [this]() {
        qCDebug(KonsoleTmuxController) << "splitterDragFinished signal received";
        setState(State::Idle);
    });

    // Splitter moved → send per-pane resize commands
    connect(_layoutManager, &TmuxLayoutManager::splitterMoved, _resizeCoordinator, &TmuxResizeCoordinator::onSplitterMoved);

    // Periodic pane title refresh
    _paneTitleTimer = new QTimer(this);
    _paneTitleTimer->setInterval(2000);
    connect(_paneTitleTimer, &QTimer::timeout, this, &TmuxController::refreshPaneTitles);
}

TmuxController::~TmuxController()
{
    _paneManager->destroyAllPaneSessions();
}

void TmuxController::initialize()
{
    setState(State::Initializing);
    _gateway->sendCommand(TmuxCommand(QStringLiteral("list-windows"))
                              .format(QStringLiteral("#{window_id} #{window_name} #{window_layout}")),
                          [this](bool success, const QString &response) {
                              handleListWindowsResponse(success, response);
                          });
}

TmuxGateway *TmuxController::gateway() const
{
    return _gateway;
}

void TmuxController::requestNewWindow(const QString &directory)
{
    TmuxCommand cmd(QStringLiteral("new-window"));
    if (!directory.isEmpty()) {
        cmd.flag(QStringLiteral("-c")).singleQuotedArg(directory);
    }
    _gateway->sendCommand(cmd);
}

void TmuxController::requestSplitPane(int paneId, Qt::Orientation orientation, const QString &directory)
{
    QString direction = (orientation == Qt::Horizontal) ? QStringLiteral("-h") : QStringLiteral("-v");
    TmuxCommand cmd(QStringLiteral("split-window"));
    cmd.flag(direction).paneTarget(paneId);
    if (!directory.isEmpty()) {
        cmd.flag(QStringLiteral("-c")).singleQuotedArg(directory);
    }
    _gateway->sendCommand(cmd);
}

void TmuxController::requestClosePane(int paneId)
{
    _gateway->sendCommand(TmuxCommand(QStringLiteral("kill-pane")).paneTarget(paneId));
}

void TmuxController::requestCloseWindow(int windowId)
{
    _gateway->sendCommand(TmuxCommand(QStringLiteral("kill-window")).windowTarget(windowId));
}

void TmuxController::requestSwapPane(int srcPaneId, int dstPaneId)
{
    _gateway->sendCommand(TmuxCommand(QStringLiteral("swap-pane")).paneSource(srcPaneId).paneTarget(dstPaneId));
}

void TmuxController::requestMovePane(int srcPaneId, int dstPaneId, Qt::Orientation orientation, bool before)
{
    TmuxCommand cmd(QStringLiteral("move-pane"));
    cmd.paneSource(srcPaneId).paneTarget(dstPaneId);
    if (orientation == Qt::Horizontal) {
        cmd.flag(QStringLiteral("-h"));
    }
    if (before) {
        cmd.flag(QStringLiteral("-b"));
    }
    _gateway->sendCommand(cmd);
}

void TmuxController::requestClearHistory(Session *session)
{
    int paneId = _paneManager->paneIdForSession(session);
    if (paneId >= 0) {
        _gateway->sendCommand(TmuxCommand(QStringLiteral("clear-history")).paneTarget(paneId));
    }
}

void TmuxController::requestClearHistoryAndReset(Session *session)
{
    int paneId = _paneManager->paneIdForSession(session);
    if (paneId >= 0) {
        // send-keys -R resets terminal state and C-l clears the visible screen,
        // then clear-history removes the scrollback that was pushed off-screen.
        _gateway->sendCommand(TmuxCommand(QStringLiteral("send-keys")).flag(QStringLiteral("-R")).paneTarget(paneId).arg(QStringLiteral("C-l")));
        _gateway->sendCommand(TmuxCommand(QStringLiteral("clear-history")).paneTarget(paneId));
    }
}

void TmuxController::requestToggleZoomPane(int paneId)
{
    _gateway->sendCommand(TmuxCommand(QStringLiteral("resize-pane")).flag(QStringLiteral("-Z")).paneTarget(paneId));
}

void TmuxController::requestBreakPane(int paneId)
{
    _gateway->sendCommand(TmuxCommand(QStringLiteral("break-pane")).paneSource(paneId));
}

void TmuxController::requestDetach()
{
    _gateway->detach();
}

bool TmuxController::hasPane(int paneId) const
{
    return _paneManager->hasPane(paneId);
}

int TmuxController::activePaneId() const
{
    return _activePaneId;
}

int TmuxController::paneIdForSession(Session *session) const
{
    return _paneManager->paneIdForSession(session);
}

int TmuxController::windowIdForPane(int paneId) const
{
    for (auto it = _windowPanes.constBegin(); it != _windowPanes.constEnd(); ++it) {
        if (it.value().contains(paneId)) {
            return it.key();
        }
    }
    return -1;
}

int TmuxController::windowCount() const
{
    return _windowToTabIndex.size();
}

int TmuxController::paneCountForWindow(int windowId) const
{
    return _windowPanes.value(windowId).size();
}

const QMap<int, int> &TmuxController::windowToTabIndex() const
{
    return _windowToTabIndex;
}

void TmuxController::applyWindowLayout(int windowId, const TmuxLayoutNode &layout)
{
    // Collect pane IDs from the layout tree
    QList<int> paneIds;
    std::function<void(const TmuxLayoutNode &)> collectPanes = [&](const TmuxLayoutNode &node) {
        if (node.type == TmuxLayoutNodeType::Leaf) {
            paneIds.append(node.paneId);
        } else {
            for (const auto &child : node.children) {
                collectPanes(child);
            }
        }
    };
    collectPanes(layout);

    QList<int> oldPaneIds = _windowPanes.value(windowId);
    _windowPanes[windowId] = paneIds;

    // Ensure all pane sessions exist
    for (int paneId : paneIds) {
        _paneManager->createPaneSession(paneId);
    }

    int tabIndex = _windowToTabIndex.value(windowId, -1);
    int newTabIndex = _layoutManager->applyLayout(tabIndex, layout);
    if (newTabIndex >= 0) {
        _windowToTabIndex[windowId] = newTabIndex;
    }

    // Destroy pane sessions for panes removed from this window
    if (tabIndex >= 0) {
        for (int oldPaneId : oldPaneIds) {
            if (!paneIds.contains(oldPaneId)) {
                _paneManager->destroyPaneSession(oldPaneId);
            }
        }
    }
}

void TmuxController::setWindowTabTitle(int windowId, const QString &name)
{
    int tabIndex = _windowToTabIndex.value(windowId, -1);
    if (tabIndex < 0) {
        return;
    }
    TabbedViewContainer *container = _viewManager->activeContainer();
    if (container) {
        container->setTabText(tabIndex, name);
    }
}

void TmuxController::refreshPaneTitles()
{
    _paneManager->queryPaneTitleInfo();
}

void TmuxController::handleListWindowsResponse(bool success, const QString &response)
{
    if (!success || response.isEmpty()) {
        return;
    }

    // Helper to collect leaf pane dimensions from layout tree
    std::function<void(const TmuxLayoutNode &)> collectPaneDimensions = [&](const TmuxLayoutNode &node) {
        if (node.type == TmuxLayoutNodeType::Leaf) {
            _stateRecovery->setPaneDimensions(node.paneId, node.width, node.height);
        } else {
            for (const auto &child : node.children) {
                collectPaneDimensions(child);
            }
        }
    };

    // _state is already Initializing (which subsumes ApplyingLayout)
    const QStringList lines = response.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        int windowId;
        QString windowName;
        QString layout;
        if (!parseListWindowsLine(line, windowId, windowName, layout)) {
            continue;
        }

        auto parsed = TmuxLayoutParser::parse(layout);
        if (parsed.has_value()) {
            collectPaneDimensions(parsed.value());
            applyWindowLayout(windowId, parsed.value());
            setWindowTabTitle(windowId, windowName);
        }
    }

    // Query pane state for each window before capturing history
    for (auto it = _windowPanes.constBegin(); it != _windowPanes.constEnd(); ++it) {
        _stateRecovery->queryPaneStates(it.key());
    }

    // Suppress %output delivery and capture pane history for all panes.
    // %output arriving during capture would mix ANSI-escaped terminal output
    // with the plain-text capture-pane content, producing garbled display.
    _paneManager->suppressAllOutput();
    const auto paneIds = _paneManager->allPaneIds();
    for (int paneId : paneIds) {
        _stateRecovery->capturePaneHistory(paneId);
    }

    setState(State::Idle);
    refreshPaneTitles();
    _paneTitleTimer->start();
    Q_EMIT initialWindowsOpened();
}

void TmuxController::onLayoutChanged(int windowId, const QString &layout, const QString &visibleLayout, bool zoomed)
{
    qCDebug(KonsoleTmuxController) << "onLayoutChanged: windowId=" << windowId << "layout=" << layout << "zoomed=" << zoomed << "state=" << static_cast<int>(_state);

    auto parsed = TmuxLayoutParser::parse(layout);
    if (parsed.has_value()) {
        // Always track the actual tmux window size, even during drag —
        // onSplitterMoved needs to know the real window dimensions so
        // select-layout doesn't exceed the tmux window size.
        _resizeCoordinator->setWindowSize(windowId, parsed.value().width, parsed.value().height);
    }

    // Skip layout-change notifications while dragging a splitter — they are
    // echo-backs of our own select-layout commands and would create a feedback
    // loop (select-layout → %layout-change → applyLayout → setSizes →
    // splitterMoved → select-layout …).
    if (_state == State::Dragging) {
        qCDebug(KonsoleTmuxController) << "onLayoutChanged: SKIPPING layout apply — state is Dragging";
        return;
    }

    // Handle zoom state transitions
    if (!zoomed && _zoomedWindows.contains(windowId)) {
        // Exiting zoom — clear Konsole's maximize state before applying layout
        _zoomedWindows.remove(windowId);
        clearMaximizeInWindow(windowId);
    }

    // While zoomed, skip layout apply — the pane is maximized locally and
    // applying the full layout would re-apply setForcedSize, shrinking the
    // zoomed display back to its tmux grid size.
    if (zoomed && _zoomedWindows.contains(windowId)) {
        return;
    }

    if (parsed.has_value()) {
        setState(State::ApplyingLayout);
        applyWindowLayout(windowId, parsed.value());
        setState(State::Idle);

        // After applying a layout, (re-)focus the active pane if it belongs
        // to this window. This handles two cases:
        // 1. %window-pane-changed arrived before %layout-change — the pane
        //    session didn't exist yet, so we deferred focus.
        // 2. A subsequent %layout-change rebuilt the splitter tree, destroying
        //    the previously focused display — we need to focus the new one.
        if (_activePaneId >= 0 && _windowPanes.value(windowId).contains(_activePaneId)) {
            focusPane(_activePaneId);
        }

        // Entering zoom — the full layout was applied above (it contains all panes),
        // now maximize the zoomed pane using Konsole's local maximize.
        if (zoomed && !_zoomedWindows.contains(windowId)) {
            _zoomedWindows.insert(windowId);
            // Parse the visible layout to find which pane is zoomed
            auto visibleParsed = TmuxLayoutParser::parse(visibleLayout);
            if (visibleParsed.has_value()) {
                std::function<int(const TmuxLayoutNode &)> findLeafPane = [&](const TmuxLayoutNode &node) -> int {
                    if (node.type == TmuxLayoutNodeType::Leaf) {
                        return node.paneId;
                    }
                    for (const auto &child : node.children) {
                        int id = findLeafPane(child);
                        if (id >= 0) {
                            return id;
                        }
                    }
                    return -1;
                };
                int zoomedPaneId = findLeafPane(visibleParsed.value());
                if (zoomedPaneId >= 0) {
                    maximizePaneInWindow(windowId, zoomedPaneId);
                }
            }
        }
    }
}

void TmuxController::onWindowAdded(int windowId)
{
    if (_state == State::Initializing) {
        return;
    }
    _gateway->sendCommand(TmuxCommand(QStringLiteral("list-windows"))
                              .windowTarget(windowId)
                              .format(QStringLiteral("#{window_id} #{window_name} #{window_layout}")),
                          [this, windowId](bool success, const QString &response) {
                              if (!success || response.isEmpty()) {
                                  return;
                              }
                              // list-windows -t @<id> returns all windows in the session,
                              // so find the line matching the requested window ID.
                              QString windowIdPrefix = QStringLiteral("@") + QString::number(windowId) + QLatin1Char(' ');
                              const QStringList lines = response.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                              QString line;
                              for (const QString &l : lines) {
                                  if (l.startsWith(windowIdPrefix)) {
                                      line = l;
                                      break;
                                  }
                              }
                              if (line.isEmpty()) {
                                  return;
                              }
                              int wId;
                              QString windowName;
                              QString layout;
                              if (!parseListWindowsLine(line, wId, windowName, layout)) {
                                  return;
                              }
                              auto parsed = TmuxLayoutParser::parse(layout);
                              if (parsed.has_value()) {
                                  setState(State::ApplyingLayout);
                                  applyWindowLayout(wId, parsed.value());
                                  setWindowTabTitle(wId, windowName);
                                  setState(State::Idle);
                              }
                          });
}

void TmuxController::onWindowClosed(int windowId)
{
    // Destroy pane sessions for this window
    if (_windowPanes.contains(windowId)) {
        const QList<int> panes = _windowPanes[windowId];
        for (int paneId : panes) {
            _paneManager->destroyPaneSession(paneId);
        }
    }
    _windowToTabIndex.remove(windowId);
    _windowPanes.remove(windowId);
}

void TmuxController::onWindowRenamed(int windowId, const QString &name)
{
    setWindowTabTitle(windowId, name);
    _paneManager->queryPaneTitleInfo();
}

void TmuxController::onWindowPaneChanged(int windowId, int paneId)
{
    Q_UNUSED(windowId)
    _activePaneId = paneId;
    focusPane(paneId);
}

bool TmuxController::focusPane(int paneId)
{
    Session *session = _paneManager->sessionForPane(paneId);
    if (!session) {
        return false;
    }
    const auto displays = session->views();
    if (!displays.isEmpty()) {
        auto *display = displays.first();
        display->setFocus(Qt::OtherFocusReason);
        return true;
    }
    return false;
}

void TmuxController::maximizePaneInWindow(int windowId, int paneId)
{
    int tabIndex = _windowToTabIndex.value(windowId, -1);
    if (tabIndex < 0) {
        return;
    }
    TabbedViewContainer *container = _viewManager->activeContainer();
    if (!container) {
        return;
    }
    ViewSplitter *splitter = container->viewSplitterAt(tabIndex);
    if (!splitter) {
        return;
    }
    Session *session = _paneManager->sessionForPane(paneId);
    if (!session) {
        return;
    }
    const auto displays = session->views();
    if (displays.isEmpty()) {
        return;
    }
    splitter->setMaximizedTerminal(displays.first());
}

void TmuxController::clearMaximizeInWindow(int windowId)
{
    int tabIndex = _windowToTabIndex.value(windowId, -1);
    if (tabIndex < 0) {
        return;
    }
    TabbedViewContainer *container = _viewManager->activeContainer();
    if (!container) {
        return;
    }
    ViewSplitter *splitter = container->viewSplitterAt(tabIndex);
    if (!splitter) {
        return;
    }
    splitter->clearMaximized();
}

void TmuxController::onSessionChanged(int sessionId, const QString &name)
{
    _sessionId = sessionId;
    _sessionName = name;
    cleanup();
    initialize();
}

void TmuxController::cleanup()
{
    _paneTitleTimer->stop();
    _resizeCoordinator->stop();
    _stateRecovery->clear();
    _paneManager->destroyAllPaneSessions();
    _windowToTabIndex.clear();
    _windowPanes.clear();
    _zoomedWindows.clear();
}

void TmuxController::onExit(const QString &reason)
{
    Q_UNUSED(reason)
    cleanup();
    Q_EMIT detached();
}

void TmuxController::sendClientSize()
{
    _resizeCoordinator->sendClientSize();
}

void TmuxController::setState(State newState)
{
    static const char *stateNames[] = {"Idle", "Initializing", "ApplyingLayout", "Dragging"};
    qCDebug(KonsoleTmuxController) << "setState:" << stateNames[static_cast<int>(_state)] << "→" << stateNames[static_cast<int>(newState)];
    _state = newState;
}

bool TmuxController::shouldSuppressResize() const
{
    return _state == State::ApplyingLayout || _state == State::Initializing;
}

bool TmuxController::parseListWindowsLine(const QString &line, int &windowId, QString &windowName, QString &layout)
{
    // Format: "@<id> <window_name> <layout_checksum>,<layout_body>"
    // Window names can contain spaces, so we can't simply split on spaces.
    // The layout is always the last space-separated token (4 hex chars + comma + dimensions).
    int firstSpace = line.indexOf(QLatin1Char(' '));
    if (firstSpace < 0) {
        return false;
    }
    QString windowIdStr = line.left(firstSpace);
    if (!windowIdStr.startsWith(QLatin1Char('@'))) {
        return false;
    }
    windowId = windowIdStr.mid(1).toInt();

    int lastSpace = line.lastIndexOf(QLatin1Char(' '));
    if (lastSpace <= firstSpace) {
        return false;
    }
    layout = line.mid(lastSpace + 1);
    windowName = line.mid(firstSpace + 1, lastSpace - firstSpace - 1);
    return true;
}

} // namespace Konsole
