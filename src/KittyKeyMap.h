/*
    SPDX-FileCopyrightText: 2026 Konsole Developers

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef KITTYKEYMAP_H
#define KITTYKEYMAP_H

#include <Qt>

namespace Konsole
{

// How the key should be encoded in kitty keyboard protocol
enum class KittyKeyType {
    // Keys that keep their traditional CSI encoding (arrows, function keys, etc.)
    // Arrows: CSI 1;mods X  (X = A/B/C/D/E/F/H)
    // F1-F4: CSI 1;mods P/Q/R/S (SS3 when no modifiers)
    // F5-F12, Insert, Delete, PageUp/Down, Home, End: CSI N;mods ~
    Legacy,
    // Functional keys that use CSI keycode u format
    CSI_u,
    // Regular printable characters — use Unicode codepoint as key code
    Plain,
    // Not a recognized key — fall through to legacy path
    Unknown,
};

struct KittyKeyInfo {
    int keyCode; // Kitty key code (Unicode codepoint or functional key code)
    KittyKeyType type;

    // For Legacy keys: the legacy number and suffix
    int legacyNumber; // The number N in CSI N;mods ~  (0 for letter-type like arrows)
    char legacySuffix; // The letter in CSI 1;mods X  (0 for tilde-type)

    // Whether this is a modifier key
    bool isModifier;
};

// Map a Qt key to Kitty key info.
// Returns type=Unknown if the key is not recognized.
KittyKeyInfo qtKeyToKittyKey(int qtKey);

// Encode Qt keyboard modifiers as kitty modifier bits.
// Returns 1 + (shift:1 | alt:2 | ctrl:4 | super:8 | hyper:16 | meta:32 | caps_lock:64 | num_lock:128)
// When no modifiers are active, returns 1.
int kittyModifierBits(Qt::KeyboardModifiers mods, bool capsLock, bool numLock);

} // namespace Konsole

#endif // KITTYKEYMAP_H
