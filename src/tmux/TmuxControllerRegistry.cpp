/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxControllerRegistry.h"

#include "TmuxController.h"
#include "session/Session.h"

namespace Konsole
{

TmuxControllerRegistry::TmuxControllerRegistry(QObject *parent)
    : QObject(parent)
{
}

TmuxControllerRegistry *TmuxControllerRegistry::instance()
{
    static TmuxControllerRegistry registry;
    return &registry;
}

void TmuxControllerRegistry::registerController(TmuxController *controller)
{
    if (!_controllers.contains(controller)) {
        _controllers.append(controller);
        Q_EMIT controllerAdded(controller);
    }
}

void TmuxControllerRegistry::unregisterController(TmuxController *controller)
{
    if (_controllers.removeOne(controller)) {
        Q_EMIT controllerRemoved(controller);
    }
}

QList<TmuxController *> TmuxControllerRegistry::controllers() const
{
    return _controllers;
}

TmuxController *TmuxControllerRegistry::controllerForSession(Session *session) const
{
    for (TmuxController *controller : _controllers) {
        if (controller->paneIdForSession(session) >= 0) {
            return controller;
        }
    }
    return nullptr;
}

TmuxController *TmuxControllerRegistry::controllerForPane(int paneId) const
{
    for (TmuxController *controller : _controllers) {
        if (controller->hasPane(paneId)) {
            return controller;
        }
    }
    return nullptr;
}

bool TmuxControllerRegistry::hasTmuxControllers() const
{
    return !_controllers.isEmpty();
}

} // namespace Konsole
