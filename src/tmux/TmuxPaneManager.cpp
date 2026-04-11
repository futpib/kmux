/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxPaneManager.h"

#include "TmuxCommand.h"
#include "TmuxGateway.h"

#include "Emulation.h"
#include "session/Session.h"
#include "profile/ProfileManager.h"
#include "session/SessionManager.h"
#include "session/VirtualSession.h"

namespace Konsole
{

TmuxPaneManager::TmuxPaneManager(TmuxGateway *gateway, QObject *parent)
    : QObject(parent)
    , _gateway(gateway)
{
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
        _gateway->sendKeys(paneId, data);
    });

    connect(session->emulation(), &Emulation::imageSizeChanged, this, [this](int, int) {
        Q_EMIT paneViewSizeChanged();
    });

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
    });

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

    if (_pausedPanes.contains(paneId)) {
        _pauseBuffers[paneId].append(data);
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
    _suppressedPanes.remove(paneId);
}

void TmuxPaneManager::pausePane(int paneId)
{
    _pausedPanes.insert(paneId);
    _gateway->sendCommand(TmuxCommand(QStringLiteral("refresh-client"))
                              .flag(QStringLiteral("-A"))
                              .singleQuotedArg(QLatin1Char('%') + QString::number(paneId) + QStringLiteral(":on")));
}

void TmuxPaneManager::continuePane(int paneId)
{
    _pausedPanes.remove(paneId);

    if (_pauseBuffers.contains(paneId)) {
        QByteArray buffered = _pauseBuffers.take(paneId);
        if (!buffered.isEmpty()) {
            auto *session = qobject_cast<VirtualSession *>(_paneToSession.value(paneId, nullptr));
            if (session) {
                session->injectData(buffered.constData(), buffered.size());
            }
        }
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
    static const QString format = QStringLiteral(
        "#{pane_id}\t#{pane_current_command}\t#{pane_current_path}\t#{pane_title}");

    _gateway->sendCommand(TmuxCommand(QStringLiteral("list-panes"))
                              .flag(QStringLiteral("-a"))
                              .format(format),
                          [this](bool success, const QString &response) {
                              if (!success || response.isEmpty()) {
                                  return;
                              }
                              const QStringList lines = response.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                              for (const QString &line : lines) {
                                  const QStringList parts = line.split(QLatin1Char('\t'));
                                  if (parts.size() < 4) {
                                      continue;
                                  }
                                  const QString &paneIdStr = parts[0];
                                  if (!paneIdStr.startsWith(QLatin1Char('%'))) {
                                      continue;
                                  }
                                  int paneId = paneIdStr.mid(1).toInt();
                                  auto *session = qobject_cast<VirtualSession *>(_paneToSession.value(paneId, nullptr));
                                  if (!session) {
                                      continue;
                                  }
                                  const QString &command = parts[1];
                                  const QString &path = parts[2];
                                  const QString &title = parts[3];
                                  if (!command.isEmpty()) {
                                      session->setExternalProcessName(command);
                                  }
                                  if (!path.isEmpty()) {
                                      session->setExternalCurrentDir(path);
                                  }
                                  if (!title.isEmpty()) {
                                      session->setExternalPaneTitle(title);
                                  }
                              }
                          });
}

} // namespace Konsole

#include "moc_TmuxPaneManager.cpp"
