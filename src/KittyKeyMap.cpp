/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "KittyKeyMap.h"

#include <QHash>

using namespace Konsole;

/* clang-format off */

// Static mapping table from Qt::Key to KittyKeyInfo
// Format: { kittyKeyCode, type, legacyNumber, legacySuffix, isModifier }

static const QHash<int, KittyKeyInfo> &keyMap()
{
    static const QHash<int, KittyKeyInfo> map = {
        // === Legacy keys (keep traditional CSI encoding) ===

        // Arrow keys: CSI 1;mods X
        { Qt::Key_Up,       { 0, KittyKeyType::Legacy, 0, 'A', false } },
        { Qt::Key_Down,     { 0, KittyKeyType::Legacy, 0, 'B', false } },
        { Qt::Key_Right,    { 0, KittyKeyType::Legacy, 0, 'C', false } },
        { Qt::Key_Left,     { 0, KittyKeyType::Legacy, 0, 'D', false } },
        { Qt::Key_Clear,    { 0, KittyKeyType::Legacy, 0, 'E', false } },  // Begin/KP_Begin
        { Qt::Key_End,      { 0, KittyKeyType::Legacy, 0, 'F', false } },
        { Qt::Key_Home,     { 0, KittyKeyType::Legacy, 0, 'H', false } },

        // F1-F4: CSI 1;mods P/Q/R/S (or SS3 P/Q/R/S when no modifiers)
        { Qt::Key_F1,       { 0, KittyKeyType::Legacy, 0, 'P', false } },
        { Qt::Key_F2,       { 0, KittyKeyType::Legacy, 0, 'Q', false } },
        { Qt::Key_F3,       { 0, KittyKeyType::Legacy, 0, 'R', false } },
        { Qt::Key_F4,       { 0, KittyKeyType::Legacy, 0, 'S', false } },

        // F5-F12: CSI N;mods ~
        { Qt::Key_F5,       { 0, KittyKeyType::Legacy, 15, 0, false } },
        { Qt::Key_F6,       { 0, KittyKeyType::Legacy, 17, 0, false } },
        { Qt::Key_F7,       { 0, KittyKeyType::Legacy, 18, 0, false } },
        { Qt::Key_F8,       { 0, KittyKeyType::Legacy, 19, 0, false } },
        { Qt::Key_F9,       { 0, KittyKeyType::Legacy, 20, 0, false } },
        { Qt::Key_F10,      { 0, KittyKeyType::Legacy, 21, 0, false } },
        { Qt::Key_F11,      { 0, KittyKeyType::Legacy, 23, 0, false } },
        { Qt::Key_F12,      { 0, KittyKeyType::Legacy, 24, 0, false } },

        // F13-F35 (extended function keys): CSI N;mods ~
        { Qt::Key_F13,      { 0, KittyKeyType::Legacy, 57376, 0, false } },
        { Qt::Key_F14,      { 0, KittyKeyType::Legacy, 57377, 0, false } },
        { Qt::Key_F15,      { 0, KittyKeyType::Legacy, 57378, 0, false } },
        { Qt::Key_F16,      { 0, KittyKeyType::Legacy, 57379, 0, false } },
        { Qt::Key_F17,      { 0, KittyKeyType::Legacy, 57380, 0, false } },
        { Qt::Key_F18,      { 0, KittyKeyType::Legacy, 57381, 0, false } },
        { Qt::Key_F19,      { 0, KittyKeyType::Legacy, 57382, 0, false } },
        { Qt::Key_F20,      { 0, KittyKeyType::Legacy, 57383, 0, false } },
        { Qt::Key_F21,      { 0, KittyKeyType::Legacy, 57384, 0, false } },
        { Qt::Key_F22,      { 0, KittyKeyType::Legacy, 57385, 0, false } },
        { Qt::Key_F23,      { 0, KittyKeyType::Legacy, 57386, 0, false } },
        { Qt::Key_F24,      { 0, KittyKeyType::Legacy, 57387, 0, false } },
        { Qt::Key_F25,      { 0, KittyKeyType::Legacy, 57388, 0, false } },

        // Insert, Delete, PageUp, PageDown: CSI N;mods ~
        { Qt::Key_Insert,   { 0, KittyKeyType::Legacy, 2, 0, false } },
        { Qt::Key_Delete,   { 0, KittyKeyType::Legacy, 3, 0, false } },
        { Qt::Key_PageUp,   { 0, KittyKeyType::Legacy, 5, 0, false } },
        { Qt::Key_PageDown, { 0, KittyKeyType::Legacy, 6, 0, false } },

        // === CSI_u keys (functional keys using CSI keycode u) ===

        { Qt::Key_Escape,    { 27,  KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_Return,    { 13,  KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_Enter,     { 13,  KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_Tab,       { 9,   KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_Backtab,   { 9,   KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_Backspace, { 127, KittyKeyType::CSI_u, 0, 0, false } },

        // Modifier keys — PUA codepoints per kitty spec
        { Qt::Key_Shift,      { 57441, KittyKeyType::CSI_u, 0, 0, true } },   // Left Shift
        { Qt::Key_Control,    { 57442, KittyKeyType::CSI_u, 0, 0, true } },   // Left Control
        { Qt::Key_Alt,        { 57443, KittyKeyType::CSI_u, 0, 0, true } },   // Left Alt
        { Qt::Key_Super_L,    { 57444, KittyKeyType::CSI_u, 0, 0, true } },   // Left Super
        { Qt::Key_Super_R,    { 57450, KittyKeyType::CSI_u, 0, 0, true } },   // Right Super
        { Qt::Key_Hyper_L,    { 57445, KittyKeyType::CSI_u, 0, 0, true } },   // Left Hyper
        { Qt::Key_Hyper_R,    { 57451, KittyKeyType::CSI_u, 0, 0, true } },   // Right Hyper
        { Qt::Key_Meta,       { 57446, KittyKeyType::CSI_u, 0, 0, true } },   // Left Meta

        // Lock keys
        { Qt::Key_CapsLock,   { 57358, KittyKeyType::CSI_u, 0, 0, true } },
        { Qt::Key_ScrollLock, { 57359, KittyKeyType::CSI_u, 0, 0, true } },
        { Qt::Key_NumLock,    { 57360, KittyKeyType::CSI_u, 0, 0, true } },

        // Print/Pause/Menu
        { Qt::Key_Print,      { 57361, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_Pause,      { 57362, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_Menu,       { 57363, KittyKeyType::CSI_u, 0, 0, false } },

        // Media keys
        { Qt::Key_MediaPlay,         { 57428, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_MediaPause,        { 57429, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_MediaTogglePlayPause, { 57430, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_MediaStop,         { 57432, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_MediaNext,         { 57435, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_MediaPrevious,     { 57434, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_VolumeDown,        { 57436, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_VolumeUp,          { 57438, KittyKeyType::CSI_u, 0, 0, false } },
        { Qt::Key_VolumeMute,        { 57439, KittyKeyType::CSI_u, 0, 0, false } },

    };
    return map;
}

/* clang-format on */

KittyKeyInfo Konsole::qtKeyToKittyKey(int qtKey)
{
    const auto &map = keyMap();
    auto it = map.find(qtKey);
    if (it != map.end()) {
        return *it;
    }

    return {0, KittyKeyType::Unknown, 0, 0, false};
}

int Konsole::kittyModifierBits(Qt::KeyboardModifiers mods, bool capsLock, bool numLock)
{
    int bits = 0;

    if (mods & Qt::ShiftModifier) {
        bits |= 1;
    }
    if (mods & Qt::AltModifier) {
        bits |= 2;
    }
    if (mods & Qt::ControlModifier) {
        bits |= 4;
    }
    if (mods & Qt::MetaModifier) {
        bits |= 8; // Super on Linux (Qt maps Meta to the "Windows" key)
    }
    if (capsLock) {
        bits |= 64;
    }
    if (numLock) {
        bits |= 128;
    }

    return 1 + bits;
}
