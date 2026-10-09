/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXPANEMANAGER_H
#define TMUXPANEMANAGER_H

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>

namespace Konsole
{

class Session;
class TerminalDisplay;
class TmuxGateway;

struct TmuxPaneModeState {
    QString name;
    int copyCursorX = 0;
    int copyCursorY = 0;
    int scrollPosition = 0;
    bool selectionPresent = false;
    bool selectionActive = false;
    int selectionStartX = 0;
    int selectionStartY = 0;
    int selectionEndX = 0;
    int selectionEndY = 0;
    QString selectionMode;
};

class TmuxPaneManager : public QObject
{
    Q_OBJECT
public:
    explicit TmuxPaneManager(TmuxGateway *gateway, QObject *parent = nullptr);

    void setGateway(TmuxGateway *gateway);

    Session *createPaneSession(int paneId);
    void destroyPaneSession(int paneId);
    void destroyAllPaneSessions();

    void deliverOutput(int paneId, const QByteArray &data);

    void suppressOutput(int paneId);
    void suppressAllOutput();
    void unsuppressOutput(int paneId);
    void completePaneRecovery(int paneId);

    bool hasPane(int paneId) const;
    int paneIdForSession(Session *session) const;
    int paneIdForDisplay(TerminalDisplay *display) const;
    Session *sessionForPane(int paneId) const;
    QList<int> allPaneIds() const;

    void queryPaneTitleInfo();
    void enterCopyMode(int paneId);
    void invalidatePaneMode(int paneId);

Q_SIGNALS:
    void paneViewSizeChanged();
    void copyModeFinished(int paneId);

private:
    void updatePaneMode(int paneId, const TmuxPaneModeState &state);
    void showPaneMode(int paneId, const QString &mode);
    void leaveCopyMode(int paneId, bool cancelServerMode);
    void requestCopyModeRecovery(int paneId);

    TmuxGateway *_gateway;
    QMap<int, Session *> _paneToSession;
    QSet<int> _suppressedPanes;
    QSet<int> _paneModeKnown;
    QMap<int, quint64> _paneModeEpoch;
    QMap<int, QString> _paneModes;
    QMap<int, TmuxPaneModeState> _paneModeStates;
    QMap<int, QList<QByteArray>> _pendingInput;
    QSet<int> _nativeCopyPanes;
    QSet<int> _serverBackedCopyPanes;
    QSet<int> _copyRecoveryPending;
};

} // namespace Konsole

#endif // TMUXPANEMANAGER_H
