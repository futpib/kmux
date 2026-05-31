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

    // Clear any garbled content from %output that arrived before the
    // emulation was sized correctly
    static const char clearSeq[] = "\033[2J\033[H";
    session->injectData(clearSeq, sizeof(clearSeq) - 1);

    QStringList lines = response.split(QLatin1Char('\n'));

    // Do NOT trim trailing blank lines. `capture-pane -p -S -` returns the full
    // scrollback plus the visible screen (history + pane_height rows); the blank
    // rows below the cursor are part of the visible screen, not spurious padding.
    // Injecting every row makes the last pane_height rows the visible screen —
    // matching tmux exactly — so the absolute cursor position restored by
    // applyPaneState() lands on the right row. Trimming those blanks shifted the
    // content up relative to the cursor on an over-tall capture: the prompt/input
    // line was pushed to the bottom while the cursor stayed on its original row,
    // so typed input rendered on the wrong row (e.g. on a TUI's box border).

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
