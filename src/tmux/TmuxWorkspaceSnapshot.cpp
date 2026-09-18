/*
    SPDX-FileCopyrightText: 2026 kmux contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxWorkspaceSnapshot.h"

#include "TmuxLayoutParser.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace Konsole
{

namespace
{
constexpr int MaxWindows = 256;
constexpr int MaxPanesPerWindow = 512;
constexpr int MaxStringLength = 32 * 1024;
constexpr int MaxArgumentCount = 1024;

bool validString(const QString &value)
{
    return value.size() <= MaxStringLength && !value.contains(QChar::Null);
}

QJsonArray stringListToJson(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values) {
        array.append(value);
    }
    return array;
}

std::optional<QStringList> stringListFromJson(const QJsonValue &value)
{
    if (!value.isArray() || value.toArray().size() > MaxArgumentCount) {
        return std::nullopt;
    }
    QStringList result;
    for (const QJsonValue &item : value.toArray()) {
        if (!item.isString() || !validString(item.toString())) {
            return std::nullopt;
        }
        result.append(item.toString());
    }
    return result;
}

QJsonObject workspaceToObject(const TmuxWorkspaceSnapshot &snapshot)
{
    QJsonArray windows;
    for (const TmuxWindowSnapshot &window : snapshot.windows) {
        QJsonArray panes;
        for (const TmuxPaneSnapshot &pane : window.panes) {
            QJsonObject paneObject;
            paneObject.insert(QStringLiteral("id"), pane.paneId);
            paneObject.insert(QStringLiteral("directory"), pane.workingDirectory);
            paneObject.insert(QStringLiteral("active"), pane.active);
            panes.append(paneObject);
        }

        QJsonObject windowObject;
        windowObject.insert(QStringLiteral("id"), window.windowId);
        windowObject.insert(QStringLiteral("index"), window.index);
        windowObject.insert(QStringLiteral("name"), window.name);
        windowObject.insert(QStringLiteral("layout"), window.layout);
        windowObject.insert(QStringLiteral("active"), window.active);
        windowObject.insert(QStringLiteral("panes"), panes);
        windows.append(windowObject);
    }

    QJsonObject object;
    object.insert(QStringLiteral("version"), snapshot.version);
    object.insert(QStringLiteral("serverPid"), QString::number(snapshot.serverPid));
    object.insert(QStringLiteral("sessionCreated"), QString::number(snapshot.sessionCreated));
    object.insert(QStringLiteral("sessionName"), snapshot.sessionName);
    object.insert(QStringLiteral("windows"), windows);
    return object;
}

std::optional<TmuxWorkspaceSnapshot> workspaceFromObject(const QJsonObject &object)
{
    TmuxWorkspaceSnapshot snapshot;
    snapshot.version = object.value(QStringLiteral("version")).toInt(-1);
    if (snapshot.version != TmuxWorkspaceSnapshot::CurrentVersion) {
        return std::nullopt;
    }

    bool pidOk = false;
    snapshot.serverPid = object.value(QStringLiteral("serverPid")).toString().toLongLong(&pidOk);
    bool createdOk = false;
    snapshot.sessionCreated = object.value(QStringLiteral("sessionCreated")).toString().toLongLong(&createdOk);
    snapshot.sessionName = object.value(QStringLiteral("sessionName")).toString();
    const QJsonValue windowsValue = object.value(QStringLiteral("windows"));
    if (!pidOk || snapshot.serverPid <= 0 || !createdOk || snapshot.sessionCreated <= 0 || snapshot.sessionName.isEmpty() || !validString(snapshot.sessionName)
        || !windowsValue.isArray() || windowsValue.toArray().size() > MaxWindows) {
        return std::nullopt;
    }

    QSet<int> windowIndexes;
    for (const QJsonValue &windowValue : windowsValue.toArray()) {
        if (!windowValue.isObject()) {
            return std::nullopt;
        }
        const QJsonObject windowObject = windowValue.toObject();
        TmuxWindowSnapshot window;
        window.windowId = windowObject.value(QStringLiteral("id")).toInt(-1);
        window.index = windowObject.value(QStringLiteral("index")).toInt(-1);
        window.name = windowObject.value(QStringLiteral("name")).toString();
        window.layout = windowObject.value(QStringLiteral("layout")).toString();
        window.active = windowObject.value(QStringLiteral("active")).toBool(false);
        const QJsonValue panesValue = windowObject.value(QStringLiteral("panes"));
        if (window.windowId < 0 || window.index < 0 || windowIndexes.contains(window.index) || !validString(window.name) || window.layout.isEmpty()
            || !validString(window.layout) || !panesValue.isArray() || panesValue.toArray().isEmpty() || panesValue.toArray().size() > MaxPanesPerWindow) {
            return std::nullopt;
        }
        windowIndexes.insert(window.index);

        QSet<int> paneIds;
        for (const QJsonValue &paneValue : panesValue.toArray()) {
            if (!paneValue.isObject()) {
                return std::nullopt;
            }
            const QJsonObject paneObject = paneValue.toObject();
            TmuxPaneSnapshot pane;
            pane.paneId = paneObject.value(QStringLiteral("id")).toInt(-1);
            pane.workingDirectory = paneObject.value(QStringLiteral("directory")).toString();
            pane.active = paneObject.value(QStringLiteral("active")).toBool(false);
            if (pane.paneId < 0 || paneIds.contains(pane.paneId) || !validString(pane.workingDirectory)) {
                return std::nullopt;
            }
            paneIds.insert(pane.paneId);
            window.panes.append(std::move(pane));
        }
        snapshot.windows.append(std::move(window));
    }

    if (!snapshot.isValid()) {
        return std::nullopt;
    }
    return snapshot;
}

bool remapNode(TmuxLayoutNode &node, const QHash<int, int> &paneIds)
{
    if (node.type == TmuxLayoutNodeType::Leaf) {
        const auto it = paneIds.constFind(node.paneId);
        if (it == paneIds.constEnd()) {
            return false;
        }
        node.paneId = it.value();
        return true;
    }
    for (TmuxLayoutNode &child : node.children) {
        if (!remapNode(child, paneIds)) {
            return false;
        }
    }
    return true;
}
}

bool TmuxWorkspaceSnapshot::isValid() const
{
    if (version != CurrentVersion || serverPid <= 0 || sessionCreated <= 0 || sessionName.isEmpty() || !validString(sessionName) || windows.isEmpty()
        || windows.size() > MaxWindows) {
        return false;
    }
    QSet<int> windowIds;
    QSet<int> windowIndexes;
    QSet<int> paneIds;
    for (const TmuxWindowSnapshot &window : windows) {
        if (window.windowId < 0 || windowIds.contains(window.windowId) || window.index < 0 || windowIndexes.contains(window.index) || !validString(window.name)
            || window.layout.isEmpty() || !validString(window.layout) || window.panes.isEmpty() || window.panes.size() > MaxPanesPerWindow) {
            return false;
        }
        windowIds.insert(window.windowId);
        windowIndexes.insert(window.index);
        for (const TmuxPaneSnapshot &pane : window.panes) {
            if (pane.paneId < 0 || paneIds.contains(pane.paneId) || !validString(pane.workingDirectory)) {
                return false;
            }
            paneIds.insert(pane.paneId);
        }
    }
    return true;
}

QByteArray TmuxWorkspaceSnapshot::toJson() const
{
    return QJsonDocument(workspaceToObject(*this)).toJson(QJsonDocument::Compact);
}

std::optional<TmuxWorkspaceSnapshot> TmuxWorkspaceSnapshot::fromJson(const QByteArray &json)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    return workspaceFromObject(document.object());
}

std::optional<QString> TmuxWorkspaceSnapshot::remapLayout(const TmuxWindowSnapshot &window, const QList<int> &newPaneIds)
{
    if (window.panes.size() != newPaneIds.size()) {
        return std::nullopt;
    }
    auto layout = TmuxLayoutParser::parse(window.layout);
    if (!layout.has_value()) {
        return std::nullopt;
    }
    QHash<int, int> paneIds;
    for (int i = 0; i < window.panes.size(); ++i) {
        if (newPaneIds.at(i) < 0 || paneIds.contains(window.panes.at(i).paneId)) {
            return std::nullopt;
        }
        paneIds.insert(window.panes.at(i).paneId, newPaneIds.at(i));
    }
    if (!remapNode(layout.value(), paneIds)) {
        return std::nullopt;
    }
    return TmuxLayoutParser::serialize(layout.value());
}

bool TmuxRestoreState::isValid() const
{
    if (version != CurrentVersion || !validString(tmuxPath) || !workspace.isValid() || tmuxArgs.size() > MaxArgumentCount
        || rshCommand.size() > MaxArgumentCount || rshCommand.contains(QString()) || visibleWindowIndexes.size() > workspace.windows.size()
        || (activeWindowIndex >= 0 && !visibleWindowIndexes.contains(activeWindowIndex))) {
        return false;
    }
    for (const QString &argument : tmuxArgs) {
        if (!validString(argument)) {
            return false;
        }
    }
    for (const QString &argument : rshCommand) {
        if (!validString(argument)) {
            return false;
        }
    }
    QSet<int> savedIndexes;
    for (const TmuxWindowSnapshot &window : workspace.windows) {
        savedIndexes.insert(window.index);
    }
    QSet<int> visibleIndexes;
    for (int index : visibleWindowIndexes) {
        if (!savedIndexes.contains(index) || visibleIndexes.contains(index)) {
            return false;
        }
        visibleIndexes.insert(index);
    }
    return true;
}

QByteArray TmuxRestoreState::toJson() const
{
    QJsonArray visibleIndexes;
    for (int index : visibleWindowIndexes) {
        visibleIndexes.append(index);
    }

    QJsonObject object;
    object.insert(QStringLiteral("version"), version);
    object.insert(QStringLiteral("tmuxPath"), tmuxPath);
    object.insert(QStringLiteral("tmuxArgs"), stringListToJson(tmuxArgs));
    object.insert(QStringLiteral("rshCommand"), stringListToJson(rshCommand));
    object.insert(QStringLiteral("visibleWindowIndexes"), visibleIndexes);
    object.insert(QStringLiteral("activeWindowIndex"), activeWindowIndex);
    object.insert(QStringLiteral("workspace"), workspaceToObject(workspace));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<TmuxRestoreState> TmuxRestoreState::fromJson(const QByteArray &json)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::nullopt;
    }
    const QJsonObject object = document.object();
    TmuxRestoreState state;
    state.version = object.value(QStringLiteral("version")).toInt(-1);
    state.tmuxPath = object.value(QStringLiteral("tmuxPath")).toString();
    auto tmuxArgs = stringListFromJson(object.value(QStringLiteral("tmuxArgs")));
    auto rshCommand = stringListFromJson(object.value(QStringLiteral("rshCommand")));
    auto workspace =
        object.value(QStringLiteral("workspace")).isObject() ? workspaceFromObject(object.value(QStringLiteral("workspace")).toObject()) : std::nullopt;
    const QJsonValue visibleValue = object.value(QStringLiteral("visibleWindowIndexes"));
    if (state.version != CurrentVersion || !validString(state.tmuxPath) || !tmuxArgs.has_value() || !rshCommand.has_value() || !workspace.has_value()
        || !visibleValue.isArray() || visibleValue.toArray().size() > workspace->windows.size()) {
        return std::nullopt;
    }
    state.tmuxArgs = std::move(tmuxArgs.value());
    state.rshCommand = std::move(rshCommand.value());
    state.workspace = std::move(workspace.value());
    state.activeWindowIndex = object.value(QStringLiteral("activeWindowIndex")).toInt(-1);
    QSet<int> visibleIndexes;
    for (const QJsonValue &value : visibleValue.toArray()) {
        const int index = value.toInt(-1);
        if (index < 0 || visibleIndexes.contains(index)) {
            return std::nullopt;
        }
        visibleIndexes.insert(index);
        state.visibleWindowIndexes.append(index);
    }
    if (!state.isValid()) {
        return std::nullopt;
    }
    return state;
}

}
