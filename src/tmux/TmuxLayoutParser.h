/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef TMUXLAYOUTPARSER_H
#define TMUXLAYOUTPARSER_H

#include <QList>
#include <QString>

#include <optional>

#include "konsoleprivate_export.h"

namespace Konsole
{

enum class TmuxLayoutNodeType { Leaf,
                                HSplit,
                                VSplit };

struct TmuxLayoutNode {
    TmuxLayoutNodeType type;
    int width = 0;
    int height = 0;
    int xOffset = 0;
    int yOffset = 0;
    int paneId = -1;                // leaf only
    QList<TmuxLayoutNode> children; // split only
};

class KONSOLEPRIVATE_EXPORT TmuxLayoutParser
{
public:
    static std::optional<TmuxLayoutNode> parse(const QString &layoutString);
    static QString serialize(const TmuxLayoutNode &root);
    static uint16_t checksum(const QByteArray &body);

private:
    static std::optional<TmuxLayoutNode> parseNode(const QString &s, int &pos);
    static bool parseDimensions(const QString &s, int &pos, TmuxLayoutNode &node);
    static bool parseInt(const QString &s, int &pos, int &value);
    static void serializeNode(const TmuxLayoutNode &node, QString &output);
};

} // namespace Konsole

#endif // TMUXLAYOUTPARSER_H
