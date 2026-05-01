/*
    SPDX-FileCopyrightText: 2007-2008 Robert Knight <robertknight@gmail.countm>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef NULLPROCESSINFO_H
#define NULLPROCESSINFO_H

#include "ProcessInfo.h"
#include "konsoleprivate_export.h"

namespace Konsole
{
/**
 * Implementation of ProcessInfo which does nothing.
 * Used on platforms where a suitable ProcessInfo subclass is not
 * available.
 *
 * isValid() will always return false for instances of NullProcessInfo
 */
class KONSOLEPRIVATE_EXPORT NullProcessInfo : public ProcessInfo
{
public:
    /**
     * Constructs a new NullProcessInfo instance.
     * See ProcessInfo::newInstance()
     */
    explicit NullProcessInfo(int pid);

    void setExternalName(const QString &name);
    void setExternalCurrentDir(const QString &dir);
    // Bind this NullProcessInfo to a real OS pid so the /proc/<pid>/...
    // backed reads below populate user/UID/arguments. Used by tmux panes,
    // where pane_current_command/path arrive separately but argv/UID need
    // to come from the kernel.
    void setExternalPid(int pid);

protected:
    void readProcessInfo(int pid) override;
    bool readProcessName(int pid) override;
    bool readCurrentDir(int pid) override;
    bool readArguments(int pid) override;
    void readUserName(void) override;

private:
    int _externalPid = 0;
};

}

#endif
