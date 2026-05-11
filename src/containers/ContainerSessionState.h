/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef CONTAINERSESSIONSTATE_H
#define CONTAINERSESSIONSTATE_H

#include <algorithm>
#include <QColor>
#include <QDateTime>
#include <QGuiApplication>
#include <QObject>
#include <QPalette>
#include <QString>

namespace Konsole::ContainerSessionState
{

inline constexpr auto PendingContainerKeyProperty = "konsolePendingContainerKey";
inline constexpr auto PendingContainerTypeProperty = "konsolePendingContainerType";
inline constexpr auto PendingContainerNameProperty = "konsolePendingContainerName";
inline constexpr auto PendingContainerUntilProperty = "konsolePendingContainerUntil";
inline constexpr qint64 PendingContainerGraceMs = 20000;

struct PendingContainerInfo {
    QString key;
    QString type;
    QString name;
    qint64 until = 0;

    bool isActive() const
    {
        return !key.isEmpty() && until > QDateTime::currentMSecsSinceEpoch();
    }
};

inline void clearPendingContainerInfo(QObject *object)
{
    if (object == nullptr) {
        return;
    }

    object->setProperty(PendingContainerKeyProperty, QString());
    object->setProperty(PendingContainerTypeProperty, QString());
    object->setProperty(PendingContainerNameProperty, QString());
    object->setProperty(PendingContainerUntilProperty, qint64(0));
}

inline void setPendingContainerInfo(QObject *object, const QString &key, const QString &type, const QString &name)
{
    if (object == nullptr || key.isEmpty()) {
        clearPendingContainerInfo(object);
        return;
    }

    object->setProperty(PendingContainerKeyProperty, key);
    object->setProperty(PendingContainerTypeProperty, type);
    object->setProperty(PendingContainerNameProperty, name);
    object->setProperty(PendingContainerUntilProperty, QDateTime::currentMSecsSinceEpoch() + PendingContainerGraceMs);
}

inline PendingContainerInfo pendingContainerInfo(const QObject *object)
{
    if (object == nullptr) {
        return {};
    }

    PendingContainerInfo info;
    info.key = object->property(PendingContainerKeyProperty).toString();
    info.type = object->property(PendingContainerTypeProperty).toString();
    info.name = object->property(PendingContainerNameProperty).toString();
    info.until = object->property(PendingContainerUntilProperty).toLongLong();
    return info;
}

inline QColor colorForContainerKey(const QString &containerKey)
{
    const uint hash = qHash(containerKey);
    const int hue = static_cast<int>(hash % 360);
    const int saturation = 130 + static_cast<int>((hash >> 7) % 90);
    const int value = 170 + static_cast<int>((hash >> 15) % 60);
    const QColor hashedColor = QColor::fromHsv(hue, saturation, value);

    const auto *guiApp = qobject_cast<QGuiApplication *>(QGuiApplication::instance());
    if (guiApp == nullptr) {
        return hashedColor;
    }

    const QColor accent = guiApp->palette().color(QPalette::Active, QPalette::Highlight);
    if (!accent.isValid()) {
        return hashedColor;
    }

    // Keep per-container hue diversity, but slightly tint with theme accent.
    return QColor::fromRgb((hashedColor.red() * 3 + accent.red()) / 4,
                           (hashedColor.green() * 3 + accent.green()) / 4,
                           (hashedColor.blue() * 3 + accent.blue()) / 4);
}

} // namespace Konsole::ContainerSessionState

#endif // CONTAINERSESSIONSTATE_H
