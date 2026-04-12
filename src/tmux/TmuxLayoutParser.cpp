/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxLayoutParser.h"

namespace Konsole
{

std::optional<TmuxLayoutNode> TmuxLayoutParser::parse(const QString &layoutString)
{
    if (layoutString.isEmpty()) {
        return std::nullopt;
    }

    // Skip the 4-char hex checksum and comma: "a1b2,"
    int pos = 0;
    int commaIdx = layoutString.indexOf(QLatin1Char(','));
    if (commaIdx < 0) {
        return std::nullopt;
    }
    pos = commaIdx + 1;

    // Back up to include the dimensions that follow the comma
    // The format after checksum is: WxH,X,Y... so we need to back up
    // Actually the comma we found IS the separator between checksum and dimensions
    // But dimensions start with W, and the format is WxH,X,Y
    // So pos is already at the start of W

    // Wait - the checksum IS the hex chars before the first comma, but
    // the dimensions format is WxH,X,Y which also contains commas.
    // The checksum is exactly 4 hex chars.
    // Let's just skip 4 chars + comma.
    if (layoutString.length() < 5 || layoutString[4] != QLatin1Char(',')) {
        return std::nullopt;
    }
    pos = 5;

    auto result = parseNode(layoutString, pos);
    return result;
}

bool TmuxLayoutParser::parseInt(const QString &s, int &pos, int &value)
{
    if (pos >= s.length() || !s[pos].isDigit()) {
        return false;
    }
    value = 0;
    while (pos < s.length() && s[pos].isDigit()) {
        value = value * 10 + (s[pos].unicode() - '0');
        pos++;
    }
    return true;
}

bool TmuxLayoutParser::parseDimensions(const QString &s, int &pos, TmuxLayoutNode &node)
{
    // Parse WxH,X,Y
    if (!parseInt(s, pos, node.width)) {
        return false;
    }
    if (pos >= s.length() || s[pos] != QLatin1Char('x')) {
        return false;
    }
    pos++; // skip 'x'
    if (!parseInt(s, pos, node.height)) {
        return false;
    }
    if (pos >= s.length() || s[pos] != QLatin1Char(',')) {
        return false;
    }
    pos++; // skip ','
    if (!parseInt(s, pos, node.xOffset)) {
        return false;
    }
    if (pos >= s.length() || s[pos] != QLatin1Char(',')) {
        return false;
    }
    pos++; // skip ','
    if (!parseInt(s, pos, node.yOffset)) {
        return false;
    }
    return true;
}

std::optional<TmuxLayoutNode> TmuxLayoutParser::parseNode(const QString &s, int &pos)
{
    TmuxLayoutNode node;

    if (!parseDimensions(s, pos, node)) {
        return std::nullopt;
    }

    if (pos >= s.length()) {
        // End of string — this must be a leaf but missing pane ID
        return std::nullopt;
    }

    QChar next = s[pos];

    if (next == QLatin1Char('{')) {
        // Horizontal split (side-by-side panes)
        node.type = TmuxLayoutNodeType::HSplit;
        pos++; // skip '{'

        while (pos < s.length() && s[pos] != QLatin1Char('}')) {
            if (s[pos] == QLatin1Char(',')) {
                pos++; // skip comma separator between children
            }
            auto child = parseNode(s, pos);
            if (!child.has_value()) {
                return std::nullopt;
            }
            node.children.append(child.value());
        }

        if (pos >= s.length() || s[pos] != QLatin1Char('}')) {
            return std::nullopt;
        }
        pos++; // skip '}'

    } else if (next == QLatin1Char('[')) {
        // Vertical split (stacked panes)
        node.type = TmuxLayoutNodeType::VSplit;
        pos++; // skip '['

        while (pos < s.length() && s[pos] != QLatin1Char(']')) {
            if (s[pos] == QLatin1Char(',')) {
                pos++; // skip comma separator between children
            }
            auto child = parseNode(s, pos);
            if (!child.has_value()) {
                return std::nullopt;
            }
            node.children.append(child.value());
        }

        if (pos >= s.length() || s[pos] != QLatin1Char(']')) {
            return std::nullopt;
        }
        pos++; // skip ']'

    } else if (next == QLatin1Char(',')) {
        // Leaf node: ",<pane-id>"
        node.type = TmuxLayoutNodeType::Leaf;
        pos++; // skip ','
        if (!parseInt(s, pos, node.paneId)) {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    return node;
}

uint16_t TmuxLayoutParser::checksum(const QByteArray &body)
{
    uint16_t csum = 0;
    for (char c : body) {
        csum = (csum >> 1) + ((csum & 1) << 15);
        csum += static_cast<unsigned char>(c);
    }
    return csum;
}

QString TmuxLayoutParser::serialize(const TmuxLayoutNode &root)
{
    QString body;
    serializeNode(root, body);
    uint16_t csum = checksum(body.toLatin1());
    return QStringLiteral("%1,%2").arg(csum, 4, 16, QLatin1Char('0')).arg(body);
}

void TmuxLayoutParser::serializeNode(const TmuxLayoutNode &node, QString &output)
{
    output += QString::number(node.width) + QLatin1Char('x') + QString::number(node.height) + QLatin1Char(',') + QString::number(node.xOffset) + QLatin1Char(',') + QString::number(node.yOffset);

    if (node.type == TmuxLayoutNodeType::Leaf) {
        output += QLatin1Char(',') + QString::number(node.paneId);
    } else {
        QChar open = (node.type == TmuxLayoutNodeType::HSplit) ? QLatin1Char('{') : QLatin1Char('[');
        QChar close = (node.type == TmuxLayoutNodeType::HSplit) ? QLatin1Char('}') : QLatin1Char(']');
        output += open;
        for (int i = 0; i < node.children.size(); ++i) {
            if (i > 0) {
                output += QLatin1Char(',');
            }
            serializeNode(node.children[i], output);
        }
        output += close;
    }
}

} // namespace Konsole
