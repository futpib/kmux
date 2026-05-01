/*
    SPDX-FileCopyrightText: 2023 Theodore Wang <theodorewang12@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "ProcessInfoTest.h"

// Qt
#include <QDir>
#include <QString>
#include <QTest>

// Konsole
#include "../NullProcessInfo.h"
#include "../ProcessInfo.h"
#include "../session/Session.h"

// Unix
#include <pwd.h>
#include <unistd.h>

// Others
#include <memory>

#include <QTest>

using namespace Konsole;

std::unique_ptr<ProcessInfo> ProcessInfoTest::createProcInfo(const KProcess &proc)
{
    return std::unique_ptr<ProcessInfo>(ProcessInfo::newInstance(proc.processId()));
}

void ProcessInfoTest::testProcessValidity()
{
    if (Session::checkProgram(QStringLiteral("bash")).isEmpty())
        return;

    KProcess proc;
    proc.setProgram(QStringLiteral("bash"));
    proc.start();

    QVERIFY(createProcInfo(proc)->isValid());

    proc.close();
    proc.waitForFinished(100);
}

void ProcessInfoTest::testProcessCwd()
{
#ifndef Q_OS_FREEBSD
    if (Session::checkProgram(QStringLiteral("bash")).isEmpty())
        return;

    KProcess proc;
    proc.setProgram({QStringLiteral("bash"), QStringLiteral("-x")});
    proc.start();

    auto procInfo = createProcInfo(proc);
    const QString startDir(QDir::currentPath());
    const QString parentDir(startDir.mid(0, startDir.lastIndexOf(QLatin1Char('/'))));

    bool ok;
    QString currDir;

    currDir = procInfo->currentDir(&ok);
    QVERIFY(ok);
    QCOMPARE(currDir, startDir);

    proc.write(QStringLiteral("cd ..\n").toLocal8Bit());
    proc.waitForReadyRead(100);
    procInfo->update();

    currDir = procInfo->currentDir(&ok);
    QVERIFY(ok);
    QCOMPARE(currDir, parentDir);

    proc.write(QStringLiteral("exit\n").toLocal8Bit());
    proc.waitForFinished(100);
#endif
}

void ProcessInfoTest::testProcessNameSpecialChars()
{
#ifndef Q_OS_FREEBSD
    if (Session::checkProgram(QStringLiteral("bash")).isEmpty())
        return;

    const QVector<QString> specNames({QStringLiteral("(( a("), QStringLiteral("("), QStringLiteral("ab) ("), QStringLiteral(")")});

    KProcess mainProc;
    mainProc.setProgram({QStringLiteral("bash"), QStringLiteral("-x")});
    mainProc.start();

    auto mainProcInfo = createProcInfo(mainProc);
    bool ok;

    for (auto specName : specNames) {
        mainProc.write(QStringLiteral("cp $(which bash) '%1'\n").arg(specName).toLocal8Bit());
        mainProc.waitForReadyRead(100);
        mainProc.write(QStringLiteral("exec %1'%2'\n").arg(QDir::currentPath() + QDir::separator(), specName).toLocal8Bit());
        mainProc.waitForReadyRead(100);

        mainProcInfo->update();

        QDir::current().remove(specName);

        const QString currName(mainProcInfo->name(&ok));
        QVERIFY(ok);
        QCOMPARE(currName, specName);
    }

    mainProc.write(QStringLiteral("exit\n").toLocal8Bit());
    mainProc.waitForFinished(100);
#endif
}

void ProcessInfoTest::testNullProcessInfoWithExternalPidReadsUserAndArgs()
{
#ifdef Q_OS_LINUX
    // After NullProcessInfo::setExternalPid binds the instance to a real
    // OS pid, the /proc/<pid>/... reads should populate UID, user name,
    // and argv exactly like UnixProcessInfo would. This is the substrate
    // that lets tmux panes resolve %u/%B and SSHProcessInfo extract
    // user/host from the pane process's command line.
    NullProcessInfo info(0);
    info.setExternalPid(static_cast<int>(getpid()));

    bool ok = false;

    const int uid = info.userId(&ok);
    QVERIFY2(ok, "UID was not set from /proc/<pid>/status");
    QCOMPARE(uid, static_cast<int>(getuid()));

    const QString user = info.userName();
    QVERIFY2(!user.isEmpty(), "User name was not resolved via getpwuid_r");

    struct passwd *expected = getpwuid(getuid());
    QVERIFY(expected != nullptr);
    QCOMPARE(user, QLatin1String(expected->pw_name));

    const QVector<QString> args = info.arguments(&ok);
    QVERIFY2(ok, "Arguments flag was not set");
    QVERIFY2(!args.isEmpty(), "Arguments were not read from /proc/<pid>/cmdline");
    QVERIFY2(args[0].endsWith(QLatin1String("ProcessInfoTest")),
             qPrintable(QStringLiteral("argv[0] is \"%1\", expected to end in ProcessInfoTest").arg(args[0])));
#else
    QSKIP("setExternalPid only reads /proc on Linux");
#endif
}

QTEST_MAIN(ProcessInfoTest)

#include "moc_ProcessInfoTest.cpp"
