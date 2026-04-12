/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXTREEMODEL_H
#define TMUXTREEMODEL_H

#include <QAbstractItemModel>
#include <QList>
#include <QString>

#include "TmuxController.h"
#include "konsoleprivate_export.h"

namespace Konsole
{

class ViewManager;

/**
 * 3-level tree model: Session → Window → Pane.
 */
class KONSOLEPRIVATE_EXPORT TmuxTreeModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum NodeType {
        SessionNode,
        WindowNode,
        PaneNode
    };
    enum Role {
        NodeTypeRole = Qt::UserRole + 1,
        IdRole,       // tmux id (session/window/pane)
        IsActiveRole, // the currently active pane/window/session
        ScoreRole,
    };

    explicit TmuxTreeModel(QObject *parent = nullptr);

    void setData(const QList<TmuxController::SessionDescriptor> &sessions);
    void setScore(const QModelIndex &index, int score);

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &idx, int role = Qt::DisplayRole) const override;

private:
    struct PaneInfo {
        int paneId = -1;
        QString title;
        bool active = false;
        mutable int score = 0;
    };
    struct WindowInfo {
        int windowId = -1;
        QString name;
        bool active = false;
        QList<PaneInfo> panes;
        mutable int score = 0;
    };
    struct SessionInfo {
        int sessionId = -1;
        QString name;
        bool active = true;
        QList<WindowInfo> windows;
        mutable int score = 0;
    };

    QList<SessionInfo> _sessions;

    // Internal pointer encoding: low 2 bits = level, upper bits = index
    // Level 0 = session, 1 = window, 2 = pane
    quintptr encodeId(int sessionRow, int windowRow = -1, int paneRow = -1) const;
    void decodeId(quintptr id, int &sessionRow, int &windowRow, int &paneRow) const;
};

} // namespace Konsole

#endif // TMUXTREEMODEL_H
