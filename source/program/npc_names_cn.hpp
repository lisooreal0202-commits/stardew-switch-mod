/* English NPC name -> Simplified Chinese display name.
 *
 * birthday_table.hpp is generated from Data/Characters and holds the English
 * names only -- the Chinese ones live in the localisation files, which are not
 * in the NSO and not in the repo, so they cannot be generated here.  Keeping
 * the mapping in its own file means the generated table can be rebuilt at any
 * time without losing this.
 *
 * Lookup is by English name, once per day, so a linear scan over 34 entries is
 * not worth optimising.
 *
 * Names of four characters or more are cut to the first two: the HUD row has
 * to share a line with a second column, and 德米特里厄斯 alone is 100px.  The
 * two-character forms are the ones players use anyway.
 */
#pragma once

#include <cstring>

namespace npcname {

struct Entry {
    const char* en;
    const char16_t* cn;
};

static const Entry kTable[] = {
    {"Abigail",    u"阿比"},
    {"Alex",       u"亚历克斯"},
    {"Caroline",   u"卡洛琳"},
    {"Clint",      u"克林特"},
    {"Demetrius",  u"德米"},
    {"Dwarf",      u"矮人"},
    {"Elliott",    u"艾利"},
    {"Emily",      u"艾米丽"},
    {"Evelyn",     u"艾芙琳"},
    {"George",     u"乔治"},
    {"Gus",        u"格斯"},
    {"Haley",      u"海莉"},
    {"Harvey",     u"哈维"},
    {"Jas",        u"贾斯"},
    {"Jodi",       u"乔迪"},
    {"Kent",       u"肯特"},
    {"Krobus",     u"克罗"},
    {"Leah",       u"莉亚"},
    {"Leo",        u"雷欧"},
    {"Lewis",      u"刘易斯"},
    {"Linus",      u"莱纳斯"},
    {"Marnie",     u"玛妮"},
    {"Maru",       u"玛鲁"},
    {"Pam",        u"潘姆"},
    {"Penny",      u"潘妮"},
    {"Pierre",     u"皮埃尔"},
    {"Robin",      u"罗宾"},
    {"Sam",        u"山姆"},
    {"Sandy",      u"桑迪"},
    {"Sebastian",  u"塞巴"},
    {"Shane",      u"谢恩"},
    {"Vincent",    u"文森特"},
    {"Willy",      u"威利"},
    {"Wizard",     u"巫师"},
};

/* Chinese name for an English one, or nullptr if this build's data has an NPC
 * the table does not know.  Callers fall back to the English name rather than
 * showing nothing -- a wrong language beats a missing reminder. */
inline const char16_t* Chinese(const char* english) {
    if (english == nullptr) {
        return nullptr;
    }
    for (const Entry& e : kTable) {
        if (std::strcmp(e.en, english) == 0) {
            return e.cn;
        }
    }
    return nullptr;
}

}  // namespace npcname
