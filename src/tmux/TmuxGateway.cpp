/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxGateway.h"

#include "Emulation.h"
#include "TmuxCommand.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(KonsoleTmuxGateway, "konsole.tmux.gateway", QtWarningMsg)

namespace Konsole
{

TmuxGateway::TmuxGateway(WriteCallback writeCallback, QObject *parent)
    : QObject(parent)
    , _writeCallback(std::move(writeCallback))
{
}

void TmuxGateway::processLine(const QByteArray &line)
{
    if (_exited) {
        return;
    }

    if (_inResponseBlock) {
        // Match %end/%error by command ID: "%end <id> <number>" or "%error <id> <number>"
        if (line.startsWith("%end ") || line.startsWith("%error ")) {
            bool success = line.startsWith("%end ");
            int idEnd = line.indexOf(' ', success ? 5 : 7);
            QByteArray idToken = (idEnd > 0) ? line.mid(success ? 5 : 7, idEnd - (success ? 5 : 7)) : line.mid(success ? 5 : 7);
            bool ok = false;
            int endId = idToken.toInt(&ok);
            if (ok && endId == _currentCommand.commandId) {
                finishCurrentCommand(success);
            }
            // If ID doesn't match, ignore (could be a nested server response)
            return;
        }
        // Accumulate response data
        if (!_serverOriginated) {
            if (!_currentCommand.response.isEmpty()) {
                _currentCommand.response += QLatin1Char('\n');
            }
            _currentCommand.response += QString::fromUtf8(line);
        }
        return;
    }

    if (line.startsWith("%begin ")) {
        // The first %begin proves the tmux server is alive and
        // responding.  Emit ready() so the controller can initialize.
        if (!_ready) {
            _ready = true;
            Q_EMIT ready();
        }

        // Format: %begin <command_id> <command_number> [<flags>]
        QList<QByteArray> parts = line.mid(7).split(' ');
        int commandId = -1;
        bool clientOriginated = true;
        if (!parts.isEmpty()) {
            bool ok = false;
            commandId = parts[0].toInt(&ok);
            if (!ok) {
                commandId = -1;
            }
        }
        if (parts.size() >= 3) {
            bool ok = false;
            int flags = parts[2].toInt(&ok);
            if (ok) {
                clientOriginated = (flags & 0x01) != 0;
            }
        }

        if (!clientOriginated || _pendingCommands.isEmpty()) {
            // Server-originated command; track ID but ignore response
            _inResponseBlock = true;
            _serverOriginated = true;
            _currentCommand = PendingCommand();
            _currentCommand.commandId = commandId;
            return;
        }
        _currentCommand = _pendingCommands.dequeue();
        _currentCommand.commandId = commandId;
        _inResponseBlock = true;
        _serverOriginated = false;
        return;
    }

    if (line.startsWith("%")) {
        handleNotification(line);
    }
}

std::optional<TmuxNotification> TmuxGateway::parseNotification(const QByteArray &line)
{
    if (line.startsWith("%output ")) {
        int firstSpace = line.indexOf(' ', 8);
        if (firstSpace < 0) {
            return std::nullopt;
        }
        QByteArray paneToken = line.mid(8, firstSpace - 8);
        int paneId = parsePaneId(paneToken);
        if (paneId < 0) {
            return std::nullopt;
        }
        QByteArray encoded = line.mid(firstSpace + 1);
        return TmuxOutputNotification{paneId, decodeOctalEscapes(encoded)};

    } else if (line.startsWith("%layout-change ")) {
        QList<QByteArray> parts = line.mid(15).split(' ');
        if (parts.size() < 2) {
            return std::nullopt;
        }
        int windowId = parseWindowId(parts[0]);
        QString layout = QString::fromUtf8(parts[1]);
        QString visibleLayout;
        bool zoomed = false;
        if (parts.size() >= 3) {
            visibleLayout = QString::fromUtf8(parts[2]);
        }
        if (parts.size() >= 4) {
            zoomed = parts[3].contains('Z');
        }
        return TmuxLayoutChangedNotification{windowId, layout, visibleLayout, zoomed};

    } else if (line.startsWith("%window-add ")) {
        int windowId = parseWindowId(line.mid(12));
        return TmuxWindowAddedNotification{windowId};

    } else if (line.startsWith("%window-close ") || line.startsWith("%unlinked-window-close ")) {
        QByteArray rest = line.startsWith("%window-close ") ? line.mid(14) : line.mid(23);
        int windowId = parseWindowId(rest.split(' ').first());
        return TmuxWindowClosedNotification{windowId};

    } else if (line.startsWith("%window-renamed ")) {
        QByteArray rest = line.mid(16);
        int spaceIdx = rest.indexOf(' ');
        if (spaceIdx < 0) {
            return std::nullopt;
        }
        int windowId = parseWindowId(rest.left(spaceIdx));
        QString name = QString::fromUtf8(rest.mid(spaceIdx + 1));
        return TmuxWindowRenamedNotification{windowId, name};

    } else if (line.startsWith("%window-pane-changed ")) {
        QList<QByteArray> parts = line.mid(21).split(' ');
        if (parts.size() < 2) {
            return std::nullopt;
        }
        int windowId = parseWindowId(parts[0]);
        int paneId = parsePaneId(parts[1]);
        return TmuxWindowPaneChangedNotification{windowId, paneId};

    } else if (line.startsWith("%session-changed ")) {
        QByteArray rest = line.mid(17);
        int spaceIdx = rest.indexOf(' ');
        if (spaceIdx < 0) {
            return std::nullopt;
        }
        int sessionId = parseSessionId(rest.left(spaceIdx));
        QString name = QString::fromUtf8(rest.mid(spaceIdx + 1));
        return TmuxSessionChangedNotification{sessionId, name};

    } else if (line.startsWith("%session-renamed ")) {
        QString name = QString::fromUtf8(line.mid(17));
        return TmuxSessionRenamedNotification{name};

    } else if (line.startsWith("%sessions-changed")) {
        return TmuxSessionsChangedNotification{};

    } else if (line.startsWith("%session-window-changed ")) {
        QList<QByteArray> parts = line.mid(24).split(' ');
        if (parts.size() < 2) {
            return std::nullopt;
        }
        int sessionId = parseSessionId(parts[0]);
        int windowId = parseWindowId(parts[1]);
        return TmuxSessionWindowChangedNotification{sessionId, windowId};

    } else if (line.startsWith("%pause ")) {
        int paneId = parsePaneId(line.mid(7));
        return TmuxPanePausedNotification{paneId};

    } else if (line.startsWith("%continue ")) {
        int paneId = parsePaneId(line.mid(10));
        return TmuxPaneContinuedNotification{paneId};

    } else if (line.startsWith("%client-session-changed ")) {
        // %client-session-changed clientname $sessionId sessionname
        QList<QByteArray> parts = line.mid(24).split(' ');
        if (parts.size() < 3) {
            return std::nullopt;
        }
        QString clientName = QString::fromUtf8(parts[0]);
        int sessionId = parseSessionId(parts[1]);
        QString sessionName = QString::fromUtf8(parts[2]);
        return TmuxClientSessionChangedNotification{clientName, sessionId, sessionName};

    } else if (line.startsWith("%client-detached ")) {
        QString clientName = QString::fromUtf8(line.mid(17));
        return TmuxClientDetachedNotification{clientName};

    } else if (line.startsWith("%exit")) {
        QString reason;
        if (line.length() > 6) {
            reason = QString::fromUtf8(line.mid(6));
        }
        return TmuxExitNotification{reason};
    }

    return std::nullopt;
}

void TmuxGateway::handleNotification(const QByteArray &line)
{
    auto notification = parseNotification(line);
    if (!notification.has_value()) {
        return;
    }
    // Log everything except %output (too noisy)
    if (!line.startsWith("%output ")) {
        qCDebug(KonsoleTmuxGateway) << "notification:" << line;
    }

    std::visit(
        [this](auto &&n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, TmuxOutputNotification>) {
                Q_EMIT outputReceived(n.paneId, n.data);
            } else if constexpr (std::is_same_v<T, TmuxLayoutChangedNotification>) {
                Q_EMIT layoutChanged(n.windowId, n.layout, n.visibleLayout, n.zoomed);
            } else if constexpr (std::is_same_v<T, TmuxWindowAddedNotification>) {
                Q_EMIT windowAdded(n.windowId);
            } else if constexpr (std::is_same_v<T, TmuxWindowClosedNotification>) {
                Q_EMIT windowClosed(n.windowId);
            } else if constexpr (std::is_same_v<T, TmuxWindowRenamedNotification>) {
                Q_EMIT windowRenamed(n.windowId, n.name);
            } else if constexpr (std::is_same_v<T, TmuxWindowPaneChangedNotification>) {
                Q_EMIT windowPaneChanged(n.windowId, n.paneId);
            } else if constexpr (std::is_same_v<T, TmuxSessionChangedNotification>) {
                Q_EMIT sessionChanged(n.sessionId, n.name);
            } else if constexpr (std::is_same_v<T, TmuxSessionRenamedNotification>) {
                Q_EMIT sessionRenamed(n.name);
            } else if constexpr (std::is_same_v<T, TmuxSessionsChangedNotification>) {
                Q_EMIT sessionsChanged();
            } else if constexpr (std::is_same_v<T, TmuxSessionWindowChangedNotification>) {
                Q_EMIT sessionWindowChanged(n.sessionId, n.windowId);
            } else if constexpr (std::is_same_v<T, TmuxPanePausedNotification>) {
                Q_EMIT panePaused(n.paneId);
            } else if constexpr (std::is_same_v<T, TmuxPaneContinuedNotification>) {
                Q_EMIT paneContinued(n.paneId);
            } else if constexpr (std::is_same_v<T, TmuxClientSessionChangedNotification>) {
                Q_EMIT clientSessionChanged(n.clientName, n.sessionId, n.sessionName);
            } else if constexpr (std::is_same_v<T, TmuxClientDetachedNotification>) {
                Q_EMIT clientDetached(n.clientName);
            } else if constexpr (std::is_same_v<T, TmuxExitNotification>) {
                _exited = true;
                Q_EMIT exitReceived(n.reason);
            }
        },
        notification.value());
}

void TmuxGateway::finishCurrentCommand(bool success)
{
    _inResponseBlock = false;
    qCDebug(KonsoleTmuxGateway) << "finishCommand:" << (success ? "OK" : "FAIL") << "cmd=" << _currentCommand.command
                                << "response=" << _currentCommand.response.left(200);
    if (_currentCommand.callback) {
        _currentCommand.callback(success, _currentCommand.response);
    }
    _currentCommand = PendingCommand();
}

void TmuxGateway::sendCommand(const TmuxCommand &command, CommandCallback callback)
{
    QString commandStr = command.build();

    if (_exited) {
        qCDebug(KonsoleTmuxGateway) << "sendCommand: DROPPED (exited):" << commandStr;
        if (callback) {
            callback(false, QString());
        }
        return;
    }

    qCDebug(KonsoleTmuxGateway) << "sendCommand:" << commandStr << "(queue depth:" << _pendingCommands.size() << ")";

    PendingCommand cmd;
    cmd.command = commandStr;
    cmd.callback = std::move(callback);
    _pendingCommands.enqueue(cmd);

    QByteArray data = commandStr.toUtf8() + '\n';
    writeToGateway(data);
}

void TmuxGateway::sendKeys(int paneId, const QByteArray &data)
{
    // Split into runs of literal-safe characters vs. hex-encoded characters.
    // Literal-safe: ASCII alphanumeric, selected special chars (following iTerm2),
    // and UTF-8 multi-byte sequences (e.g. Cyrillic, CJK, emoji).
    auto isAsciiLiteral = [](char c) -> bool {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == ')' || c == ':' || c == ',' || c == '_';
    };

    auto isUtf8LeadByte = [](unsigned char c) -> bool {
        return c >= 0xC0 && c <= 0xF4;
    };

    auto isUtf8ContinuationByte = [](unsigned char c) -> bool {
        return (c & 0xC0) == 0x80;
    };

    // Return the expected byte count for a UTF-8 lead byte, or 0 if invalid.
    auto utf8SequenceLength = [](unsigned char c) -> int {
        if (c < 0xC0)
            return 0;
        if (c < 0xE0)
            return 2;
        if (c < 0xF0)
            return 3;
        if (c <= 0xF4)
            return 4;
        return 0;
    };

    int i = 0;
    while (i < data.size()) {
        unsigned char byte = static_cast<unsigned char>(data[i]);
        if (isAsciiLiteral(data[i]) || isUtf8LeadByte(byte)) {
            // Collect run of literal ASCII characters and UTF-8 sequences (max 1000 bytes)
            QByteArray literal;
            while (i < data.size() && literal.size() < 1000) {
                unsigned char b = static_cast<unsigned char>(data[i]);
                if (isAsciiLiteral(data[i])) {
                    literal.append(data[i]);
                    i++;
                } else if (isUtf8LeadByte(b)) {
                    int seqLen = utf8SequenceLength(b);
                    // Verify we have enough bytes and they are valid continuation bytes
                    if (i + seqLen > data.size()) {
                        break;
                    }
                    bool valid = true;
                    for (int j = 1; j < seqLen; j++) {
                        if (!isUtf8ContinuationByte(static_cast<unsigned char>(data[i + j]))) {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) {
                        break;
                    }
                    literal.append(data.mid(i, seqLen));
                    i += seqLen;
                } else {
                    break;
                }
            }
            sendCommand(TmuxCommand(QStringLiteral("send-keys")).flag(QStringLiteral("-l")).paneTarget(paneId).arg(QString::fromUtf8(literal)));
        } else {
            // Collect run of hex-encoded characters (max 125)
            QStringList hexParts;
            while (i < data.size() && !isAsciiLiteral(data[i]) && !isUtf8LeadByte(static_cast<unsigned char>(data[i])) && hexParts.size() < 125) {
                unsigned char b = static_cast<unsigned char>(data[i]);
                if (b == 0) {
                    hexParts.append(QStringLiteral("C-Space"));
                } else {
                    hexParts.append(QStringLiteral("0x%1").arg(b, 0, 16));
                }
                i++;
            }
            sendCommand(TmuxCommand(QStringLiteral("send-keys"))
                            .paneTarget(paneId)
                            .arg(hexParts.join(QLatin1Char(' '))));
        }
    }
}

void TmuxGateway::detach()
{
    sendCommand(TmuxCommand(QStringLiteral("detach")));
}

QByteArray TmuxGateway::decodeOctalEscapes(const QByteArray &encoded)
{
    QByteArray result;
    result.reserve(encoded.size());

    int i = 0;
    while (i < encoded.size()) {
        char c = encoded[i];
        if (c == '\\' && i + 3 < encoded.size()) {
            // Try to read 3 octal digits
            int value = 0;
            bool valid = true;
            int j = i + 1;
            int digits = 0;
            while (digits < 3 && j < encoded.size()) {
                char d = encoded[j];
                if (d == '\r') {
                    // Skip carriage returns (tmux line driver artifact)
                    j++;
                    continue;
                }
                if (d < '0' || d > '7') {
                    valid = false;
                    break;
                }
                value = value * 8 + (d - '0');
                digits++;
                j++;
            }
            if (valid && digits == 3) {
                result.append(static_cast<char>(value));
                i = j;
            } else {
                result.append('?');
                i++;
            }
        } else if (static_cast<unsigned char>(c) < ' ' && c != '\t') {
            // Skip control characters (but not high bytes from UTF-8)
            i++;
        } else {
            result.append(c);
            i++;
        }
    }

    return result;
}

QByteArray TmuxGateway::decodeVisEncoded(const QByteArray &encoded)
{
    QByteArray result;
    result.reserve(encoded.size());

    int i = 0;
    while (i < encoded.size()) {
        char c = encoded[i];
        if (c == '\\' && i + 1 < encoded.size()) {
            char next = encoded[i + 1];
            // C-style escapes (VIS_CSTYLE)
            if (next == 'n') {
                result.append('\n');
                i += 2;
            } else if (next == 'r') {
                result.append('\r');
                i += 2;
            } else if (next == 't') {
                result.append('\t');
                i += 2;
            } else if (next == 'b') {
                result.append('\b');
                i += 2;
            } else if (next == 'a') {
                result.append('\a');
                i += 2;
            } else if (next == 'v') {
                result.append('\v');
                i += 2;
            } else if (next == 'f') {
                result.append('\f');
                i += 2;
            } else if (next == '0' && i + 3 < encoded.size() && encoded[i + 2] == '0' && encoded[i + 3] == '0') {
                result.append('\0');
                i += 4;
            } else if (next == '\\') {
                result.append('\\');
                i += 2;
            } else if (next >= '0' && next <= '7') {
                // Octal escape: \ddd (skip \r from line driver)
                int value = 0;
                int j = i + 1;
                int digits = 0;
                while (digits < 3 && j < encoded.size()) {
                    char d = encoded[j];
                    if (d == '\r') {
                        j++;
                        continue;
                    }
                    if (d < '0' || d > '7') {
                        break;
                    }
                    value = value * 8 + (d - '0');
                    digits++;
                    j++;
                }
                if (digits == 3) {
                    result.append(static_cast<char>(value));
                    i = j;
                } else {
                    result.append(c);
                    i++;
                }
            } else {
                result.append(c);
                i++;
            }
        } else {
            result.append(c);
            i++;
        }
    }

    return result;
}

int TmuxGateway::parsePaneId(const QByteArray &token)
{
    // Format: %<number>
    if (token.isEmpty() || token[0] != '%') {
        return -1;
    }
    bool ok = false;
    int id = token.mid(1).toInt(&ok);
    return ok ? id : -1;
}

int TmuxGateway::parseWindowId(const QByteArray &token)
{
    // Format: @<number>
    QByteArray trimmed = token.trimmed();
    if (trimmed.isEmpty() || trimmed[0] != '@') {
        return -1;
    }
    bool ok = false;
    int id = trimmed.mid(1).toInt(&ok);
    return ok ? id : -1;
}

int TmuxGateway::parseSessionId(const QByteArray &token)
{
    // Format: $<number>
    if (token.isEmpty() || token[0] != '$') {
        return -1;
    }
    bool ok = false;
    int id = token.mid(1).toInt(&ok);
    return ok ? id : -1;
}

void TmuxGateway::writeToGateway(const QByteArray &data)
{
    if (_writeCallback) {
        _writeCallback(data);
    }
}

} // namespace Konsole
