/*
    SPDX-FileCopyrightText: 2007-2008 Robert Knight <robertknight@gmail.com>
    SPDX-FileCopyrightText: 1997, 1998 Lars Doelle <lars.doelle@on-line.de>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "EscapeSequenceUrlExtractor.h"
#include "Screen.h"

#include <QHostInfo>
#include <QUrl>

namespace Konsole
{
EscapeSequenceUrlExtractor::EscapeSequenceUrlExtractor() = default;

void Konsole::EscapeSequenceUrlExtractor::setScreen(Konsole::Screen *screen)
{
    _screen = screen;
    clear();
}

void EscapeSequenceUrlExtractor::appendUrlText_impl(char32_t c)
{
    if (_currentUrl.text.size() >= MaximumTextLength) {
        abortUrlInput();
        return;
    }
    if (_currentUrl.text.isEmpty()) {
        // We need to  on getCursorX because we want the position of the
        // last printed character, not the cursor.
        const int realCcolumn = _screen->getCursorY() + _screen->getHistLines();
        _currentUrl.begin = Coordinate{realCcolumn, _screen->getCursorX() - 1};
    }
    const char32_t c32 = c;
    _currentUrl.text += QString::fromUcs4(&c32, 1);
}

void EscapeSequenceUrlExtractor::setUrl(const QString &url)
{
    if (_reading) {
        endUrlInput();
    }
    if (url.isEmpty() || url.size() > MaximumUrlLength) {
        return;
    }

    QUrl qUrl(url, QUrl::StrictMode);
    if (!qUrl.isValid() || qUrl.isRelative() || !_allowedUriSchemas.contains(qUrl.scheme() + QLatin1String("://")) || !qUrl.userInfo().isEmpty()) {
        return;
    }

    // Reject invisible controls, including percent-encoded bidi overrides.
    const QString decoded = QUrl::fromPercentEncoding(url.toUtf8());
    for (const QChar c : decoded) {
        if (c.category() == QChar::Other_Control || c.category() == QChar::Other_Format) {
            return;
        }
    }

    if (qUrl.scheme() == QLatin1String("http") || qUrl.scheme() == QLatin1String("https")) {
        if (qUrl.host().isEmpty()) {
            return;
        }
    } else if (qUrl.scheme() == QLatin1String("file")) {
        if ((!qUrl.host().isEmpty() && qUrl.host() != QHostInfo::localHostName() && qUrl.host() != QLatin1String("localhost"))
            || !qUrl.path().startsWith(QLatin1Char('/')) || qUrl.path().startsWith(QLatin1String("//")) || qUrl.port() != -1) {
            return;
        }
        qUrl.setHost(QString());
    }

    // Keep the same unambiguous destination for hover, confirmation, and dispatch.
    _currentUrl.url = qUrl.toString(QUrl::FullyEncoded);
    _reading = true;
}

void EscapeSequenceUrlExtractor::abortUrlInput()
{
    _reading = false;
    _currentUrl = ExtractedUrl{};
}

void EscapeSequenceUrlExtractor::endUrlInput()
{
    Q_ASSERT(reading());
    _reading = false;

    const int realCcolumn = _screen->getCursorY() + _screen->getHistLines();
    const auto currentPos = Coordinate{realCcolumn, _screen->getCursorX()};
    _currentUrl.end = currentPos;
    if (!_currentUrl.text.isEmpty()) {
        if (_history.size() >= MaximumUrlHistory) {
            _history.removeFirst();
        }
        _history.append(_currentUrl);
    }

    _currentUrl = ExtractedUrl{};
}

void EscapeSequenceUrlExtractor::clear()
{
    abortUrlInput();
    _history.clear();
}

void EscapeSequenceUrlExtractor::clearBetween(int loca, int loce)
{
    _history.removeIf([&](const ExtractedUrl &url) {
        int beginLoc = url.begin.row * _screen->getColumns() + url.begin.col;
        int endLoc = url.end.row * _screen->getColumns() + url.end.col;

        return (loca <= beginLoc && beginLoc <= loce) || (loca <= endLoc && endLoc <= loce);
    });
}

void EscapeSequenceUrlExtractor::setAllowedLinkSchema(const QStringList &schema)
{
    if (_allowedUriSchemas == schema) {
        return;
    }
    clear();
    _allowedUriSchemas = schema;
}

void EscapeSequenceUrlExtractor::historyLinesRemoved(int lines)
{
    for (auto &url : _history) {
        url.begin.row -= lines;
        url.end.row -= lines;
    }
    _history.erase(std::remove_if(std::begin(_history),
                                  std::end(_history),
                                  [](const ExtractedUrl &url) {
                                      const bool toRemove = url.begin.row < 0;
                                      return toRemove;
                                  }),
                   std::end(_history));
}

QVector<ExtractedUrl> EscapeSequenceUrlExtractor::history() const
{
    return _history;
}

}

#include "moc_EscapeSequenceUrlExtractor.cpp"
