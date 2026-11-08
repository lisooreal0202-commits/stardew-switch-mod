#pragma once

/* Leo's 16x15 head, baked into the binary as colour runs.
 *
 * He is the one villager who cannot be drawn the normal way: his sheet is
 * Characters/ParrotBoy.xnb and he only exists in Game1.locations while Ginger
 * Island is loaded, so on most days there is no NPC object to read a sheet
 * from.  Loading the sheet ourselves would mean calling
 * ContentManager.Load<Texture2D>, a generic method that needs a MethodInfo*
 * we do not have.
 *
 * So the pixels are baked in, the same way the weather icons are: cropped at
 * (0, 6, 16, 15) -- the window npccrop gives for "ParrotBoy" -- and stored as
 * horizontal runs merged vertically.  No texture is loaded at runtime.
 *
 * Generated from Content/Characters/ParrotBoy.xnb (PC 1.6.15).  Do not edit.
 */

/* Same layout as IconRun (weather_icons.hpp, included first), so DrawIconRuns
 * can blit this directly. */
static constexpr IconRun kLeoHeadRuns[] = {
    {0,5,1,1,0xFF2D1625u},{0,6,3,1,0xFF2D1625u},{0,10,2,1,0xFF2D1625u},{0,11,1,1,0xFF2D1625u},
    {1,2,4,1,0xFF2D1625u},{1,4,3,1,0xFF2D1625u},{1,5,1,1,0xFF773B2Du},{1,7,1,3,0xFF2D1625u},
    {1,11,1,1,0xFF773B2Du},{1,12,3,1,0xFF2D1625u},{2,3,1,1,0xFF2D1625u},{2,5,1,1,0xFF422123u},
    {2,7,1,1,0xFF773B2Du},{2,8,2,1,0xFFB76F4Bu},{2,9,1,2,0xFFB76F4Bu},{2,11,1,1,0xFF422123u},
    {3,3,1,1,0xFF773B2Du},{3,5,2,1,0xFF2D1625u},{3,6,2,1,0xFF773B2Du},{3,7,1,1,0xFFB76F4Bu},
    {3,9,1,1,0xFF773B2Du},{3,10,1,1,0xFF422123u},{3,11,1,1,0xFFE5887Eu},{4,3,1,1,0xFFB76F4Bu},
    {4,4,2,1,0xFF773B2Du},{4,7,1,1,0xFF422123u},{4,8,1,1,0xFF2D1625u},{4,9,1,1,0xFF422123u},
    {4,10,1,2,0xFF724046u},{4,12,1,1,0xFFE5887Eu},{4,13,1,1,0xFF422123u},{4,14,1,1,0xFF510120u},
    {5,3,2,1,0xFF2D1625u},{5,5,4,1,0xFF422123u},{5,6,1,1,0xFF422123u},{5,7,1,1,0xFF2D1625u},
    {5,8,1,1,0xFF422123u},{5,9,1,1,0xFF724046u},{5,10,2,1,0xFF2D1625u},{5,11,1,1,0xFF9FA4A8u},
    {5,12,1,1,0xFFEDF4FAu},{5,13,1,1,0xFF724046u},{5,14,6,1,0xFF422123u},{6,4,1,1,0xFF2D1625u},
    {6,6,3,1,0xFF773B2Du},{6,7,1,1,0xFF773B2Du},{6,8,1,1,0xFF2D1625u},{6,9,1,1,0xFF422123u},
    {6,11,1,1,0xFF2D1625u},{6,12,1,1,0xFF53326Du},{6,13,1,1,0xFFE5887Eu},{7,2,1,1,0xFF2D1625u},
    {7,3,1,1,0xFF773B2Du},{7,4,1,1,0xFF422123u},{7,7,2,1,0xFFB76F4Bu},{7,8,1,1,0xFF773B2Du},
    {7,9,1,1,0xFF2D1625u},{7,10,1,1,0xFF724046u},{7,11,1,1,0xFFE5887Eu},{7,12,1,1,0xFFFFC9AEu},
    {7,13,2,1,0xFFFFC9AEu},{8,1,5,1,0xFF2D1625u},{8,2,1,1,0xFF773B2Du},{8,3,1,1,0xFFB76F4Bu},
    {8,4,1,1,0xFF773B2Du},{8,8,2,1,0xFFB76F4Bu},{8,9,1,1,0xFF773B2Du},{8,10,1,1,0xFF2D1625u},
    {8,11,1,1,0xFF724046u},{8,12,1,1,0xFFE5887Eu},{9,2,2,1,0xFFB76F4Bu},{9,3,1,1,0xFF773B2Du},
    {9,4,4,1,0xFF2D1625u},{9,5,1,1,0xFF773B2Du},{9,6,1,1,0xFF2D1625u},{9,7,1,1,0xFF773B2Du},
    {9,9,1,1,0xFFB76F4Bu},{9,10,2,1,0xFF773B2Du},{9,11,1,1,0xFF2D1625u},{9,12,3,1,0xFF2D1625u},
    {9,13,1,1,0xFFE5887Eu},{10,3,1,1,0xFF2D1625u},{10,5,2,1,0xFFB76F4Bu},{10,6,1,1,0xFF773B2Du},
    {10,7,6,1,0xFF2D1625u},{10,8,1,1,0xFF2D1625u},{10,9,1,1,0xFF773B2Du},{10,11,1,1,0xFF773B2Du},
    {10,13,1,1,0xFF724046u},{11,2,1,1,0xFF2D1625u},{11,6,2,1,0xFFB76F4Bu},{11,8,2,1,0xFF422123u},
    {11,9,1,1,0xFF2D1625u},{11,10,2,1,0xFF2D1625u},{11,11,1,1,0xFF2D1625u},{11,13,1,1,0xFF422123u},
    {11,14,1,1,0xFF510120u},{12,5,1,1,0xFF773B2Du},{12,9,1,1,0xFF422123u},{12,11,1,2,0xFF422123u},
    {13,5,1,1,0xFF2D1625u},{13,6,1,1,0xFF773B2Du},{13,8,1,1,0xFF2D1625u},{13,9,1,1,0xFF773B2Du},
    {13,10,2,1,0xFF773B2Du},{13,11,2,1,0xFF2D1625u},{14,6,1,1,0xFF2D1625u},{14,9,2,1,0xFF2D1625u},
    {15,10,1,1,0xFF2D1625u}
};

static constexpr int kLeoHeadRunCount =
    static_cast<int>(sizeof(kLeoHeadRuns) / sizeof(kLeoHeadRuns[0]));
