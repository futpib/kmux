/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXPANEMANAGER_H
#define TMUXPANEMANAGER_H

#include <QMap>
#include <QObject>
#include <QSet>

namespace Konsole
{

class Session;
class TerminalDisplay;
class TmuxGateway;

class TmuxPaneManager : public QObject
{
    Q_OBJECT
public:
    explicit TmuxPaneManager(TmuxGateway *gateway, QObject *parent = nullptr);

    Session *createPaneSession(int paneId);
    void destroyPaneSession(int paneId);
    void destroyAllPaneSessions();

    void deliverOutput(int paneId, const QByteArray &data);

    void suppressOutput(int paneId);
    void suppressAllOutput();
    void unsuppressOutput(int paneId);

    bool hasPane(int paneId) const;
    int paneIdForSession(Session *session) const;
    int paneIdForDisplay(TerminalDisplay *display) const;
    Session *sessionForPane(int paneId) const;
    QList<int> allPaneIds() const;

    void queryPaneTitleInfo();

Q_SIGNALS:
    void paneViewSizeChanged();

private:
    TmuxGateway *_gateway;
    QMap<int, Session *> _paneToSession;
    QSet<int> _suppressedPanes;
};

} // namespace Konsole

#endif // TMUXPANEMANAGER_H
