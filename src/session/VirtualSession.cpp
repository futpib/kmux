/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "VirtualSession.h"

#include "Emulation.h"
#include "NullProcessInfo.h"

namespace Konsole
{

VirtualSession::VirtualSession(QObject *parent)
    : Session(NoPtyTag{}, parent)
{
}

void VirtualSession::injectData(const char *data, int length)
{
    if (_emulation) {
        _emulation->receiveData(data, length);
    }
}

void VirtualSession::setExternalProcessName(const QString &name)
{
    auto *nullInfo = static_cast<NullProcessInfo *>(_sessionProcessInfo);
    nullInfo->setExternalName(name);
    Q_EMIT sessionAttributeChanged();
}

void VirtualSession::setExternalCurrentDir(const QString &dir)
{
    auto *nullInfo = static_cast<NullProcessInfo *>(_sessionProcessInfo);
    nullInfo->setExternalCurrentDir(dir);
    Q_EMIT sessionAttributeChanged();
}

void VirtualSession::setExternalPaneTitle(const QString &title)
{
    setSessionAttribute(WindowTitle, title);
}

void VirtualSession::setExternalPid(int pid)
{
    auto *nullInfo = static_cast<NullProcessInfo *>(_sessionProcessInfo);
    bool ok = false;
    const int currentPid = nullInfo->pid(&ok);
    if (ok && currentPid == pid) {
        return;
    }
    nullInfo->setExternalPid(pid);
    Q_EMIT sessionAttributeChanged();
}

void VirtualSession::run()
{
    // No-op: virtual sessions have no PTY to start
}

void VirtualSession::close()
{
    Q_EMIT finished(this);
}

} // namespace Konsole

#include "moc_VirtualSession.cpp"
