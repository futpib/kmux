/*
    SPDX-FileCopyrightText: 2007-2008 Robert Knight <robertknight@gmail.com>
    SPDX-FileCopyrightText: 2020 Tomaz Canabrava <tcanabrava@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "EscapeSequenceUrlFilter.h"

#include "EscapeSequenceUrlExtractor.h"
#include "EscapeSequenceUrlFilterHotSpot.h"
#include "session/Session.h"
#include "terminalDisplay/TerminalColor.h"
#include "terminalDisplay/TerminalDisplay.h"

using namespace Konsole;

EscapeSequenceUrlFilter::EscapeSequenceUrlFilter(Session *session, TerminalDisplay *window)
    : _session(session)
    , _window(window)
{
}

void EscapeSequenceUrlFilter::process()
{
    QPointer<ScreenWindow> sWindow = _window->screenWindow();
    if (!sWindow || !sWindow->screen() || !sWindow->screen()->urlExtractor()) {
        return;
    }

    const auto urls = sWindow->screen()->urlExtractor()->history();
    const auto *image = sWindow->getImage();
    const int columns = sWindow->windowColumns();
    const auto *colors = _window->terminalColor()->colorTable();

    for (const auto &escapedUrl : urls) {
        if (escapedUrl.begin.row < sWindow->currentLine() || escapedUrl.end.row >= sWindow->currentLine() + sWindow->windowLines()
            || escapedUrl.begin.row > escapedUrl.end.row || escapedUrl.begin.col < 0 || escapedUrl.begin.col >= columns || escapedUrl.end.col < 0
            || escapedUrl.end.col > columns) {
            continue;
        }

        const int beginRow = escapedUrl.begin.row - sWindow->currentLine();
        const int endRow = escapedUrl.end.row - sWindow->currentLine();
        // Compare the destination with the cells currently displayed, since
        // later output may overwrite or conceal the original link label.
        QString visibleText;
        for (int row = beginRow; row <= endRow; ++row) {
            const int firstColumn = row == beginRow ? escapedUrl.begin.col : 0;
            const int lastColumn = row == endRow ? escapedUrl.end.col : columns;
            for (int column = firstColumn; column < lastColumn; ++column) {
                const auto &cell = image[row * columns + column];
                const bool plainAscii = !cell.rendition.f.conceal && !cell.rendition.f.blink && !cell.rendition.f.extended && cell.character < 128
                    && cell.foregroundColor.color(colors) != cell.backgroundColor.color(colors);
                visibleText += plainAscii ? QChar(ushort(cell.character)) : QChar(QChar::ReplacementCharacter);
            }
        }
        QSharedPointer<HotSpot> spot(
            // TODO:
            // This uses Column / Row while everything else uses Row/Column.
            // Move everything else to QPoint begin / QPoint End.
            new EscapeSequenceUrlHotSpot(beginRow, escapedUrl.begin.col, endRow, escapedUrl.end.col, visibleText, escapedUrl.url));

        addHotSpot(spot);
    }
}
