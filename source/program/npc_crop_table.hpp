#pragma once

/* Per-NPC head-crop offsets for the world-map markers.
 *
 * The marker draws `Rectangle(0, offset, 16, 15)` out of the NPC's walk
 * sheet, matching NPCMapLocations (Bouhm, Nexus #239).
 *
 * The first block is copied verbatim from the PC mod
 * (`Framework/ModConstants.cs -> NpcMarkerOffsets`) so villagers look
 * exactly like they do on PC.  Do not retune these.
 *
 * Those offsets are MIT-licensed, Copyright (c) 2019 Brian Pak (Bouhm),
 * https://github.com/Bouhm/stardew-valley-mods.  The full notice and the
 * reasoning for keeping the copy are in THIRD_PARTY.md.
 *
 * The second block only covers NPCs that are missing from the mod's table
 * and therefore fell back to offset 0, which left blank rows at the top and
 * cut the chin off:
 *   Bouncer   content starts at row 4, and his hat crown is the top of the
 *             silhouette -- offset 2 keeps hat + full face.
 *   MrQi      content starts at row 1 and his head is 17 rows tall (hat 1-10,
 *             face 11-17), so a 15-row window at 0 clips the chin; 3 keeps
 *             the whole face.
 *   Henchman  content starts at row 3 and the glowing eyes sit on row 14,
 *             which row 0's window cuts off.
 */

namespace npccrop {

struct Entry {
    const char* name;
    int offset;
};

static constexpr Entry kTable[] = {
    /* --- NPCMapLocations NpcMarkerOffsets (verbatim) --- */
    {"Abigail",    3},
    {"Alex",       0},
    {"Birdie",     6},
    {"Caroline",   2},
    {"Clint",     -1},
    {"Demetrius", -2},
    {"Dwarf",      1},
    {"Elliott",   -1},
    {"Emily",      1},
    {"Evelyn",     4},
    {"Fizz",       4},
    {"George",     4},
    {"Gunther",    3},
    {"Gus",        2},
    {"Haley",      2},
    {"Harvey",    -1},
    {"Jas",        7},
    {"Jodi",       3},
    {"Kent",      -1},
    {"Krobus",     0},
    {"Leah",       2},
    {"Leo",        6},
    {"Lewis",      1},
    {"Linus",      6},
    {"Marlon",     2},
    {"Marnie",     4},
    {"Maru",       2},
    {"Pam",        5},
    {"Penny",      3},
    {"Pierre",     0},
    {"Robin",      2},
    {"Sam",        0},
    {"Sandy",      2},
    {"Sebastian",  1},
    {"Shane",      1},
    {"Vincent",    8},
    {"Willy",     -1},
    {"Wizard",     0},

    /* --- added: not in the mod's table, previously fell back to 0 --- */
    {"Bouncer",    2},
    {"Henchman",   3},
    {"MrQi",       3},
};

static constexpr int kTableLen =
    static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));

inline bool PrefixMatch(const char* name, const char* s, bool exactOnly) {
    while (*name != '\0' && *s == *name) {
        ++name;
        ++s;
    }
    if (*name != '\0') {
        return false;
    }
    return exactOnly ? (*s == '\0') : (*s == '\0' || *s == '_');
}

/* `spriteName` is the internal sprite name, e.g. "Abigail" or "Abigail_Winter".
 * Exact match wins; otherwise a `_`-suffixed seasonal/beach variant inherits
 * the base NPC's crop.  Leo's sprite is "ParrotBoy", so it needs an alias. */
inline int Lookup(const char* spriteName) {
    if (spriteName == nullptr || *spriteName == '\0') {
        return 0;
    }

    /* Leo is drawn from Characters/ParrotBoy. */
    if (PrefixMatch("ParrotBoy", spriteName, false)) {
        return 6;
    }

    for (int i = 0; i < kTableLen; i++) {
        if (PrefixMatch(kTable[i].name, spriteName, true)) {
            return kTable[i].offset;
        }
    }

    for (int i = 0; i < kTableLen; i++) {
        if (PrefixMatch(kTable[i].name, spriteName, false)) {
            return kTable[i].offset;
        }
    }

    return 0;
}

}  // namespace npccrop
