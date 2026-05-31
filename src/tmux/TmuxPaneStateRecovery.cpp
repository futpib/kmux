/*
    SPDX-FileCopyrightText: 2025 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxPaneStateRecovery.h"

#include "TmuxCommand.h"
#include "TmuxGateway.h"
#include "TmuxPaneManager.h"

#include "Emulation.h"
#include "session/VirtualSession.h"

namespace Konsole
{

TmuxPaneStateRecovery::TmuxPaneStateRecovery(TmuxGateway *gateway, TmuxPaneManager *paneManager, QObject *parent)
    : QObject(parent)
    , _gateway(gateway)
    , _paneManager(paneManager)
{
}

namespace
{
TmuxFormatSpec paneStateSpec()
{
    return TmuxFormatSpec({
        QStringLiteral("pane_id"),
        QStringLiteral("alternate_on"),
        QStringLiteral("cursor_x"),
        QStringLiteral("cursor_y"),
        QStringLiteral("scroll_region_upper"),
        QStringLiteral("scroll_region_lower"),
        QStringLiteral("cursor_flag"),
        QStringLiteral("insert_flag"),
        QStringLiteral("keypad_cursor_flag"),
        QStringLiteral("keypad_flag"),
        QStringLiteral("wrap_flag"),
        QStringLiteral("mouse_standard_flag"),
        QStringLiteral("mouse_button_flag"),
        QStringLiteral("mouse_any_flag"),
        QStringLiteral("mouse_sgr_flag"),
    });
}
} // namespace

void TmuxPaneStateRecovery::queryPaneStates(int windowId)
{
    TmuxFormatSpec spec = paneStateSpec();
    _gateway->sendCommand(TmuxCommand(QStringLiteral("list-panes")).windowTarget(windowId).format(spec),
                          [this, windowId, spec](bool success, const QString &response) {
                              handlePaneStateResponse(windowId, success, response, spec);
                          });
}

void TmuxPaneStateRecovery::handlePaneStateResponse(int windowId, bool success, const QString &response, const TmuxFormatSpec &spec)
{
    Q_UNUSED(windowId)

    if (!success || response.isEmpty()) {
        return;
    }

    for (const auto &row : spec.parseRows(response)) {
        const QString paneIdStr = row.value(QStringLiteral("pane_id"));
        if (!paneIdStr.startsWith(QLatin1Char('%'))) {
            continue;
        }
        int paneId = paneIdStr.mid(1).toInt();

        TmuxPaneState state;
        state.paneId = paneId;
        state.alternateOn = row.value(QStringLiteral("alternate_on")) == QLatin1String("1");
        state.cursorX = row.value(QStringLiteral("cursor_x")).toInt();
        state.cursorY = row.value(QStringLiteral("cursor_y")).toInt();
        state.scrollRegionUpper = row.value(QStringLiteral("scroll_region_upper")).toInt();
        state.scrollRegionLower = row.value(QStringLiteral("scroll_region_lower")).toInt();
        state.cursorVisible = row.value(QStringLiteral("cursor_flag")) == QLatin1String("1");
        state.insertMode = row.value(QStringLiteral("insert_flag")) == QLatin1String("1");
        state.appCursorKeys = row.value(QStringLiteral("keypad_cursor_flag")) == QLatin1String("1");
        state.appKeypad = row.value(QStringLiteral("keypad_flag")) == QLatin1String("1");
        state.wrapMode = row.value(QStringLiteral("wrap_flag")) == QLatin1String("1");
        state.mouseStandard = row.value(QStringLiteral("mouse_standard_flag")) == QLatin1String("1");
        state.mouseButton = row.value(QStringLiteral("mouse_button_flag")) == QLatin1String("1");
        state.mouseAny = row.value(QStringLiteral("mouse_any_flag")) == QLatin1String("1");
        state.mouseSGR = row.value(QStringLiteral("mouse_sgr_flag")) == QLatin1String("1");

        _paneStates[paneId] = state;
    }
}

void TmuxPaneStateRecovery::setPaneDimensions(int paneId, int width, int height)
{
    _paneDimensions[paneId] = qMakePair(width, height);
}

void TmuxPaneStateRecovery::capturePaneHistory(int paneId)
{
    _pendingCapture.insert(paneId);
    _gateway->sendCommand(TmuxCommand(QStringLiteral("capture-pane"))
                              .flag(QStringLiteral("-p"))
                              .flag(QStringLiteral("-J"))
                              .flag(QStringLiteral("-e"))
                              .paneTarget(paneId)
                              .flag(QStringLiteral("-S"))
                              .arg(QStringLiteral("-")),
                          [this, paneId](bool success, const QString &response) {
                              handleCapturePaneResponse(paneId, success, response);
                          });
}

bool TmuxPaneStateRecovery::isPendingCapture(int paneId) const
{
    return _pendingCapture.contains(paneId);
}

void TmuxPaneStateRecovery::handleCapturePaneResponse(int paneId, bool success, const QString &response)
{
    _pendingCapture.remove(paneId);

    if (!success || response.isEmpty()) {
        Q_EMIT paneRecoveryComplete(paneId);
        return;
    }

    auto *session = qobject_cast<VirtualSession *>(_paneManager->sessionForPane(paneId));
    if (!session) {
        Q_EMIT paneRecoveryComplete(paneId);
        return;
    }

    // Set the emulation screen size to match the tmux pane dimensions
    // before injecting content, so long lines wrap at the correct column
    if (_paneDimensions.contains(paneId)) {
        auto dims = _paneDimensions.take(paneId);
        session->emulation()->setImageSize(dims.second, dims.first);
    }

    // If the pane is on the alternate screen (a full-screen TUI like htop/vim),
    // capture-pane returns the *alternate* screen's contents — so we must switch
    // to the alternate screen BEFORE injecting the captured frame. Entering the
    // alternate screen clears it, so doing this after injection (as
    // applyPaneState used to) would wipe the frame we just painted and leave the
    // active screen blank — the TUI then only ever shows subsequent %output
    // diffs on an empty screen. Entering it first means the frame lands on the
    // alternate screen where it belongs.
    const bool alternateOn = _paneStates.contains(paneId) && _paneStates[paneId].alternateOn;
    if (alternateOn) {
        static const char altScreenSeq[] = "\033[?1049h";
        session->injectData(altScreenSeq, sizeof(altScreenSeq) - 1);
    }

    // Clear before injecting the captured frame. Recovery can run more than
    // once for the same pane on a single attach: initialize() runs, then tmux's
    // %session-changed re-runs it, so handleCapturePaneResponse fires twice.
    // \033[2J\033[H alone clears only the visible screen, leaving the first
    // injection's lines in scrollback — the second injection then stacks on top
    // of them, pushing content down so the restored cursor row no longer matches
    // tmux's cursor_y (the off-by-one that makes typed input land on the wrong
    // row). \033[3J also clears the scrollback history, making re-injection a
    // full replace and recovery idempotent.
    static const char clearSeq[] = "\033[3J\033[2J\033[H";
    session->injectData(clearSeq, sizeof(clearSeq) - 1);

    QStringList lines = response.split(QLatin1Char('\n'));

    // Trim trailing empty lines — capture-pane pads to the pane height
    // which would push real content off-screen
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) {
        lines.removeLast();
    }

    for (int i = 0; i < lines.size(); ++i) {
        QByteArray lineData = lines[i].toUtf8();
        if (i < lines.size() - 1) {
            lineData.append("\r\n");
        }
        session->injectData(lineData.constData(), lineData.size());
    }

    applyPaneState(paneId);
    Q_EMIT paneRecoveryComplete(paneId);
}

void TmuxPaneStateRecovery::applyPaneState(int paneId)
{
    if (!_paneStates.contains(paneId)) {
        return;
    }

    auto *session = qobject_cast<VirtualSession *>(_paneManager->sessionForPane(paneId));
    if (!session) {
        return;
    }

    const TmuxPaneState &state = _paneStates[paneId];
    QByteArray seq;

    // Note: entering the alternate screen (\033[?1049h) is NOT done here. For an
    // alternate-screen pane handleCapturePaneResponse switches to it *before*
    // injecting the captured frame, because \033[?1049h clears the screen — doing
    // it here (after injection) would wipe the recovered frame. By the time this
    // runs we're already on the correct screen; only cursor/scroll/mode state
    // remains to restore.

    if (state.scrollRegionUpper != 0 || state.scrollRegionLower != -1) {
        int lower = state.scrollRegionLower;
        if (lower < 0) {
            lower = 9999;
        }
        seq.append(QStringLiteral("\033[%1;%2r").arg(state.scrollRegionUpper + 1).arg(lower + 1).toUtf8());
    }

    seq.append(QStringLiteral("\033[%1;%2H").arg(state.cursorY + 1).arg(state.cursorX + 1).toUtf8());

    if (!state.cursorVisible) {
        seq.append("\033[?25l");
    }

    if (state.insertMode) {
        seq.append("\033[4h");
    }

    if (state.appCursorKeys) {
        seq.append("\033[?1h");
    }

    if (state.appKeypad) {
        seq.append("\033=");
    }

    if (!state.wrapMode) {
        seq.append("\033[?7l");
    }

    if (state.mouseStandard) {
        seq.append("\033[?1000h");
    }
    if (state.mouseButton) {
        seq.append("\033[?1002h");
    }
    if (state.mouseAny) {
        seq.append("\033[?1003h");
    }
    if (state.mouseSGR) {
        seq.append("\033[?1006h");
    }

    if (!seq.isEmpty()) {
        session->injectData(seq.constData(), seq.size());
    }

    _paneStates.remove(paneId);
}

void TmuxPaneStateRecovery::clear()
{
    _paneStates.clear();
}

} // namespace Konsole

#include "moc_TmuxPaneStateRecovery.cpp"
