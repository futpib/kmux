/*
    SPDX-FileCopyrightText: 2026 Konsole contributors

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "TmuxFormatSpecTest.h"

#include <QRegularExpression>
#include <QTest>

#include "../tmux/TmuxFormatSpec.h"

using namespace Konsole;

namespace
{
// Pull the boundary nonces out of the format string so tests can simulate
// what tmux would emit without coupling to TmuxFormatSpec's RNG.
struct Boundaries {
    QString fieldSep;
    QString recordSep;
};

Boundaries boundariesFor(const TmuxFormatSpec &spec)
{
    const QString fmt = spec.tmuxFormatString();
    QRegularExpression re(QStringLiteral("(__KMUX_FS_[0-9a-f]+__).*?(__KMUX_RS_[0-9a-f]+__)$"));
    auto m = re.match(fmt);
    Q_ASSERT(m.hasMatch());
    return {m.captured(1), m.captured(2)};
}

QString simulateTmuxResponse(const TmuxFormatSpec &spec, const QList<QStringList> &rows)
{
    const auto b = boundariesFor(spec);
    QString out;
    for (const auto &row : rows) {
        for (int i = 0; i < row.size(); ++i) {
            if (i > 0) {
                out += b.fieldSep;
            }
            out += row[i];
        }
        out += b.recordSep;
        // tmux's `-F` appends a newline after each record; mimic that so the
        // parser exercises its leading-newline-on-next-record stripping path.
        out += QLatin1Char('\n');
    }
    return out;
}
} // namespace

void TmuxFormatSpecTest::testFormatStringEmbedsFieldsBetweenBoundaries()
{
    TmuxFormatSpec spec({QStringLiteral("session_id"), QStringLiteral("session_name"), QStringLiteral("pane_active")});
    const QString fmt = spec.tmuxFormatString();

    // Each tmux variable gets its `#{...}` braces back.
    QVERIFY(fmt.contains(QStringLiteral("#{session_id}")));
    QVERIFY(fmt.contains(QStringLiteral("#{session_name}")));
    QVERIFY(fmt.contains(QStringLiteral("#{pane_active}")));

    // Boundary nonces are present and follow the expected shape.
    QVERIFY(fmt.contains(QRegularExpression(QStringLiteral("__KMUX_FS_[0-9a-f]{8}__"))));
    QVERIFY(fmt.contains(QRegularExpression(QStringLiteral("__KMUX_RS_[0-9a-f]{8}__"))));

    // Boundary must contain no characters that tmux interprets specially in a
    // double-quoted -F argument or in its format-string evaluator.
    QVERIFY(!fmt.contains(QLatin1Char('"')));
    QVERIFY(!fmt.contains(QLatin1Char('\\')));
}

void TmuxFormatSpecTest::testParseRowsRoundtripsValues()
{
    TmuxFormatSpec spec({QStringLiteral("session_id"), QStringLiteral("session_name"), QStringLiteral("pane_title")});
    const QString resp = simulateTmuxResponse(spec,
                                              {{QStringLiteral("$0"), QStringLiteral("slopd"), QStringLiteral("Claude Code")},
                                               {QStringLiteral("$1"), QStringLiteral("scratch"), QStringLiteral("bash")}});

    const auto rows = spec.parseRows(resp);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].value(QStringLiteral("session_id")), QStringLiteral("$0"));
    QCOMPARE(rows[0].value(QStringLiteral("session_name")), QStringLiteral("slopd"));
    QCOMPARE(rows[0].value(QStringLiteral("pane_title")), QStringLiteral("Claude Code"));
    QCOMPARE(rows[1].value(QStringLiteral("session_id")), QStringLiteral("$1"));
    QCOMPARE(rows[1].value(QStringLiteral("pane_title")), QStringLiteral("bash"));
}

void TmuxFormatSpecTest::testParseRowsSurvivesTabInValue()
{
    // The whole point of TmuxFormatSpec: if the user's pane title contains a
    // literal tab byte (the original kmux bug), the schema must not shift.
    TmuxFormatSpec spec({QStringLiteral("session_id"), QStringLiteral("pane_title"), QStringLiteral("pane_active")});
    const QString tabbedTitle = QStringLiteral("\t Claude Code");
    const QString resp = simulateTmuxResponse(spec, {{QStringLiteral("$0"), tabbedTitle, QStringLiteral("1")}});

    const auto rows = spec.parseRows(resp);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].value(QStringLiteral("session_id")), QStringLiteral("$0"));
    QCOMPARE(rows[0].value(QStringLiteral("pane_title")), tabbedTitle);
    QCOMPARE(rows[0].value(QStringLiteral("pane_active")), QStringLiteral("1"));
}

void TmuxFormatSpecTest::testParseRowsSurvivesNewlineInValue()
{
    // A newline inside a field value must not cause the parser to think it's
    // hit a record boundary — tmux's automatic per-record `\n` is handled
    // separately by the explicit record-separator nonce.
    TmuxFormatSpec spec({QStringLiteral("window_id"), QStringLiteral("pane_title")});
    const QString multilineTitle = QStringLiteral("line one\nline two");
    const QString resp = simulateTmuxResponse(spec, {{QStringLiteral("@1"), multilineTitle}, {QStringLiteral("@2"), QStringLiteral("plain")}});

    const auto rows = spec.parseRows(resp);
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows[0].value(QStringLiteral("pane_title")), multilineTitle);
    QCOMPARE(rows[1].value(QStringLiteral("window_id")), QStringLiteral("@2"));
}

void TmuxFormatSpecTest::testParseRowsHandlesEmptyResponse()
{
    TmuxFormatSpec spec({QStringLiteral("session_id")});
    QCOMPARE(spec.parseRows(QString()).size(), 0);
    QCOMPARE(spec.parseRows(QStringLiteral("\n")).size(), 0);
}

void TmuxFormatSpecTest::testParseRowsDropsMalformedRecords()
{
    TmuxFormatSpec spec({QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")});
    const auto b = boundariesFor(spec);

    // Build a response with one good row and one short row (only 2 fields).
    QString resp;
    resp += QStringLiteral("v1") + b.fieldSep + QStringLiteral("v2") + b.fieldSep + QStringLiteral("v3") + b.recordSep + QLatin1Char('\n');
    resp += QStringLiteral("only-two") + b.fieldSep + QStringLiteral("fields") + b.recordSep + QLatin1Char('\n');

    const auto rows = spec.parseRows(resp);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].value(QStringLiteral("c")), QStringLiteral("v3"));
}

void TmuxFormatSpecTest::testFieldsWithDuplicateBoundaryNonceAreDistinct()
{
    // Two specs constructed back-to-back must use different nonces — otherwise
    // a stale response from a previous query would parse against a new spec.
    TmuxFormatSpec a({QStringLiteral("x")});
    TmuxFormatSpec b({QStringLiteral("x")});
    QVERIFY(a.tmuxFormatString() != b.tmuxFormatString());
}

QTEST_GUILESS_MAIN(Konsole::TmuxFormatSpecTest)

#include "moc_TmuxFormatSpecTest.cpp"
