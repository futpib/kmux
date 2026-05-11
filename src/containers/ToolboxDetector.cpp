/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "ToolboxDetector.h"
#include "ToolboxListParser.h"

#include <KLocalizedString>
#include <KSandbox>

#include <QFile>
#include <QProcess>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QTextStream>

namespace Konsole
{
static bool hasToolboxMarkers(const QHash<QByteArray, QByteArray> &env)
{
    return env.contains("TOOLBOX_PATH") || env.contains("TOOLBOX_NAME") || env.value("container") == "toolbox";
}

static QString containerNameFromToolboxEnv(const QHash<QByteArray, QByteArray> &env)
{
    QString containerName = QString::fromUtf8(env.value("TOOLBOX_NAME"));
    if (containerName.isEmpty()) {
        containerName = QString::fromUtf8(env.value("CONTAINER_ID"));
    }
    return containerName;
}

static QHash<QByteArray, QByteArray> readProcEnvironment(int pid)
{
    QHash<QByteArray, QByteArray> env;
    QFile file(QStringLiteral("/proc/%1/environ").arg(pid));
    if (!file.open(QIODevice::ReadOnly)) {
        return env;
    }

    const QList<QByteArray> entries = file.readAll().split('\0');
    for (const QByteArray &entry : entries) {
        const int sep = entry.indexOf('=');
        if (sep <= 0) {
            continue;
        }
        env.insert(entry.left(sep), entry.mid(sep + 1));
    }
    return env;
}

static QString readContainerNameFromContainerEnv(int pid)
{
    QFile file(QStringLiteral("/proc/%1/root/run/.containerenv").arg(pid));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine().trimmed();
        if (!line.startsWith(QStringLiteral("name="))) {
            continue;
        }
        QString value = line.mid(5).trimmed();
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
            value = value.mid(1, value.size() - 2);
        }
        return value;
    }

    return {};
}

ToolboxDetector::ToolboxDetector(QObject *parent)
    : IContainerDetector(parent)
{
}

QString ToolboxDetector::typeId() const
{
    return QStringLiteral("toolbox");
}

QString ToolboxDetector::displayName() const
{
    return i18n("Toolbox");
}

QString ToolboxDetector::iconName() const
{
    return QStringLiteral("utilities-terminal");
}

std::optional<ContainerInfo> ToolboxDetector::detect(int pid) const
{
    if (pid <= 0) {
        return std::nullopt;
    }

    // Fast-path for sessions started already inside toolbox.
    const auto env = readProcEnvironment(pid);
    if (!hasToolboxMarkers(env)) {
        return std::nullopt;
    }

    QString containerName = containerNameFromToolboxEnv(env);
    if (containerName.isEmpty()) {
        containerName = readContainerNameFromContainerEnv(pid);
    }
    if (containerName.isEmpty()) {
        return std::nullopt;
    }

    return buildContainerInfo(containerName);
}

ContainerInfo ToolboxDetector::buildContainerInfo(const QString &name) const
{
    return ContainerInfo{.detector = this, .name = name, .displayName = name, .iconName = iconName(), .hostPid = std::nullopt};
}

QStringList ToolboxDetector::entryCommand(const QString &containerName) const
{
    return {QStringLiteral("toolbox"), QStringLiteral("enter"), containerName};
}

void ToolboxDetector::startListContainers()
{
    // Return if toolbox is not found
    if (QStandardPaths::findExecutable(QStringLiteral("toolbox")).isEmpty()) {
        Q_EMIT listContainersFinished({});
        return;
    }
    auto *process = new QProcess(this);
    process->setProgram(QStringLiteral("toolbox"));
    process->setArguments({QStringLiteral("list"), QStringLiteral("--containers")});
    auto done = QSharedPointer<bool>::create(false);

    connect(process, &QProcess::finished, this, [this, process, done](int exitCode, QProcess::ExitStatus exitStatus) {
        if (*done) {
            return;
        }
        *done = true;
        QList<ContainerInfo> containers;
        process->deleteLater();

        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            Q_EMIT listContainersFinished(containers);
            return;
        }

        const QString output = QString::fromUtf8(process->readAllStandardOutput());
        const QStringList names = parseToolboxContainerNames(output);
        for (const QString &name : names) {
            containers.append(buildContainerInfo(name));
        }

        Q_EMIT listContainersFinished(containers);
    });

    connect(process, &QProcess::errorOccurred, this, [this, process, done](QProcess::ProcessError) {
        if (*done) {
            return;
        }
        *done = true;
        process->deleteLater();
        Q_EMIT listContainersFinished({});
    });

    KSandbox::startHostProcess(*process, QProcess::ReadOnly);
}

} // namespace Konsole

#include "moc_ToolboxDetector.cpp"
