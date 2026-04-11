/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXCONTROLLERREGISTRY_H
#define TMUXCONTROLLERREGISTRY_H

#include <QList>
#include <QObject>

#include "konsoleprivate_export.h"

namespace Konsole
{

class Session;
class TmuxController;

class KONSOLEPRIVATE_EXPORT TmuxControllerRegistry : public QObject
{
    Q_OBJECT
public:
    static TmuxControllerRegistry *instance();

    void registerController(TmuxController *controller);
    void unregisterController(TmuxController *controller);

    QList<TmuxController *> controllers() const;

    TmuxController *controllerForSession(Session *session) const;
    TmuxController *controllerForPane(int paneId) const;

    bool hasTmuxControllers() const;

Q_SIGNALS:
    void controllerAdded(TmuxController *controller);
    void controllerRemoved(TmuxController *controller);

private:
    explicit TmuxControllerRegistry(QObject *parent = nullptr);

    QList<TmuxController *> _controllers;
};

} // namespace Konsole

#endif // TMUXCONTROLLERREGISTRY_H
