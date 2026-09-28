/*
    SPDX-FileCopyrightText: 2013, 2018 Kurt Hindenburg <kurt.hindenburg@gmail.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

// Own
#include "KeyboardTranslatorTest.h"

#include "../keyboardtranslator/KeyboardTranslatorReader.h"

// Qt
#include <QTest>

using namespace Konsole;

Q_DECLARE_METATYPE(Qt::KeyboardModifiers)

void KeyboardTranslatorTest::testEntryTextWildcards_data()
{
    // Shift   = 1 + (1 << 0) = 2
    // Alt     = 1 + (1 << 2) = 3
    // Control = 1 + (1 << 4) = 5

    QTest::addColumn<QByteArray>("text");
    QTest::addColumn<QByteArray>("result");
    QTest::addColumn<bool>("wildcards");
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");

    QTest::newRow("Home no wildcards no modifiers") << QByteArray("Home") << QByteArray("Home") << false << Qt::KeyboardModifiers(Qt::NoModifier);
    QTest::newRow("Home no wildcards Shift modifiers") << QByteArray("Home") << QByteArray("Home") << false << Qt::KeyboardModifiers(Qt::ShiftModifier);
    QTest::newRow("Home no wildcards Alt modifiers") << QByteArray("Home") << QByteArray("Home") << false << Qt::KeyboardModifiers(Qt::AltModifier);
    QTest::newRow("Home no wildcards Control modifiers") << QByteArray("Home") << QByteArray("Home") << false << Qt::KeyboardModifiers(Qt::ControlModifier);

    QTest::newRow("Home yes wildcards no modifiers") << QByteArray("Home") << QByteArray("Home") << true << Qt::KeyboardModifiers(Qt::NoModifier);
    QTest::newRow("Home yes wildcards Shift modifiers") << QByteArray("Home") << QByteArray("Home") << true << Qt::KeyboardModifiers(Qt::ShiftModifier);
    QTest::newRow("Home yes wildcards Alt modifiers") << QByteArray("Home") << QByteArray("Home") << true << Qt::KeyboardModifiers(Qt::AltModifier);
    QTest::newRow("Home yes wildcards Control modifiers") << QByteArray("Home") << QByteArray("Home") << true << Qt::KeyboardModifiers(Qt::ControlModifier);

    // text, results: no mod, shift, alt, control
    QList<QByteArray> entry;
    entry << QByteArray("E*") << QByteArray("E1") << QByteArray("E2") << QByteArray("E3") << QByteArray("E5");
    QTest::newRow("E* yes wildcards no modifiers") << entry[0] << entry[1] << true << Qt::KeyboardModifiers(Qt::NoModifier);
    QTest::newRow("E* yes wildcards Shift modifiers") << entry[0] << entry[2] << true << Qt::KeyboardModifiers(Qt::ShiftModifier);
    QTest::newRow("E* yes wildcards Alt modifiers") << entry[0] << entry[3] << true << Qt::KeyboardModifiers(Qt::AltModifier);
    QTest::newRow("E* yes wildcards Control modifiers") << entry[0] << entry[4] << true << Qt::KeyboardModifiers(Qt::ControlModifier);

    // combinations
    entry.clear();
    entry << QByteArray("E*") << QByteArray("E4") << QByteArray("E6") << QByteArray("E8") << QByteArray("E7");
    QTest::newRow("E* yes wildcards Shift+Alt modifiers") << entry[0] << entry[1] << true << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::AltModifier);
    QTest::newRow("E* yes wildcards Shift+Control modifiers") << entry[0] << entry[2] << true << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::ControlModifier);
    QTest::newRow("E* yes wildcards Shift+Alt+Control modifiers")
        << entry[0] << entry[3] << true << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier);
    QTest::newRow("E* yes wildcards Alt+Control modifiers") << entry[0] << entry[4] << true << Qt::KeyboardModifiers(Qt::AltModifier | Qt::ControlModifier);

    // text, results: no mod, shift, alt, control
    entry.clear();
    entry << QByteArray("\033[24;*~") << QByteArray("\033[24;1~") << QByteArray("\033[24;2~") << QByteArray("\033[24;3~") << QByteArray("\033[24;5~");
    QTest::newRow("\033[24;*~ yes wildcards no modifiers") << entry[0] << entry[1] << true << Qt::KeyboardModifiers(Qt::NoModifier);
    QTest::newRow("\033[24;*~ yes wildcards Shift modifiers") << entry[0] << entry[2] << true << Qt::KeyboardModifiers(Qt::ShiftModifier);
    QTest::newRow("\033[24;*~ yes wildcards Alt modifiers") << entry[0] << entry[3] << true << Qt::KeyboardModifiers(Qt::AltModifier);
    QTest::newRow("\033[24;*~ yes wildcards Control modifiers") << entry[0] << entry[4] << true << Qt::KeyboardModifiers(Qt::ControlModifier);

    // combinations
    entry.clear();
    entry << QByteArray("\033[24;*~") << QByteArray("\033[24;4~") << QByteArray("\033[24;6~") << QByteArray("\033[24;8~") << QByteArray("\033[24;7~");
    QTest::newRow("\033[24;*~ yes wildcards Shift+Alt modifiers") << entry[0] << entry[1] << true << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::AltModifier);
    QTest::newRow("\033[24;*~ yes wildcards Shift+Control modifiers")
        << entry[0] << entry[2] << true << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::ControlModifier);
    QTest::newRow("\033[24;*~ yes wildcards Shift+Alt+Control modifiers")
        << entry[0] << entry[3] << true << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier);
    QTest::newRow("\033[24;*~ yes wildcards Alt+Control modifiers")
        << entry[0] << entry[4] << true << Qt::KeyboardModifiers(Qt::AltModifier | Qt::ControlModifier);
}

void KeyboardTranslatorTest::testEntryTextWildcards()
{
    QFETCH(QByteArray, text);
    QFETCH(QByteArray, result);
    QFETCH(bool, wildcards);
    QFETCH(Qt::KeyboardModifiers, modifiers);

    KeyboardTranslator::Entry entry;
    entry.setText(text);

    QCOMPARE(entry.text(wildcards, modifiers), result);
}

void KeyboardTranslatorTest::testHexKeys()
{
    QFile linuxkeytab(QFINDTESTDATA(QStringLiteral("data/test.keytab")));
    QVERIFY(linuxkeytab.exists());
    QVERIFY(linuxkeytab.open(QIODevice::ReadOnly));

    auto translator = std::unique_ptr<KeyboardTranslator>(new KeyboardTranslator(QStringLiteral("testtranslator")));

    KeyboardTranslatorReader reader(&linuxkeytab);
    while (reader.hasNextEntry()) {
        translator->addEntry(reader.nextEntry());
    }
    linuxkeytab.close();

    // A worthless check ATM
    if (reader.parseError()) {
        QFAIL("Parse failure");
    }

    QCOMPARE(QStringLiteral("testtranslator"), translator->name());
    QCOMPARE(QString(), translator->description());

    auto entry = translator->findEntry(Qt::Key_Backspace, Qt::NoModifier);
    QVERIFY(!entry.isNull());
    QCOMPARE(int(Qt::Key_Backspace), entry.keyCode());
    QCOMPARE(QByteArray("\x7F"), entry.text());
    QCOMPARE(QByteArray("\\x7f"), entry.escapedText());
    QCOMPARE(Qt::KeyboardModifiers(Qt::NoModifier), entry.modifiers());
    QCOMPARE(Qt::KeyboardModifiers(Qt::NoModifier), entry.modifierMask());
    QCOMPARE(KeyboardTranslator::States(KeyboardTranslator::NoState), entry.state());
    QCOMPARE(QStringLiteral("Backspace"), entry.conditionToString());
    QCOMPARE(QStringLiteral("\\x7f"), entry.resultToString());
    QVERIFY(entry.matches(Qt::Key_Backspace, Qt::NoModifier, KeyboardTranslator::NoState));
    QVERIFY(entry == translator->findEntry(Qt::Key_Backspace, Qt::NoModifier));

    entry = translator->findEntry(Qt::Key_Delete, Qt::NoModifier);
    QVERIFY(!entry.isNull());
    QCOMPARE(int(Qt::Key_Delete), entry.keyCode());
    QCOMPARE(QByteArray("\x08"), entry.text());
    QCOMPARE(QByteArray("\\b"), entry.escapedText());
    QCOMPARE(Qt::KeyboardModifiers(Qt::NoModifier), entry.modifiers());
    QCOMPARE(Qt::KeyboardModifiers(Qt::NoModifier), entry.modifierMask());
    QCOMPARE(KeyboardTranslator::States(KeyboardTranslator::NoState), entry.state());
    QCOMPARE(QStringLiteral("Del"), entry.conditionToString());
    QCOMPARE(QStringLiteral("\\b"), entry.resultToString());
    QVERIFY(entry.matches(Qt::Key_Delete, Qt::NoModifier, KeyboardTranslator::NoState));
    QVERIFY(!entry.matches(Qt::Key_Backspace, Qt::NoModifier, KeyboardTranslator::NoState));
    QVERIFY(!(entry == translator->findEntry(Qt::Key_Backspace, Qt::NoModifier)));

    entry = translator->findEntry(Qt::Key_Space, Qt::NoModifier);
    QVERIFY(!entry.isNull());
    QCOMPARE(int(Qt::Key_Space), entry.keyCode());
    QEXPECT_FAIL("", "Several keytabs use x00 as Space +Control;  text() fails", Continue);
    QCOMPARE(QByteArray("\x00"), entry.text());
    QCOMPARE(QByteArray("\\x00"), entry.escapedText());
    QCOMPARE(Qt::KeyboardModifiers(Qt::NoModifier), entry.modifiers());
    QCOMPARE(Qt::KeyboardModifiers(Qt::NoModifier), entry.modifierMask());
    QCOMPARE(KeyboardTranslator::States(KeyboardTranslator::NoState), entry.state());
    QCOMPARE(QStringLiteral("Space"), entry.conditionToString());
    QCOMPARE(QStringLiteral("\\x00"), entry.resultToString());
    QVERIFY(entry.matches(Qt::Key_Space, Qt::NoModifier, KeyboardTranslator::NoState));
    QVERIFY(entry == translator->findEntry(Qt::Key_Space, Qt::NoModifier));
    QVERIFY(!entry.matches(Qt::Key_Backspace, Qt::NoModifier, KeyboardTranslator::NoState));
    QVERIFY(!(entry == translator->findEntry(Qt::Key_Backspace, Qt::NoModifier)));
}

void KeyboardTranslatorTest::testDefaultArrowKeys_data()
{
    QTest::addColumn<QString>("layout");
    QTest::addColumn<int>("key");
    QTest::addColumn<Qt::KeyboardModifiers>("modifiers");
    QTest::addColumn<int>("states");
    QTest::addColumn<QByteArray>("expectedText");
    QTest::addColumn<int>("expectedCommand");

    for (const auto &layout : {QStringLiteral("default"), QStringLiteral("macos")}) {
        for (bool alternateScreen : {false, true}) {
            for (bool applicationCursor : {false, true}) {
                int states = KeyboardTranslator::AnsiState;
                if (alternateScreen) {
                    states |= KeyboardTranslator::AlternateScreenState;
                }
                if (applicationCursor) {
                    states |= KeyboardTranslator::CursorKeysState;
                }
                const QByteArray prefix =
                    layout.toLatin1() + (alternateScreen ? "-alternate" : "-primary") + (applicationCursor ? "-application-cursor" : "-normal-cursor");

                for (int key : {Qt::Key_Left, Qt::Key_Right}) {
                    const char suffix = key == Qt::Key_Left ? 'D' : 'C';
                    const QByteArray name = prefix + (key == Qt::Key_Left ? "-left" : "-right");
                    QTest::newRow(QByteArray(name + "-plain").constData())
                        << layout << key << Qt::KeyboardModifiers(Qt::NoModifier) << states
                        << QByteArray(QByteArray(applicationCursor ? "\033O" : "\033[") + suffix) << int(KeyboardTranslator::NoCommand);
                    QTest::newRow(QByteArray(name + "-shift").constData()) << layout << key << Qt::KeyboardModifiers(Qt::ShiftModifier) << states
                                                                           << QByteArray(QByteArray("\033[1;2") + suffix) << int(KeyboardTranslator::NoCommand);
                    QTest::newRow(QByteArray(name + "-shift-ctrl").constData())
                        << layout << key << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::ControlModifier) << states
                        << QByteArray(QByteArray("\033[1;6") + suffix) << int(KeyboardTranslator::NoCommand);
                    QTest::newRow(QByteArray(name + "-shift-alt").constData())
                        << layout << key << Qt::KeyboardModifiers(Qt::ShiftModifier | Qt::AltModifier) << states << QByteArray(QByteArray("\033[1;4") + suffix)
                        << int(KeyboardTranslator::NoCommand);
                }

                // Vertical Shift+arrows still scroll the primary screen locally.
                for (int key : {Qt::Key_Up, Qt::Key_Down}) {
                    const bool up = key == Qt::Key_Up;
                    const QByteArray expected = alternateScreen ? QByteArray(up ? "\033[1;2A" : "\033[1;2B") : QByteArray();
                    const int command = alternateScreen ? KeyboardTranslator::NoCommand
                                                        : (up ? KeyboardTranslator::ScrollLineUpCommand : KeyboardTranslator::ScrollLineDownCommand);
                    QTest::newRow(QByteArray(prefix + (up ? "-shift-up" : "-shift-down")).constData())
                        << layout << key << Qt::KeyboardModifiers(Qt::ShiftModifier) << states << expected << command;
                }
            }
        }
    }
}

void KeyboardTranslatorTest::testDefaultArrowKeys()
{
    QFETCH(QString, layout);
    QFETCH(int, key);
    QFETCH(Qt::KeyboardModifiers, modifiers);
    QFETCH(int, states);
    QFETCH(QByteArray, expectedText);
    QFETCH(int, expectedCommand);

    QFile keytab(QFINDTESTDATA(QStringLiteral("../../data/keyboard-layouts/") + layout + QStringLiteral(".keytab")));
    QVERIFY(keytab.open(QIODevice::ReadOnly));
    KeyboardTranslator translator(layout);
    KeyboardTranslatorReader reader(&keytab);
    while (reader.hasNextEntry()) {
        translator.addEntry(reader.nextEntry());
    }
    QVERIFY(!reader.parseError());

    const auto entry = translator.findEntry(key, modifiers, KeyboardTranslator::States(states));
    QVERIFY(!entry.isNull());
    QCOMPARE(entry.text(true, modifiers), expectedText);
    QCOMPARE(int(entry.command()), expectedCommand);
}

QTEST_GUILESS_MAIN(KeyboardTranslatorTest)

#include "moc_KeyboardTranslatorTest.cpp"
