/*
    SPDX-FileCopyrightText: 2007-2008 Robert Knight <robertknight@gmail.countm>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "NullProcessInfo.h"

#ifdef Q_OS_LINUX
#include <pwd.h>
#include <unistd.h>

#include <vector>

#include <QFile>
#include <QTextStream>
#endif

using namespace Konsole;

NullProcessInfo::NullProcessInfo(int pid)
    : ProcessInfo(pid)
{
}

void NullProcessInfo::setExternalName(const QString &name)
{
    setName(name);
}

void NullProcessInfo::setExternalCurrentDir(const QString &dir)
{
    setCurrentDir(dir);
}

void NullProcessInfo::setExternalPid(int pid)
{
    if (pid <= 0 || pid == _externalPid) {
        return;
    }
    _externalPid = pid;
    setPid(pid);

#ifdef Q_OS_LINUX
    // Read UID from /proc/<pid>/status
    QFile statusFile(QStringLiteral("/proc/%1/status").arg(pid));
    if (statusFile.open(QIODevice::ReadOnly)) {
        QTextStream stream(&statusFile);
        QString line;
        while (!(line = stream.readLine()).isNull()) {
            if (line.startsWith(QLatin1String("Uid:"))) {
                const auto parts = line.split(QLatin1Char('\t'), Qt::SkipEmptyParts);
                if (parts.size() >= 2) {
                    bool ok = false;
                    const int uid = parts[1].toInt(&ok);
                    if (ok) {
                        setUserId(uid);
                    }
                }
                break;
            }
        }
    }

    readUserName();
    readArguments(pid);
#endif
}

void NullProcessInfo::readProcessInfo(int /*pid*/)
{
}

bool NullProcessInfo::readProcessName(int /*pid*/)
{
    return false;
}

bool NullProcessInfo::readCurrentDir(int /*pid*/)
{
    return false;
}

bool NullProcessInfo::readArguments(int pid)
{
#ifdef Q_OS_LINUX
    if (pid <= 0) {
        return false;
    }
    QFile argFile(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!argFile.open(QIODevice::ReadOnly)) {
        setFileError(argFile.error());
        return false;
    }
    QTextStream stream(&argFile);
    const QString data = stream.readAll();
    const QStringList argList = data.split(QLatin1Char('\0'));
    clearArguments();
    for (const QString &entry : argList) {
        if (!entry.isEmpty()) {
            addArgument(entry);
        }
    }
    return true;
#else
    Q_UNUSED(pid);
    return false;
#endif
}

void NullProcessInfo::readUserName()
{
#ifdef Q_OS_LINUX
    bool ok = false;
    const int uid = userId(&ok);
    if (!ok) {
        return;
    }

    long bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize == -1) {
        bufsize = 16384;
    }
    std::vector<char> buf(static_cast<size_t>(bufsize));
    struct passwd pwd;
    struct passwd *result = nullptr;
    const int status = getpwuid_r(uid, &pwd, buf.data(), buf.size(), &result);
    if (status == 0 && result != nullptr) {
        setUserName(QLatin1String(pwd.pw_name));
    }
#endif
}
