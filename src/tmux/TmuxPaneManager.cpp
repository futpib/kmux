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
    });

    _gateway->sendCommand(TmuxCommand(QStringLiteral("list-panes")).allSessions().format(spec), [this, spec](bool success, const QString &response) {
        if (!success || response.isEmpty()) {
            return;
        }
        for (const auto &row : spec.parseRows(response)) {
            const QString paneIdStr = row.value(QStringLiteral("pane_id"));
            if (!paneIdStr.startsWith(QLatin1Char('%'))) {
                continue;
            }
            int paneId = paneIdStr.mid(1).toInt();
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
        }
    });
}

} // namespace Konsole

#include "moc_TmuxPaneManager.cpp"
