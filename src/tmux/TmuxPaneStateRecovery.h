/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXPANESTATERECOVERY_H
#define TMUXPANESTATERECOVERY_H

#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>

namespace Konsole
{

class TmuxFormatSpec;
class TmuxGateway;
class TmuxPaneManager;

struct TmuxPaneState {
    int paneId = -1;
    bool alternateOn = false;
    int cursorX = 0;
    int cursorY = 0;
    int scrollRegionUpper = 0;
    int scrollRegionLower = -1; // -1 = bottom of screen
    bool cursorVisible = true;
    bool insertMode = false;
    bool appCursorKeys = false;
    bool appKeypad = false;
    bool wrapMode = true;
    bool mouseStandard = false;
    bool mouseButton = false;
    bool mouseAny = false;
    bool mouseSGR = false;
};

class TmuxPaneStateRecovery : public QObject
{
    Q_OBJECT
public:
    TmuxPaneStateRecovery(TmuxGateway *gateway, TmuxPaneManager *paneManager, QObject *parent = nullptr);

    void queryPaneStates(int windowId);
    void setPaneDimensions(int paneId, int width, int height);
    void capturePaneHistory(int paneId);
    void applyPaneState(int paneId);
    void clear();

    bool isPendingCapture(int paneId) const;

Q_SIGNALS:
    void paneRecoveryComplete(int paneId);

private:
    void handlePaneStateResponse(int windowId, bool success, const QString &response, const TmuxFormatSpec &spec);
    void handleCapturePaneResponse(int paneId, bool success, const QString &response);

    TmuxGateway *_gateway;
    TmuxPaneManager *_paneManager;
    QHash<int, TmuxPaneState> _paneStates;
    QHash<int, QPair<int, int>> _paneDimensions; // paneId → (width, height)
    QSet<int> _pendingCapture;
};

} // namespace Konsole

#endif // TMUXPANESTATERECOVERY_H
