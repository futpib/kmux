/*
    SPDX-FileCopyrightText: 2018 Kurt Hindenburg <kurt.hindenburg@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "Vt102EmulationTest.h"

#include <QSignalSpy>
#include <QTest>

// The below is to verify the old #defines match the new constexprs
// Just copy/paste for now from Vt102Emulation.cpp
/* clang-format off */
#define TY_CONSTRUCT(T, A, N) (((((int)(N)) & 0xffff) << 16) | ((((int)(A)) & 0xff) << 8) | (((int)(T)) & 0xff))
#define TY_CHR()        TY_CONSTRUCT(0, 0, 0)
#define TY_CTL(A)       TY_CONSTRUCT(1, A, 0)
#define TY_ESC(A)       TY_CONSTRUCT(2, A, 0)
#define TY_ESC_CS(A, B) TY_CONSTRUCT(3, A, B)
#define TY_ESC_DE(A)    TY_CONSTRUCT(4, A, 0)
#define TY_CSI_PS(A, N) TY_CONSTRUCT(5, A, N)
#define TY_CSI_PN(A)    TY_CONSTRUCT(6, A, 0)
#define TY_CSI_PR(A, N) TY_CONSTRUCT(7, A, N)
#define TY_VT52(A)      TY_CONSTRUCT(8, A, 0)
#define TY_CSI_PG(A)    TY_CONSTRUCT(9, A, 0)
#define TY_CSI_PE(A)    TY_CONSTRUCT(10, A, 0)
/* clang-format on */

using namespace Konsole;

constexpr int token_construct(int t, int a, int n)
{
    return (((n & 0xffff) << 16) | ((a & 0xff) << 8) | (t & 0xff));
}
constexpr int token_chr()
{
    return token_construct(0, 0, 0);
}
constexpr int token_ctl(int a)
{
    return token_construct(1, a, 0);
}
constexpr int token_esc(int a)
{
    return token_construct(2, a, 0);
}
constexpr int token_esc_cs(int a, int b)
{
    return token_construct(3, a, b);
}
constexpr int token_esc_de(int a)
{
    return token_construct(4, a, 0);
}
constexpr int token_csi_ps(int a, int n)
{
    return token_construct(5, a, n);
}
constexpr int token_csi_pn(int a)
{
    return token_construct(6, a, 0);
}
constexpr int token_csi_pr(int a, int n)
{
    return token_construct(7, a, n);
}
constexpr int token_vt52(int a)
{
    return token_construct(8, a, 0);
}
constexpr int token_csi_pg(int a)
{
    return token_construct(9, a, 0);
}
constexpr int token_csi_pe(int a)
{
    return token_construct(10, a, 0);
}
constexpr int token_csi_sp(int a)
{
    return token_construct(11, a, 0);
}
constexpr int token_csi_psp(int a, int n)
{
    return token_construct(12, a, n);
}
constexpr int token_csi_pq(int a)
{
    return token_construct(13, a, 0);
}

void Vt102EmulationTest::sendAndCompare(TestEmulation *em, const char *input, size_t inputLen, const QString &expectedPrint, const QByteArray &expectedSent)
{
    em->_currentScreen->clearEntireScreen();

    em->receiveData(input, inputLen);
    QString printed = em->_currentScreen->text(0, em->_currentScreen->getColumns(), Screen::PlainText);
    printed.chop(2); // Remove trailing space and newline
    QCOMPARE(printed, expectedPrint);
    QCOMPARE(em->lastSent, expectedSent);
}

void Vt102EmulationTest::testParse()
{
    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);
    Q_ASSERT(em._currentScreen != nullptr);

    sendAndCompare(&em, "a", 1, QStringLiteral("a"), "");

    const char tertiaryDeviceAttributes[] = {0x1b, '[', '=', '0', 'c'};
    sendAndCompare(&em, tertiaryDeviceAttributes, sizeof tertiaryDeviceAttributes, QStringLiteral(""), "\033P!|7E4B4445\033\\");
}

Q_DECLARE_METATYPE(std::vector<TestEmulation::Item>)

struct ItemToString {
    QString operator()(const TestEmulation::ProcessToken &item)
    {
        return QStringLiteral("processToken(0x%0, %1, %2)").arg(QString::number(item.code, 16), QString::number(item.p), QString::number(item.q));
    }

    QString operator()(TestEmulation::ProcessSessionAttributeRequest)
    {
        return QStringLiteral("ProcessSessionAttributeRequest");
    }

    QString operator()(TestEmulation::ProcessChecksumRequest)
    {
        return QStringLiteral("ProcessChecksumRequest");
    }

    QString operator()(TestEmulation::DecodingError)
    {
        return QStringLiteral("DecodingError");
    }
};

namespace QTest
{
template <>
char *toString(const std::vector<TestEmulation::Item> &items)
{
    QStringList res;
    for (auto item : items) {
        res.append(std::visit(ItemToString{}, item));
    }
    return toString(res.join(QStringLiteral(",")));
}
}

void Vt102EmulationTest::testTokenizing_data()
{
    QTest::addColumn<QVector<uint>>("input");
    QTest::addColumn<std::vector<TestEmulation::Item>>("expectedItems");

    using ProcessToken = TestEmulation::ProcessToken;

    using C = QVector<uint>;
    using I = std::vector<TestEmulation::Item>;

    /* clang-format off */
    QTest::newRow("NUL") << C{'@' - '@'} << I{ProcessToken{token_ctl('@'), 0, 0}};
    QTest::newRow("SOH") << C{'A' - '@'} << I{ProcessToken{token_ctl('A'), 0, 0}};
    QTest::newRow("STX") << C{'B' - '@'} << I{ProcessToken{token_ctl('B'), 0, 0}};
    QTest::newRow("ETX") << C{'C' - '@'} << I{ProcessToken{token_ctl('C'), 0, 0}};
    QTest::newRow("EOT") << C{'D' - '@'} << I{ProcessToken{token_ctl('D'), 0, 0}};
    QTest::newRow("ENQ") << C{'E' - '@'} << I{ProcessToken{token_ctl('E'), 0, 0}};
    QTest::newRow("ACK") << C{'F' - '@'} << I{ProcessToken{token_ctl('F'), 0, 0}};
    QTest::newRow("BEL") << C{'G' - '@'} << I{ProcessToken{token_ctl('G'), 0, 0}};
    QTest::newRow("BS")  << C{'H' - '@'} << I{ProcessToken{token_ctl('H'), 0, 0}};
    QTest::newRow("TAB") << C{'I' - '@'} << I{ProcessToken{token_ctl('I'), 0, 0}};
    QTest::newRow("LF")  << C{'J' - '@'} << I{ProcessToken{token_ctl('J'), 0, 0}};
    QTest::newRow("VT")  << C{'K' - '@'} << I{ProcessToken{token_ctl('K'), 0, 0}};
    QTest::newRow("FF")  << C{'L' - '@'} << I{ProcessToken{token_ctl('L'), 0, 0}};
    QTest::newRow("CR")  << C{'M' - '@'} << I{ProcessToken{token_ctl('M'), 0, 0}};
    QTest::newRow("SO")  << C{'N' - '@'} << I{ProcessToken{token_ctl('N'), 0, 0}};
    QTest::newRow("SI")  << C{'O' - '@'} << I{ProcessToken{token_ctl('O'), 0, 0}};

    QTest::newRow("DLE") << C{'P' - '@'} << I{ProcessToken{token_ctl('P'), 0, 0}};
    QTest::newRow("XON") << C{'Q' - '@'} << I{ProcessToken{token_ctl('Q'), 0, 0}};
    QTest::newRow("DC2") << C{'R' - '@'} << I{ProcessToken{token_ctl('R'), 0, 0}};
    QTest::newRow("XOFF") << C{'S' - '@'} << I{ProcessToken{token_ctl('S'), 0, 0}};
    QTest::newRow("DC4") << C{'T' - '@'} << I{ProcessToken{token_ctl('T'), 0, 0}};
    QTest::newRow("NAK") << C{'U' - '@'} << I{ProcessToken{token_ctl('U'), 0, 0}};
    QTest::newRow("SYN") << C{'V' - '@'} << I{ProcessToken{token_ctl('V'), 0, 0}};
    QTest::newRow("ETB") << C{'W' - '@'} << I{ProcessToken{token_ctl('W'), 0, 0}};
    QTest::newRow("CAN") << C{'X' - '@'} << I{ProcessToken{token_ctl('X'), 0, 0}};
    QTest::newRow("EM")  << C{'Y' - '@'} << I{ProcessToken{token_ctl('Y'), 0, 0}};
    QTest::newRow("SUB") << C{'Z' - '@'} << I{ProcessToken{token_ctl('Z'), 0, 0}};
    // [ is ESC and parsed differently
    QTest::newRow("FS")  << C{'\\' - '@'} << I{ProcessToken{token_ctl('\\'), 0, 0}};
    QTest::newRow("GS")  << C{']' - '@'} << I{ProcessToken{token_ctl(']'), 0, 0}};
    QTest::newRow("RS")  << C{'^' - '@'} << I{ProcessToken{token_ctl('^'), 0, 0}};
    QTest::newRow("US")  << C{'_' - '@'} << I{ProcessToken{token_ctl('_'), 0, 0}};

    QTest::newRow("DEL") << C{127} << I{};

    const uint ESC = '\033';

    QTest::newRow("ESC 7") << C{ESC, '7'} << I{ProcessToken{token_esc('7'), 0, 0}};
    QTest::newRow("ESC 8") << C{ESC, '8'} << I{ProcessToken{token_esc('8'), 0, 0}};
    QTest::newRow("ESC D") << C{ESC, 'D'} << I{ProcessToken{token_esc('D'), 0, 0}};
    QTest::newRow("ESC E") << C{ESC, 'E'} << I{ProcessToken{token_esc('E'), 0, 0}};
    QTest::newRow("ESC H") << C{ESC, 'H'} << I{ProcessToken{token_esc('H'), 0, 0}};
    QTest::newRow("ESC M") << C{ESC, 'M'} << I{ProcessToken{token_esc('M'), 0, 0}};
    QTest::newRow("ESC Z") << C{ESC, 'Z'} << I{ProcessToken{token_esc('Z'), 0, 0}};

    QTest::newRow("ESC c") << C{ESC, 'c'} << I{ProcessToken{token_esc('c'), 0, 0}};

    QTest::newRow("ESC n") << C{ESC, 'n'} << I{ProcessToken{token_esc('n'), 0, 0}};
    QTest::newRow("ESC o") << C{ESC, 'o'} << I{ProcessToken{token_esc('o'), 0, 0}};
    QTest::newRow("ESC >") << C{ESC, '>'} << I{ProcessToken{token_esc('>'), 0, 0}};
    QTest::newRow("ESC <") << C{ESC, '<'} << I{ProcessToken{token_esc('<'), 0, 0}};
    QTest::newRow("ESC =") << C{ESC, '='} << I{ProcessToken{token_esc('='), 0, 0}};

    QTest::newRow("ESC #3") << C{ESC, '#', '3'} << I{ProcessToken{token_esc_de('3'), 0, 0}};
    QTest::newRow("ESC #4") << C{ESC, '#', '4'} << I{ProcessToken{token_esc_de('4'), 0, 0}};
    QTest::newRow("ESC #5") << C{ESC, '#', '5'} << I{ProcessToken{token_esc_de('5'), 0, 0}};
    QTest::newRow("ESC #6") << C{ESC, '#', '6'} << I{ProcessToken{token_esc_de('6'), 0, 0}};
    QTest::newRow("ESC #8") << C{ESC, '#', '8'} << I{ProcessToken{token_esc_de('8'), 0, 0}};

    QTest::newRow("ESC %G") << C{ESC, '%', 'G'} << I{ProcessToken{token_esc_cs('%', 'G'), 0, 0}};
    QTest::newRow("ESC %@") << C{ESC, '%', '@'} << I{ProcessToken{token_esc_cs('%', '@'), 0, 0}};

    QTest::newRow("ESC (0") << C{ESC, '(', '0'} << I{ProcessToken{token_esc_cs('(', '0'), 0, 0}};
    QTest::newRow("ESC (A") << C{ESC, '(', 'A'} << I{ProcessToken{token_esc_cs('(', 'A'), 0, 0}};
    QTest::newRow("ESC (B") << C{ESC, '(', 'B'} << I{ProcessToken{token_esc_cs('(', 'B'), 0, 0}};

    QTest::newRow("ESC )0") << C{ESC, ')', '0'} << I{ProcessToken{token_esc_cs(')', '0'), 0, 0}};
    QTest::newRow("ESC )A") << C{ESC, ')', 'A'} << I{ProcessToken{token_esc_cs(')', 'A'), 0, 0}};
    QTest::newRow("ESC )B") << C{ESC, ')', 'B'} << I{ProcessToken{token_esc_cs(')', 'B'), 0, 0}};

    QTest::newRow("ESC *0") << C{ESC, '*', '0'} << I{ProcessToken{token_esc_cs('*', '0'), 0, 0}};
    QTest::newRow("ESC *A") << C{ESC, '*', 'A'} << I{ProcessToken{token_esc_cs('*', 'A'), 0, 0}};
    QTest::newRow("ESC *B") << C{ESC, '*', 'B'} << I{ProcessToken{token_esc_cs('*', 'B'), 0, 0}};

    QTest::newRow("ESC +0") << C{ESC, '+', '0'} << I{ProcessToken{token_esc_cs('+', '0'), 0, 0}};
    QTest::newRow("ESC +A") << C{ESC, '+', 'A'} << I{ProcessToken{token_esc_cs('+', 'A'), 0, 0}};
    QTest::newRow("ESC +B") << C{ESC, '+', 'B'} << I{ProcessToken{token_esc_cs('+', 'B'), 0, 0}};

    QTest::newRow("ESC [8;12;45t") << C{ESC, '[', '8', ';', '1', '2', ';', '4', '5', 't'} << I{ProcessToken{token_csi_ps('t', 8), 12, 45}};
    QTest::newRow("ESC [18t")      << C{ESC, '[', '1', '8', 't'} << I{ProcessToken{token_csi_ps('t', 18), 0, 0}};
    QTest::newRow("ESC [18;1;2t")  << C{ESC, '[', '1', '8', ';', '1', ';', '2', 't'} << I{ProcessToken{token_csi_ps('t', 18), 1, 2}};

    QTest::newRow("ESC [K")  << C{ESC, '[', 'K'} << I{ProcessToken{token_csi_ps('K', 0), 0, 0}};
    QTest::newRow("ESC [0K") << C{ESC, '[', '0', 'K'} << I{ProcessToken{token_csi_ps('K', 0), 0, 0}};
    QTest::newRow("ESC [1K") << C{ESC, '[', '1', 'K'} << I{ProcessToken{token_csi_ps('K', 1), 0, 0}};

    QTest::newRow("ESC [@")   << C{ESC, '[', '@'} << I{ProcessToken{token_csi_pn('@'), 0, 0}};
    QTest::newRow("ESC [12@") << C{ESC, '[', '1', '2', '@'} << I{ProcessToken{token_csi_pn('@'), 12, 0}};
    QTest::newRow("ESC [H")   << C{ESC, '[', 'H'} << I{ProcessToken{token_csi_pn('H'), 0, 0}};
    QTest::newRow("ESC [24H") << C{ESC, '[', '2', '4', 'H'} << I{ProcessToken{token_csi_pn('H'), 24, 0}};
    QTest::newRow("ESC [32,13H") << C{ESC, '[', '3', '2', ';', '1', '3', 'H'} << I{ProcessToken{token_csi_pn('H'), 32, 13}};

    QTest::newRow("ESC [m")   << C{ESC, '[', 'm'} << I{ProcessToken{token_csi_ps('m', 0), 0, 0}};
    QTest::newRow("ESC [1m")  << C{ESC, '[', '1', 'm'} << I{ProcessToken{token_csi_ps('m', 1), 0, 0}};
    QTest::newRow("ESC [1;2m") << C{ESC, '[', '1', ';', '2', 'm'} << I{ProcessToken{token_csi_ps('m', 1), 0, 0}, ProcessToken{token_csi_ps('m', 2), 0, 0}};
    QTest::newRow("ESC [38;2;193;202;218m") << C{ESC, '[', '3', '8', ';', '2', ';', '1', '9', '3', ';', '2', '0', '2', ';', '2', '1', '8', 'm'}
                                            << I{ProcessToken{token_csi_ps('m', 38), 4, 0xC1CADA}};
    QTest::newRow("ESC [38;2;193;202;218;2m") << C{ESC, '[', '3', '8', ';', '2', ';', '1', '9', '3', ';', '2', '0', '2', ';', '2', '1', '8', ';', '2', 'm'}
                                              << I{ProcessToken{token_csi_ps('m', 38), 4, 0xC1CADA}, ProcessToken{token_csi_ps('m', 2), 0, 0}};
    QTest::newRow("ESC [38:2:193:202:218m") << C{ESC, '[', '3', '8', ':', '2', ':', '1', '9', '3', ':', '2', '0', '2', ':', '2', '1', '8', 'm'}
                                            << I{ProcessToken{token_csi_ps('m', 38), 4, 0xC1CADA}};
    QTest::newRow("ESC [38:2:193:202:218;2m") << C{ESC, '[', '3', '8', ':', '2', ':', '1', '9', '3', ':', '2', '0', '2', ':', '2', '1', '8', ';', '2', 'm'}
                                              << I{ProcessToken{token_csi_ps('m', 38), 4, 0xC1CADA}, ProcessToken{token_csi_ps('m', 2), 0, 0}};
    QTest::newRow("ESC [38:2:1:193:202:218m") << C{ESC, '[', '3', '8', ':', '2', ':', '1', ':', '1', '9', '3', ':', '2', '0', '2', ':', '2', '1', '8', 'm'}
                                              << I{ProcessToken{token_csi_ps('m', 38), 4, 0xC1CADA}};
    QTest::newRow("ESC [38;5;255;2m") << C{ESC, '[', '3', '8', ';', '5', ';', '2', '5', '5', ';', '2', 'm'}
                                      << I{ProcessToken{token_csi_ps('m', 38), 3, 255}, ProcessToken{token_csi_ps('m', 2), 0, 0}};
    QTest::newRow("ESC [38:5:255m") << C{ESC, '[', '3', '8', ':', '5', ':', '2', '5', '5', 'm'}
                                    << I{ProcessToken{token_csi_ps('m', 38), 3, 255}};

    QTest::newRow("ESC [5n")  << C{ESC, '[', '5', 'n'} << I{ProcessToken{token_csi_ps('n', 5), 0, 0}};

    QTest::newRow("ESC [?1h") << C{ESC, '[', '?', '1', 'h'} << I{ProcessToken{token_csi_pr('h', 1), 0, 0}};
    QTest::newRow("ESC [?1l") << C{ESC, '[', '?', '1', 'l'} << I{ProcessToken{token_csi_pr('l', 1), 0, 0}};
    QTest::newRow("ESC [?1r") << C{ESC, '[', '?', '1', 'r'} << I{ProcessToken{token_csi_pr('r', 1), 0, 0}};
    QTest::newRow("ESC [?1s") << C{ESC, '[', '?', '1', 's'} << I{ProcessToken{token_csi_pr('s', 1), 0, 0}};

    QTest::newRow("ESC [?1;2h") << C{ESC, '[', '?', '1', ';', '2', 'h'}
                                << I{ProcessToken{token_csi_pr('h', 1), 0, 0}, ProcessToken{token_csi_pr('h', 2), 1, 0}};
    QTest::newRow("ESC [?1;2l") << C{ESC, '[', '?', '1', ';', '2', 'l'}
                                << I{ProcessToken{token_csi_pr('l', 1), 0, 0}, ProcessToken{token_csi_pr('l', 2), 1, 0}};
    QTest::newRow("ESC [?1;2r") << C{ESC, '[', '?', '1', ';', '2', 'r'}
                                << I{ProcessToken{token_csi_pr('r', 1), 0, 0}, ProcessToken{token_csi_pr('r', 2), 1, 0}};
    QTest::newRow("ESC [?1;2s") << C{ESC, '[', '?', '1', ';', '2', 's'}
                                << I{ProcessToken{token_csi_pr('s', 1), 0, 0}, ProcessToken{token_csi_pr('s', 2), 1, 0}};

    QTest::newRow("ESC [? q")  << C{ESC, '[', ' ', 'q'} << I{ProcessToken{token_csi_sp('q'), 0, 0}};
    QTest::newRow("ESC [? 1q") << C{ESC, '[', '1', ' ', 'q'} << I{ProcessToken{token_csi_psp('q', 1), 0, 0}};

    QTest::newRow("ESC [!p") << C{ESC, '[', '!', 'p'} << I{ProcessToken{token_csi_pe('p'), 0, 0}};
    QTest::newRow("ESC [=c") << C{ESC, '[', '=', 'p'} << I{ProcessToken{token_csi_pq('p'), 0, 0}};
    QTest::newRow("ESC [>c") << C{ESC, '[', '>', 'p'} << I{ProcessToken{token_csi_pg('p'), 0, 0}};
    /* clang-format on */
}

void Vt102EmulationTest::testTokenizing()
{
    QFETCH(QVector<uint>, input);
    QFETCH(std::vector<TestEmulation::Item>, expectedItems);

    TestEmulation em;
    em.reset();
    em.blockFurtherProcessing = true;

    em._currentScreen->clearEntireScreen();

    em.receiveChars(input);
    QString printed = em._currentScreen->text(0, em._currentScreen->getColumns(), Screen::PlainText);
    printed.chop(2); // Remove trailing space and newline

    QCOMPARE(printed, QStringLiteral(""));
    QCOMPARE(em.items, expectedItems);
}

void Vt102EmulationTest::testTokenizingVT52_data()
{
    QTest::addColumn<QVector<uint>>("input");
    QTest::addColumn<std::vector<TestEmulation::Item>>("expectedItems");

    using ProcessToken = TestEmulation::ProcessToken;

    using C = QVector<uint>;
    using I = std::vector<TestEmulation::Item>;

    /* clang-format off */
    QTest::newRow("NUL") << C{'@' - '@'} << I{ProcessToken{token_ctl('@'), 0, 0}};
    QTest::newRow("SOH") << C{'A' - '@'} << I{ProcessToken{token_ctl('A'), 0, 0}};
    QTest::newRow("STX") << C{'B' - '@'} << I{ProcessToken{token_ctl('B'), 0, 0}};
    QTest::newRow("ETX") << C{'C' - '@'} << I{ProcessToken{token_ctl('C'), 0, 0}};
    QTest::newRow("EOT") << C{'D' - '@'} << I{ProcessToken{token_ctl('D'), 0, 0}};
    QTest::newRow("ENQ") << C{'E' - '@'} << I{ProcessToken{token_ctl('E'), 0, 0}};
    QTest::newRow("ACK") << C{'F' - '@'} << I{ProcessToken{token_ctl('F'), 0, 0}};
    QTest::newRow("BEL") << C{'G' - '@'} << I{ProcessToken{token_ctl('G'), 0, 0}};
    QTest::newRow("BS")  << C{'H' - '@'} << I{ProcessToken{token_ctl('H'), 0, 0}};
    QTest::newRow("TAB") << C{'I' - '@'} << I{ProcessToken{token_ctl('I'), 0, 0}};
    QTest::newRow("LF")  << C{'J' - '@'} << I{ProcessToken{token_ctl('J'), 0, 0}};
    QTest::newRow("VT")  << C{'K' - '@'} << I{ProcessToken{token_ctl('K'), 0, 0}};
    QTest::newRow("FF")  << C{'L' - '@'} << I{ProcessToken{token_ctl('L'), 0, 0}};
    QTest::newRow("CR")  << C{'M' - '@'} << I{ProcessToken{token_ctl('M'), 0, 0}};
    QTest::newRow("SO")  << C{'N' - '@'} << I{ProcessToken{token_ctl('N'), 0, 0}};
    QTest::newRow("SI")  << C{'O' - '@'} << I{ProcessToken{token_ctl('O'), 0, 0}};

    QTest::newRow("DLE") << C{'P' - '@'} << I{ProcessToken{token_ctl('P'), 0, 0}};
    QTest::newRow("XON") << C{'Q' - '@'} << I{ProcessToken{token_ctl('Q'), 0, 0}};
    QTest::newRow("DC2") << C{'R' - '@'} << I{ProcessToken{token_ctl('R'), 0, 0}};
    QTest::newRow("XOFF") << C{'S' - '@'} << I{ProcessToken{token_ctl('S'), 0, 0}};
    QTest::newRow("DC4") << C{'T' - '@'} << I{ProcessToken{token_ctl('T'), 0, 0}};
    QTest::newRow("NAK") << C{'U' - '@'} << I{ProcessToken{token_ctl('U'), 0, 0}};
    QTest::newRow("SYN") << C{'V' - '@'} << I{ProcessToken{token_ctl('V'), 0, 0}};
    QTest::newRow("ETB") << C{'W' - '@'} << I{ProcessToken{token_ctl('W'), 0, 0}};
    QTest::newRow("CAN") << C{'X' - '@'} << I{ProcessToken{token_ctl('X'), 0, 0}};
    QTest::newRow("EM")  << C{'Y' - '@'} << I{ProcessToken{token_ctl('Y'), 0, 0}};
    QTest::newRow("SUB") << C{'Z' - '@'} << I{ProcessToken{token_ctl('Z'), 0, 0}};
    // [ is ESC and parsed differently
    QTest::newRow("FS")  << C{'\\' - '@'} << I{ProcessToken{token_ctl('\\'), 0, 0}};
    QTest::newRow("GS")  << C{']' - '@'} << I{ProcessToken{token_ctl(']'), 0, 0}};
    QTest::newRow("RS")  << C{'^' - '@'} << I{ProcessToken{token_ctl('^'), 0, 0}};
    QTest::newRow("US")  << C{'_' - '@'} << I{ProcessToken{token_ctl('_'), 0, 0}};

    QTest::newRow("DEL") << C{127} << I{};

    const uint ESC = '\033';

    QTest::newRow("ESC A") << C{ESC, 'A'} << I{ProcessToken{token_vt52('A'), 0, 0}};
    QTest::newRow("ESC B") << C{ESC, 'B'} << I{ProcessToken{token_vt52('B'), 0, 0}};
    QTest::newRow("ESC C") << C{ESC, 'C'} << I{ProcessToken{token_vt52('C'), 0, 0}};
    QTest::newRow("ESC D") << C{ESC, 'D'} << I{ProcessToken{token_vt52('D'), 0, 0}};
    QTest::newRow("ESC F") << C{ESC, 'F'} << I{ProcessToken{token_vt52('F'), 0, 0}};
    QTest::newRow("ESC G") << C{ESC, 'G'} << I{ProcessToken{token_vt52('G'), 0, 0}};
    QTest::newRow("ESC H") << C{ESC, 'H'} << I{ProcessToken{token_vt52('H'), 0, 0}};
    QTest::newRow("ESC I") << C{ESC, 'I'} << I{ProcessToken{token_vt52('I'), 0, 0}};
    QTest::newRow("ESC J") << C{ESC, 'J'} << I{ProcessToken{token_vt52('J'), 0, 0}};
    QTest::newRow("ESC K") << C{ESC, 'K'} << I{ProcessToken{token_vt52('K'), 0, 0}};
    QTest::newRow("ESC Yab") << C{ESC, 'Y', 'a', 'b'} << I{ProcessToken{token_vt52('Y'), 'a', 'b'}};
    QTest::newRow("ESC Z") << C{ESC, 'Z'} << I{ProcessToken{token_vt52('Z'), 0, 0}};
    QTest::newRow("ESC <") << C{ESC, '<'} << I{ProcessToken{token_vt52('<'), 0, 0}};
    QTest::newRow("ESC =") << C{ESC, '='} << I{ProcessToken{token_vt52('='), 0, 0}};
    QTest::newRow("ESC >") << C{ESC, '>'} << I{ProcessToken{token_vt52('>'), 0, 0}};
    /* clang-format on */
}

void Vt102EmulationTest::testTokenizingVT52()
{
    QFETCH(QVector<uint>, input);
    QFETCH(std::vector<TestEmulation::Item>, expectedItems);

    TestEmulation em;
    em.reset();
    em.resetMode(MODE_Ansi);
    em.blockFurtherProcessing = true;

    em._currentScreen->clearEntireScreen();

    em.receiveChars(input);
    QString printed = em._currentScreen->text(0, em._currentScreen->getColumns(), Screen::PlainText);
    printed.chop(2); // Remove trailing space and newline

    QCOMPARE(printed, QStringLiteral(""));
    QCOMPARE(em.items, expectedItems);
}

void Vt102EmulationTest::testTokenFunctions()
{
    QCOMPARE(token_construct(0, 0, 0), TY_CONSTRUCT(0, 0, 0));
    QCOMPARE(token_chr(), TY_CHR());
    QCOMPARE(token_ctl(8 + '@'), TY_CTL(8 + '@'));
    QCOMPARE(token_ctl('G'), TY_CTL('G'));
    QCOMPARE(token_csi_pe('p'), TY_CSI_PE('p'));
    QCOMPARE(token_csi_pg('c'), TY_CSI_PG('c'));
    QCOMPARE(token_csi_pn(8), TY_CSI_PN(8));
    QCOMPARE(token_csi_pn('N'), TY_CSI_PN('N'));
    QCOMPARE(token_csi_pr('r', 2), TY_CSI_PR('r', 2));
    QCOMPARE(token_csi_pr('s', 1000), TY_CSI_PR('s', 1000));
    QCOMPARE(token_csi_ps('m', 8), TY_CSI_PS('m', 8));
    QCOMPARE(token_csi_ps('m', 48), TY_CSI_PS('m', 48));
    QCOMPARE(token_csi_ps('K', 2), TY_CSI_PS('K', 2));
    QCOMPARE(token_esc(8), TY_ESC(8));
    QCOMPARE(token_esc('='), TY_ESC('='));
    QCOMPARE(token_esc('>'), TY_ESC('>'));
    QCOMPARE(token_esc_cs(8, 0), TY_ESC_CS(8, 0));
    QCOMPARE(token_esc_cs('(', '0'), TY_ESC_CS('(', '0'));
    QCOMPARE(token_esc_cs(')', 'B'), TY_ESC_CS(')', 'B'));
    QCOMPARE(token_esc_de(8), TY_ESC_DE(8));
    QCOMPARE(token_esc_de('3'), TY_ESC_DE('3'));
    QCOMPARE(token_vt52('A'), TY_VT52('A'));
    QCOMPARE(token_vt52('Z'), TY_VT52('Z'));
    QCOMPARE(token_vt52('='), TY_VT52('='));
    QCOMPARE(token_vt52('>'), TY_VT52('>'));
}

void Vt102EmulationTest::testBufferedUpdates()
{
    TestEmulation em;
    em.reset();

    QSignalSpy outputChangedSpy(&em, &TestEmulation::outputChanged);

    // Test the normal buffered update behaviour.
    em.receiveChars({'h', 'e', 'l', 'l', 'o', '!'});

    QCOMPARE(outputChangedSpy.count(), 0);
    QVERIFY(outputChangedSpy.wait(15));
    outputChangedSpy.clear();

    const uint ESC = '\033';

    // Test that synchronized updates can time out.
    em.receiveChars({ESC, '[', '?', '2', '0', '2', '6', 'h'});

    QVERIFY(!outputChangedSpy.wait(800));
    QVERIFY(outputChangedSpy.wait(300));
    outputChangedSpy.clear();

    // Test that synchronized updates work.
    em.receiveChars({ESC, '[', '?', '2', '0', '2', '6', 'h'});
    em.receiveChars({ESC, '[', '?', '2', '0', '2', '6', 'l'});

    QCOMPARE(outputChangedSpy.count(), 2);
}

void Vt102EmulationTest::testTmuxControlModePassthrough()
{
    // Verify that entering tmux control mode (DCS 1000p) works
    // and that lines are delivered via tmuxControlModeLineReceived
    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);

    QSignalSpy startedSpy(&em, &Vt102Emulation::tmuxControlModeStarted);
    QSignalSpy lineSpy(&em, &Vt102Emulation::tmuxControlModeLineReceived);

    const uint ESC = 0x1B;

    // Enter DCS 1000p (tmux control mode)
    em.receiveChars({ESC, 'P', '1', '0', '0', '0', 'p'});
    QCOMPARE(startedSpy.count(), 1);

    // Send a tmux protocol line: "%begin 123 456 0\n"
    QVector<uint> line;
    for (char c : QByteArrayLiteral("%begin 123 456 0\n")) {
        line.append(static_cast<uint>(c));
    }
    em.receiveChars(line);
    QCOMPARE(lineSpy.count(), 1);
    QCOMPARE(lineSpy.at(0).at(0).toByteArray(), QByteArrayLiteral("%begin 123 456 0"));
}

void Vt102EmulationTest::testTmuxControlModeUtf8()
{
    // Verify that Unicode codepoints (from the post-UTF-8-decode path)
    // are re-encoded as UTF-8 when buffered in the tmux line buffer.
    // receiveChars() receives Unicode codepoints, not raw bytes.
    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);

    QSignalSpy lineSpy(&em, &Vt102Emulation::tmuxControlModeLineReceived);

    const uint ESC = 0x1B;

    // Enter tmux control mode
    em.receiveChars({ESC, 'P', '1', '0', '0', '0', 'p'});

    // Send Unicode codepoints as they would appear after UTF-8 decoding
    // ❯ = U+276F, → = U+2192, ─ = U+2500
    em.receiveChars({0x2192, ' ', 't', 'e', 's', 't', ' ', 0x2500, 0x2500, '\n'});

    QCOMPARE(lineSpy.count(), 1);
    QByteArray received = lineSpy.at(0).at(0).toByteArray();
    // Codepoints should be re-encoded as UTF-8
    // → = U+2192 = \xE2\x86\x92
    // ─ = U+2500 = \xE2\x94\x80
    QByteArray expected("\xE2\x86\x92 test \xE2\x94\x80\xE2\x94\x80");
    QCOMPARE(received, expected);
}

void Vt102EmulationTest::testTmuxControlModeUtf8ViaReceiveData()
{
    // Test the real data path: raw bytes go through receiveData() which
    // UTF-8 decodes them before passing to receiveChars(). The put()
    // function must re-encode Unicode codepoints back to UTF-8 for
    // the tmux line buffer.
    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);

    QSignalSpy lineSpy(&em, &Vt102Emulation::tmuxControlModeLineReceived);

    // Enter tmux control mode via receiveData (DCS 1000p)
    const char dcs[] = "\033P1000p";
    em.receiveData(dcs, sizeof(dcs) - 1);

    // Send a tmux protocol line containing UTF-8 text: "→ test ──\n"
    // → = U+2192 = \xE2\x86\x92
    // ─ = U+2500 = \xE2\x94\x80
    const char line[] = "\xE2\x86\x92 test \xE2\x94\x80\xE2\x94\x80\n";
    em.receiveData(line, sizeof(line) - 1);

    QCOMPARE(lineSpy.count(), 1);
    QByteArray received = lineSpy.at(0).at(0).toByteArray();
    QByteArray expected("\xE2\x86\x92 test \xE2\x94\x80\xE2\x94\x80");
    QCOMPARE(received, expected);
}

void Vt102EmulationTest::testTmuxControlModeEscInData()
{
    // Verify that ESC bytes within tmux control mode data do NOT
    // break out of DCS passthrough. Only ESC \ (ST) should terminate it.
    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);

    QSignalSpy lineSpy(&em, &Vt102Emulation::tmuxControlModeLineReceived);
    QSignalSpy endedSpy(&em, &Vt102Emulation::tmuxControlModeEnded);

    const uint ESC = 0x1B;

    // Enter tmux control mode
    em.receiveChars({ESC, 'P', '1', '0', '0', '0', 'p'});

    // Send a line with ESC sequences (like tmux %output containing terminal escape codes)
    // This simulates: %output %1 \033[0;32mhello\033[0m\n
    QVector<uint> line;
    for (char c : QByteArrayLiteral("%output %1 ")) {
        line.append(static_cast<uint>(c));
    }
    // \033[0;32m
    line.append(ESC);
    line.append('[');
    line.append('0');
    line.append(';');
    line.append('3');
    line.append('2');
    line.append('m');
    for (char c : QByteArrayLiteral("hello")) {
        line.append(static_cast<uint>(c));
    }
    // \033[0m
    line.append(ESC);
    line.append('[');
    line.append('0');
    line.append('m');
    line.append('\n');

    em.receiveChars(line);

    // Should still be in tmux control mode (ESC didn't terminate it)
    QCOMPARE(endedSpy.count(), 0);
    // The line should have been received with ESC bytes preserved
    QCOMPARE(lineSpy.count(), 1);
    QByteArray received = lineSpy.at(0).at(0).toByteArray();
    QVERIFY(received.startsWith("%output %1 "));
    QVERIFY(received.contains("\033[0;32m"));
    QVERIFY(received.contains("hello"));
    QVERIFY(received.contains("\033[0m"));
}

void Vt102EmulationTest::testTmuxControlModeC1InData()
{
    // Verify that 8-bit C1 control codes (0x90, 0x9B, 0x9D, etc.)
    // do NOT break out of DCS passthrough in tmux control mode.
    // Uses receiveData() to test the real byte-level path.
    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);

    QSignalSpy lineSpy(&em, &Vt102Emulation::tmuxControlModeLineReceived);
    QSignalSpy endedSpy(&em, &Vt102Emulation::tmuxControlModeEnded);

    // Enter tmux control mode via receiveData
    const char dcs[] = "\033P1000p";
    em.receiveData(dcs, sizeof(dcs) - 1);

    // Send a line containing C1 bytes as they would appear in the raw
    // PTY stream. These bytes (0x90, 0x9B, 0x9D, 0x98) are 8-bit C1
    // control codes but inside DCS passthrough they should be treated as data.
    // Note: the UTF-8 decoder will decode these as Unicode codepoints
    // U+0090, U+009B, U+009D, U+0098 (C2 90, C2 9B, C2 9D, C2 98 in UTF-8).
    const char line[] = "data \xC2\x90\xC2\x9B\xC2\x9D\xC2\x98\n";
    em.receiveData(line, sizeof(line) - 1);

    // Should still be in tmux control mode
    QCOMPARE(endedSpy.count(), 0);
    QCOMPARE(lineSpy.count(), 1);
    QByteArray received = lineSpy.at(0).at(0).toByteArray();
    QVERIFY(received.startsWith("data "));
    // The C1 codepoints should be re-encoded as UTF-8 (2-byte sequences)
    QCOMPARE(received, QByteArray("data \xC2\x90\xC2\x9B\xC2\x9D\xC2\x98"));
}

void Vt102EmulationTest::testTmuxControlModeST()
{
    // Verify that ESC \ (ST) correctly terminates tmux control mode
    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);

    QSignalSpy endedSpy(&em, &Vt102Emulation::tmuxControlModeEnded);

    const uint ESC = 0x1B;

    // Enter tmux control mode
    em.receiveChars({ESC, 'P', '1', '0', '0', '0', 'p'});

    // Send ESC \ (String Terminator) to exit DCS passthrough
    em.receiveChars({ESC, '\\'});

    QCOMPARE(endedSpy.count(), 1);
}

void Vt102EmulationTest::testTmuxControlModeUtf8ChunkBoundary()
{
    // Test that UTF-8 sequences split across receiveData() chunk boundaries
    // are handled correctly in tmux control mode.
    // The line "→──\n" is UTF-8: \xE2\x86\x92 \xE2\x94\x80 \xE2\x94\x80 \n
    // We split at every possible byte boundary within this data.
    const QByteArray dcs("\033P1000p");
    const QByteArray fullLine("\xE2\x86\x92\xE2\x94\x80\xE2\x94\x80\n");
    const QByteArray expected("\xE2\x86\x92\xE2\x94\x80\xE2\x94\x80");

    // Test splitting at every byte position
    for (int split = 1; split < fullLine.size(); split++) {
        TestEmulation em;
        em.reset();
        em.setCodec(TestEmulation::Utf8Codec);

        QSignalSpy lineSpy(&em, &Vt102Emulation::tmuxControlModeLineReceived);

        em.receiveData(dcs.constData(), dcs.size());

        // Send in two chunks split at position 'split'
        QByteArray chunk1 = fullLine.left(split);
        QByteArray chunk2 = fullLine.mid(split);
        em.receiveData(chunk1.constData(), chunk1.size());
        em.receiveData(chunk2.constData(), chunk2.size());

        QCOMPARE(lineSpy.count(), 1);
        QByteArray received = lineSpy.at(0).at(0).toByteArray();
        if (received != expected) {
            qWarning() << "Failed at split position" << split
                       << "chunk1:" << chunk1.toHex(' ')
                       << "chunk2:" << chunk2.toHex(' ');
        }
        QCOMPARE(received, expected);
    }
}

void Vt102EmulationTest::testTmuxControlModeUtf8OutputBoundary()
{
    // Simulate tmux splitting a UTF-8 character across two %output lines.
    // The gateway session's receiveData decodes the DCS passthrough.
    // TmuxGateway decodes octal escapes and calls injectData on the
    // virtual session. If tmux splits "→─" (UTF-8: E2 86 92 E2 94 80)
    // as two %output lines where the first has "\342\206" and the second
    // has "\222\342\224\200", the decoded bytes from each line are passed
    // to separate injectData calls, splitting the UTF-8 sequence.
    //
    // This tests the virtual session's receiveData handling of incomplete
    // UTF-8 sequences across calls.
    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);

    // Simulate two injectData calls with a UTF-8 split
    // → = U+2192 = E2 86 92
    // ─ = U+2500 = E2 94 80
    // Split after the first byte of → : chunk1 = [E2], chunk2 = [86 92 E2 94 80]
    const char chunk1[] = "\xE2";
    const char chunk2[] = "\x86\x92\xE2\x94\x80";

    em.receiveData(chunk1, sizeof(chunk1) - 1);
    em.receiveData(chunk2, sizeof(chunk2) - 1);

    // Read what was rendered on screen
    QString printed = em._currentScreen->text(0, em._currentScreen->getColumns(), Screen::PlainText);

    // Should contain → and ─
    QVERIFY2(printed.contains(QChar(0x2192)), qPrintable(QStringLiteral("Missing → (U+2192), got: ") + printed.left(20)));
    QVERIFY2(printed.contains(QChar(0x2500)), qPrintable(QStringLiteral("Missing ─ (U+2500), got: ") + printed.left(20)));
}

void Vt102EmulationTest::testTmuxControlModeRawBytePassthrough()
{
    // Test that raw UTF-8 bytes in tmux control mode are passed through
    // without lossy Unicode round-tripping. Previously, receiveData() would
    // decode raw bytes via QStringDecoder into Unicode, and put() would
    // re-encode back to UTF-8. When a PTY read split a multi-byte UTF-8
    // sequence at a chunk boundary, QStringDecoder produced U+FFFD for the
    // incomplete trailing bytes, corrupting the tmux protocol data.
    //
    // The fix: receiveRawData() intercepts raw bytes in tmux control mode
    // and passes them directly to the line buffer without Unicode round-trip.

    // Simulate: DCS entry, then two chunks where the split falls inside
    // a 3-byte UTF-8 character (╭ = U+256D = E2 95 AD)
    const QByteArray dcs("\033P1000p");

    // Full line: "╭──\n" = E2 95 AD E2 94 80 E2 94 80 0A
    // Split after first byte of ╭: chunk1 = [E2], chunk2 = [95 AD E2 94 80 E2 94 80 0A]
    const QByteArray chunk1("\xE2");
    const QByteArray chunk2("\x95\xAD\xE2\x94\x80\xE2\x94\x80\n");
    const QByteArray expected("\xE2\x95\xAD\xE2\x94\x80\xE2\x94\x80");

    TestEmulation em;
    em.reset();
    em.setCodec(TestEmulation::Utf8Codec);

    QSignalSpy lineSpy(&em, &Vt102Emulation::tmuxControlModeLineReceived);

    em.receiveData(dcs.constData(), dcs.size());
    em.receiveData(chunk1.constData(), chunk1.size());
    em.receiveData(chunk2.constData(), chunk2.size());

    QCOMPARE(lineSpy.count(), 1);
    QByteArray received = lineSpy.at(0).at(0).toByteArray();

    // Verify exact byte match — no U+FFFD (EF BF BD) substitution
    QVERIFY2(!received.contains("\xEF\xBF\xBD"),
             qPrintable(QStringLiteral("U+FFFD found in output! hex: ") + QString::fromLatin1(received.toHex(' '))));
    QCOMPARE(received, expected);
}

QTEST_GUILESS_MAIN(Vt102EmulationTest)

#include "moc_Vt102EmulationTest.cpp"
