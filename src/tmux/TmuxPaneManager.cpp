/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxPaneManager.h"

#include "TmuxCommand.h"
#include "TmuxGateway.h"
#include "TmuxPaneOptions.h"

#include "Emulation.h"
#include "Vt102Emulation.h"
#include "profile/ProfileManager.h"
#include "session/Session.h"
#include "session/SessionManager.h"
#include "session/VirtualSession.h"
#include "terminalDisplay/TerminalDisplay.h"

#include <utility>

namespace Konsole
{

TmuxPaneManager::TmuxPaneManager(TmuxGateway *gateway, QObject *parent)
    : QObject(parent)
    , _gateway(gateway)
{
}

void TmuxPaneManager::setGateway(TmuxGateway *gateway)
{
    _gateway = gateway;
    for (auto it = _paneToSession.constBegin(); it != _paneToSession.constEnd(); ++it) {
        ++_paneModeEpoch[it.key()];
    }
    _paneModeKnown.clear();
    _paneModes.clear();
    _paneModeStates.clear();
    _pendingInput.clear();
    _serverBackedCopyPanes.clear();
    _copyRecoveryPending.clear();
    for (auto it = _paneToSession.constBegin(); it != _paneToSession.constEnd(); ++it) {
        if (!_nativeCopyPanes.contains(it.key())) {
            showPaneMode(it.key(), QString());
        }
    }
}

Session *TmuxPaneManager::createPaneSession(int paneId)
{
    if (_paneToSession.contains(paneId)) {
        return _paneToSession[paneId];
    }

    VirtualSession *session = SessionManager::instance()->createVirtualSession(
        ProfileManager::instance()->defaultProfile());
    session->setPaneSyncPolicy(Session::PaneSyncPolicy::SyncWithSiblings);
    session->emulation()->setSuppressTerminalResponsesDuringReceive(true);

    connect(session->emulation(), &Emulation::sendData, this, [this, paneId](const QByteArray &data) {
        if (!_paneModeKnown.contains(paneId)) {
            auto &pending = _pendingInput[paneId];
            qsizetype pendingSize = 0;
            for (const QByteArray &chunk : std::as_const(pending)) {
                pendingSize += chunk.size();
            }
            if (pendingSize + data.size() <= 64 * 1024) {
                pending.append(data);
            }
            return;
        }
        const QString mode = _paneModes.value(paneId);
        if (!mode.isEmpty()) {
            if (data == QByteArray(1, '\x1b')) {
                cancelPaneMode(paneId, mode);
            }
            return;
        }
        _gateway->sendKeys(paneId, data);
    });

    connect(session, &Session::selectModeChanged, this, [this, paneId](bool enabled) {
        if (!enabled && _nativeCopyPanes.contains(paneId)) {
            leaveCopyMode(paneId, _serverBackedCopyPanes.contains(paneId));
        }
    });

    connect(session->emulation(), &Emulation::imageSizeChanged, this, [this](int, int) {
        Q_EMIT paneViewSizeChanged();
    });

    if (auto *vtEmulation = qobject_cast<Vt102Emulation *>(session->emulation())) {
        connect(vtEmulation, &Vt102Emulation::kittyKeyboardStateChanged, this, [this, paneId](const QString &state) {
            TmuxCommand command(QStringLiteral("set-option"));
            command.flag(QStringLiteral("-p")).paneTarget(paneId);
            if (state.isEmpty()) {
                command.flag(QStringLiteral("-u")).arg(QLatin1String(TmuxKittyKeyboardStateOption));
            } else {
                command.arg(QLatin1String(TmuxKittyKeyboardStateOption)).singleQuotedArg(state);
            }
            _gateway->sendCommand(command);
        });
    }

    connect(session, &Session::finished, this, [this, paneId]() {
        // If the pane is still tracked, the close was initiated by the user
        // (not by tmux), so we need to tell tmux to kill the pane.
        // When tmux initiates the close, destroyPaneSession() removes the
        // pane from _paneToSession before calling session->close().
        if (_paneToSession.contains(paneId)) {
            _gateway->sendCommand(TmuxCommand(QStringLiteral("kill-pane")).paneTarget(paneId));
        }
    });

    connect(session, &QObject::destroyed, this, [this, paneId]() {
        _paneToSession.remove(paneId);
        _paneModeKnown.remove(paneId);
        _paneModeEpoch.remove(paneId);
        _paneModes.remove(paneId);
        _paneModeStates.remove(paneId);
        _pendingInput.remove(paneId);
        _nativeCopyPanes.remove(paneId);
        _serverBackedCopyPanes.remove(paneId);
        _copyRecoveryPending.remove(paneId);
        _suppressedPanes.remove(paneId);
    });

    _paneModeEpoch.insert(paneId, 0);
    _paneToSession[paneId] = session;
    return session;
}

void TmuxPaneManager::destroyPaneSession(int paneId)
{
    auto it = _paneToSession.find(paneId);
    if (it != _paneToSession.end()) {
        Session *session = it.value();
        _paneToSession.erase(it);
        session->close();
    }
}

void TmuxPaneManager::destroyAllPaneSessions()
{
    const auto paneIds = _paneToSession.keys();
    for (int paneId : paneIds) {
        destroyPaneSession(paneId);
    }
}

void TmuxPaneManager::deliverOutput(int paneId, const QByteArray &data)
{
    if (_suppressedPanes.contains(paneId)) {
        return;
    }

    auto *session = qobject_cast<VirtualSession *>(_paneToSession.value(paneId, nullptr));
    if (session) {
        session->injectData(data.constData(), data.size());
    }
}

void TmuxPaneManager::suppressOutput(int paneId)
{
    _suppressedPanes.insert(paneId);
}

void TmuxPaneManager::suppressAllOutput()
{
    for (auto it = _paneToSession.constBegin(); it != _paneToSession.constEnd(); ++it) {
        _suppressedPanes.insert(it.key());
    }
}

void TmuxPaneManager::unsuppressOutput(int paneId)
{
    if (!_nativeCopyPanes.contains(paneId)) {
        _suppressedPanes.remove(paneId);
    }
}

void TmuxPaneManager::completePaneRecovery(int paneId)
{
    unsuppressOutput(paneId);
    if (!_serverBackedCopyPanes.contains(paneId) || !_paneModeStates.contains(paneId)) {
        return;
    }

    Session *session = _paneToSession.value(paneId, nullptr);
    if (!session) {
        return;
    }
    const TmuxPaneModeState &state = _paneModeStates[paneId];
    for (TerminalDisplay *display : session->views()) {
        display->setTmuxCopyModeState(state.copyCursorX,
                                      state.copyCursorY,
                                      state.scrollPosition,
                                      state.selectionPresent,
                                      state.selectionActive,
                                      state.selectionStartX,
                                      state.selectionStartY,
                                      state.selectionEndX,
                                      state.selectionEndY,
                                      state.selectionMode);
    }
}

bool TmuxPaneManager::hasPane(int paneId) const
{
    return _paneToSession.contains(paneId);
}

int TmuxPaneManager::paneIdForSession(Session *session) const
{
    for (auto it = _paneToSession.constBegin(); it != _paneToSession.constEnd(); ++it) {
        if (it.value() == session) {
            return it.key();
        }
    }
    return -1;
}

int TmuxPaneManager::paneIdForDisplay(TerminalDisplay *display) const
{
    for (auto it = _paneToSession.constBegin(); it != _paneToSession.constEnd(); ++it) {
        if (it.value()->views().contains(display)) {
            return it.key();
        }
    }
    return -1;
}

Session *TmuxPaneManager::sessionForPane(int paneId) const
{
    return _paneToSession.value(paneId, nullptr);
}

QList<int> TmuxPaneManager::allPaneIds() const
{
    return _paneToSession.keys();
}

void TmuxPaneManager::queryPaneTitleInfo()
{
    TmuxFormatSpec spec({
        QStringLiteral("pane_id"),
        QStringLiteral("pane_current_command"),
        QStringLiteral("pane_current_path"),
        QStringLiteral("pane_title"),
        QStringLiteral("pane_pid"),
        QStringLiteral("pane_mode"),
        QStringLiteral("copy_cursor_x"),
        QStringLiteral("copy_cursor_y"),
        QStringLiteral("scroll_position"),
        QStringLiteral("selection_present"),
        QStringLiteral("selection_active"),
        QStringLiteral("selection_start_x"),
        QStringLiteral("selection_start_y"),
        QStringLiteral("selection_end_x"),
        QStringLiteral("selection_end_y"),
        QStringLiteral("selection_mode"),
    });

    const QMap<int, quint64> requestedEpochs = _paneModeEpoch;
    _gateway->sendCommand(TmuxCommand(QStringLiteral("list-panes")).allSessions().format(spec),
                          [this, spec, requestedEpochs](bool success, const QString &response) {
                              if (!success || response.isEmpty()) {
                                  return;
                              }
                              for (const auto &row : spec.parseRows(response)) {
                                  const QString paneIdStr = row.value(QStringLiteral("pane_id"));
                                  if (!paneIdStr.startsWith(QLatin1Char('%'))) {
                                      continue;
                                  }
                                  int paneId = paneIdStr.mid(1).toInt();
                                  if (!requestedEpochs.contains(paneId) || requestedEpochs.value(paneId) != _paneModeEpoch.value(paneId)) {
                                      continue;
                                  }
                                  auto *session = qobject_cast<VirtualSession *>(_paneToSession.value(paneId, nullptr));
                                  if (!session) {
                                      continue;
                                  }
                                  const QString command = row.value(QStringLiteral("pane_current_command"));
                                  const QString path = row.value(QStringLiteral("pane_current_path"));
                                  const QString title = row.value(QStringLiteral("pane_title"));
                                  bool pidOk = false;
                                  const int panePid = row.value(QStringLiteral("pane_pid")).toInt(&pidOk);
                                  if (pidOk && panePid > 0) {
                                      session->setExternalPid(panePid);
                                  }
                                  if (!command.isEmpty()) {
                                      session->setExternalProcessName(command);
                                  }
                                  if (!path.isEmpty()) {
                                      session->setExternalCurrentDir(path);
                                  }
                                  if (!title.isEmpty()) {
                                      session->setExternalPaneTitle(title);
                                  }

                                  TmuxPaneModeState modeState;
                                  modeState.name = row.value(QStringLiteral("pane_mode"));
                                  modeState.copyCursorX = row.value(QStringLiteral("copy_cursor_x")).toInt();
                                  modeState.copyCursorY = row.value(QStringLiteral("copy_cursor_y")).toInt();
                                  modeState.scrollPosition = row.value(QStringLiteral("scroll_position")).toInt();
                                  modeState.selectionPresent = row.value(QStringLiteral("selection_present")) == QLatin1String("1");
                                  modeState.selectionActive = row.value(QStringLiteral("selection_active")) == QLatin1String("1");
                                  modeState.selectionStartX = row.value(QStringLiteral("selection_start_x")).toInt();
                                  modeState.selectionStartY = row.value(QStringLiteral("selection_start_y")).toInt();
                                  modeState.selectionEndX = row.value(QStringLiteral("selection_end_x")).toInt();
                                  modeState.selectionEndY = row.value(QStringLiteral("selection_end_y")).toInt();
                                  modeState.selectionMode = row.value(QStringLiteral("selection_mode"));
                                  updatePaneMode(paneId, modeState);
                              }
                          });
}

void TmuxPaneManager::enterCopyMode(int paneId)
{
    Session *session = _paneToSession.value(paneId, nullptr);
    if (!session || _nativeCopyPanes.contains(paneId)) {
        return;
    }

    _copyRecoveryPending.remove(paneId);
    _nativeCopyPanes.insert(paneId);
    suppressOutput(paneId);
    showPaneMode(paneId, QStringLiteral("copy-mode"));
    session->setSelectMode(true);
}

void TmuxPaneManager::invalidatePaneMode(int paneId)
{
    if (_paneToSession.contains(paneId)) {
        ++_paneModeEpoch[paneId];
        _paneModeKnown.remove(paneId);
    }
}

void TmuxPaneManager::cancelPaneMode(int paneId, const QString &mode)
{
    if (mode == QLatin1String("copy-mode")) {
        _gateway->sendCommand(TmuxCommand(QStringLiteral("send-keys")).flag(QStringLiteral("-X")).paneTarget(paneId).arg(QStringLiteral("cancel")));
        return;
    }

    // Tree mode handles keys but does not expose a -X command. Check the
    // server's current mode so a delayed Escape cannot reach the pane program.
    const QString escape = TmuxCommand(QStringLiteral("send-keys")).paneTarget(paneId).arg(QStringLiteral("Escape")).build();
    _gateway->sendCommand(TmuxCommand(QStringLiteral("if-shell"))
                              .flag(QStringLiteral("-F"))
                              .paneTarget(paneId)
                              .singleQuotedArg(QStringLiteral("#{pane_in_mode}"))
                              .singleQuotedArg(escape));
}

void TmuxPaneManager::updatePaneMode(int paneId, const TmuxPaneModeState &state)
{
    Session *session = _paneToSession.value(paneId, nullptr);
    if (!session) {
        return;
    }

    _paneModeKnown.insert(paneId);
    _paneModes[paneId] = state.name;
    _paneModeStates[paneId] = state;

    const QList<QByteArray> pendingInput = _pendingInput.take(paneId);
    if (state.name.isEmpty()) {
        for (const QByteArray &data : pendingInput) {
            _gateway->sendKeys(paneId, data);
        }
    } else {
        for (const QByteArray &data : pendingInput) {
            if (data == QByteArray(1, '\x1b')) {
                cancelPaneMode(paneId, state.name);
                break;
            }
        }
    }

    if (state.name == QLatin1String("copy-mode")) {
        const bool entering = !_nativeCopyPanes.contains(paneId);
        _serverBackedCopyPanes.insert(paneId);
        if (entering) {
            enterCopyMode(paneId);
            for (TerminalDisplay *display : session->views()) {
                display->setTmuxCopyModeState(state.copyCursorX,
                                              state.copyCursorY,
                                              state.scrollPosition,
                                              state.selectionPresent,
                                              state.selectionActive,
                                              state.selectionStartX,
                                              state.selectionStartY,
                                              state.selectionEndX,
                                              state.selectionEndY,
                                              state.selectionMode);
            }
        }
        return;
    }

    if (_serverBackedCopyPanes.contains(paneId)) {
        _serverBackedCopyPanes.remove(paneId);
        if (_nativeCopyPanes.remove(paneId)) {
            session->setSelectMode(false);
            requestCopyModeRecovery(paneId);
        }
    }

    if (!_nativeCopyPanes.contains(paneId)) {
        showPaneMode(paneId, state.name);
    }
}

void TmuxPaneManager::showPaneMode(int paneId, const QString &mode)
{
    Session *session = _paneToSession.value(paneId, nullptr);
    if (!session) {
        return;
    }
    for (TerminalDisplay *display : session->views()) {
        display->setTmuxMode(mode);
    }
}

void TmuxPaneManager::leaveCopyMode(int paneId, bool cancelServerMode)
{
    if (!_nativeCopyPanes.remove(paneId)) {
        return;
    }

    if (!cancelServerMode) {
        showPaneMode(paneId, _paneModes.value(paneId));
        requestCopyModeRecovery(paneId);
        return;
    }

    _serverBackedCopyPanes.remove(paneId);
    _gateway->sendCommand(TmuxCommand(QStringLiteral("send-keys")).flag(QStringLiteral("-X")).paneTarget(paneId).arg(QStringLiteral("cancel")),
                          [this, paneId](bool, const QString &) {
                              requestCopyModeRecovery(paneId);
                          });
}

void TmuxPaneManager::requestCopyModeRecovery(int paneId)
{
    if (_copyRecoveryPending.contains(paneId)) {
        return;
    }
    _copyRecoveryPending.insert(paneId);
    Q_EMIT copyModeFinished(paneId);
}

} // namespace Konsole

#include "moc_TmuxPaneManager.cpp"
