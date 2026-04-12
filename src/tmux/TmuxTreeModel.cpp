/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxTreeModel.h"

namespace Konsole
{

TmuxTreeModel::TmuxTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
{
}

quintptr TmuxTreeModel::encodeId(int sessionRow, int windowRow, int paneRow) const
{
    Q_UNUSED(paneRow)
    // Level: 1=session, 2=window (needs sessionRow), 3=pane (needs sessionRow+windowRow)
    // Keep internalId() non-zero to avoid ambiguity with default-constructed QModelIndex.
    if (windowRow >= 0) {
        // level 3 (pane): session(20) | window(20) | level(4)
        return (quintptr(sessionRow) << 24) | (quintptr(windowRow) << 4) | 3;
    }
    if (sessionRow >= 0) {
        // level 2 (window): session(20) | level(4)
        return (quintptr(sessionRow) << 4) | 2;
    }
    // level 1 (session)
    return 1;
}

void TmuxTreeModel::decodeId(quintptr id, int &sessionRow, int &windowRow, int &paneRow) const
{
    int level = int(id & 0xF);
    sessionRow = -1;
    windowRow = -1;
    paneRow = -1;
    if (level == 1) {
        return;
    }
    if (level == 2) {
        sessionRow = int(id >> 4);
        return;
    }
    // level 3
    windowRow = int((id >> 4) & 0xFFFFF);
    sessionRow = int(id >> 24);
}

void TmuxTreeModel::setData(const QList<TmuxController::SessionDescriptor> &sessions)
{
    beginResetModel();
    _sessions.clear();
    for (const auto &src : sessions) {
        SessionInfo s;
        s.sessionId = src.sessionId;
        s.name = src.name;
        s.active = src.active;
        for (const auto &srcW : src.windows) {
            WindowInfo w;
            w.windowId = srcW.windowId;
            w.name = srcW.name;
            w.active = srcW.active;
            for (const auto &srcP : srcW.panes) {
                PaneInfo p;
                p.paneId = srcP.paneId;
                p.title = srcP.title;
                p.active = srcP.active;
                w.panes.append(p);
            }
            s.windows.append(w);
        }
        _sessions.append(s);
    }
    endResetModel();
}

QModelIndex TmuxTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (column != 0 || row < 0) {
        return {};
    }
    if (!parent.isValid()) {
        // session level
        if (row >= _sessions.size()) {
            return {};
        }
        return createIndex(row, 0, encodeId(-1));
    }

    int sRow, wRow, pRow;
    decodeId(parent.internalId(), sRow, wRow, pRow);
    int parentLevel = int(parent.internalId() & 0xF);

    if (parentLevel == 1) {
        // parent is a session; child is a window
        int sessionIdx = parent.row();
        if (sessionIdx >= _sessions.size() || row >= _sessions[sessionIdx].windows.size()) {
            return {};
        }
        return createIndex(row, 0, encodeId(sessionIdx));
    }
    if (parentLevel == 2) {
        // parent is a window; child is a pane
        int sessionIdx = sRow;
        int windowIdx = parent.row();
        if (sessionIdx < 0 || sessionIdx >= _sessions.size())
            return {};
        if (windowIdx < 0 || windowIdx >= _sessions[sessionIdx].windows.size())
            return {};
        if (row >= _sessions[sessionIdx].windows[windowIdx].panes.size())
            return {};
        return createIndex(row, 0, encodeId(sessionIdx, windowIdx));
    }
    return {};
}

QModelIndex TmuxTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    int sRow, wRow, pRow;
    decodeId(child.internalId(), sRow, wRow, pRow);
    int level = int(child.internalId() & 0xF);
    if (level == 1) {
        // session — top level
        return {};
    }
    if (level == 2) {
        // child is a window; parent is the session at row sRow
        int sessionIdx = sRow;
        if (sessionIdx < 0 || sessionIdx >= _sessions.size())
            return {};
        return createIndex(sessionIdx, 0, encodeId(-1));
    }
    // level == 3: child is a pane; parent is the window at row wRow in session sRow
    int sessionIdx = sRow;
    int windowIdx = wRow;
    if (sessionIdx < 0 || sessionIdx >= _sessions.size())
        return {};
    if (windowIdx < 0 || windowIdx >= _sessions[sessionIdx].windows.size())
        return {};
    return createIndex(windowIdx, 0, encodeId(sessionIdx));
}

int TmuxTreeModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return _sessions.size();
    }
    int level = int(parent.internalId() & 0xF);
    if (level == 1) {
        int sessionIdx = parent.row();
        if (sessionIdx < 0 || sessionIdx >= _sessions.size())
            return 0;
        return _sessions[sessionIdx].windows.size();
    }
    if (level == 2) {
        int sRow, wRow, pRow;
        decodeId(parent.internalId(), sRow, wRow, pRow);
        int sessionIdx = sRow;
        int windowIdx = parent.row();
        if (sessionIdx < 0 || sessionIdx >= _sessions.size())
            return 0;
        if (windowIdx < 0 || windowIdx >= _sessions[sessionIdx].windows.size())
            return 0;
        return _sessions[sessionIdx].windows[windowIdx].panes.size();
    }
    return 0;
}

int TmuxTreeModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant TmuxTreeModel::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid())
        return {};
    int level = int(idx.internalId() & 0xF);
    int sRow, wRow, pRow;
    decodeId(idx.internalId(), sRow, wRow, pRow);

    if (level == 1) {
        // session
        if (idx.row() < 0 || idx.row() >= _sessions.size())
            return {};
        const SessionInfo &s = _sessions[idx.row()];
        switch (role) {
        case Qt::DisplayRole:
            return s.name;
        case NodeTypeRole:
            return int(SessionNode);
        case IdRole:
            return s.sessionId;
        case IsActiveRole:
            return s.active;
        case ScoreRole:
            return s.score;
        }
    } else if (level == 2) {
        // window
        int sessionIdx = sRow;
        if (sessionIdx < 0 || sessionIdx >= _sessions.size())
            return {};
        if (idx.row() < 0 || idx.row() >= _sessions[sessionIdx].windows.size())
            return {};
        const WindowInfo &w = _sessions[sessionIdx].windows[idx.row()];
        switch (role) {
        case Qt::DisplayRole:
            return w.name.isEmpty() ? QStringLiteral("window %1").arg(w.windowId) : w.name;
        case NodeTypeRole:
            return int(WindowNode);
        case IdRole:
            return w.windowId;
        case IsActiveRole:
            return w.active;
        case ScoreRole:
            return w.score;
        }
    } else if (level == 3) {
        // pane
        int sessionIdx = sRow;
        int windowIdx = wRow;
        if (sessionIdx < 0 || sessionIdx >= _sessions.size())
            return {};
        if (windowIdx < 0 || windowIdx >= _sessions[sessionIdx].windows.size())
            return {};
        if (idx.row() < 0 || idx.row() >= _sessions[sessionIdx].windows[windowIdx].panes.size())
            return {};
        const PaneInfo &p = _sessions[sessionIdx].windows[windowIdx].panes[idx.row()];
        switch (role) {
        case Qt::DisplayRole:
            return p.title.isEmpty() ? QStringLiteral("pane %%1").arg(p.paneId) : p.title;
        case NodeTypeRole:
            return int(PaneNode);
        case IdRole:
            return p.paneId;
        case IsActiveRole:
            return p.active;
        case ScoreRole:
            return p.score;
        }
    }
    return {};
}

void TmuxTreeModel::setScore(const QModelIndex &index, int score)
{
    if (!index.isValid())
        return;
    int level = int(index.internalId() & 0xF);
    int sRow, wRow, pRow;
    decodeId(index.internalId(), sRow, wRow, pRow);
    if (level == 1 && index.row() >= 0 && index.row() < _sessions.size()) {
        _sessions[index.row()].score = score;
    } else if (level == 2 && sRow >= 0 && sRow < _sessions.size() && index.row() >= 0 && index.row() < _sessions[sRow].windows.size()) {
        _sessions[sRow].windows[index.row()].score = score;
    } else if (level == 3 && sRow >= 0 && sRow < _sessions.size() && wRow >= 0 && wRow < _sessions[sRow].windows.size() && index.row() >= 0 && index.row() < _sessions[sRow].windows[wRow].panes.size()) {
        _sessions[sRow].windows[wRow].panes[index.row()].score = score;
    }
}

} // namespace Konsole

#include "moc_TmuxTreeModel.cpp"
