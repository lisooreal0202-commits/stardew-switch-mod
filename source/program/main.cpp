// Stage 2 of the native (subsdk8) route.
//
// Stage 1 proved the module loads and that a hook survives; this one makes the
// module *do* something visible, using a patch that is already hardware-verified
// through the overlay plugin:
//
//     GameLocation.damageMonster
//       0x14EA504  mov w22, w4   ->  mov w22, #9999
//       0x14EA508  mov w21, w3   ->  mov w21, #9999
//
// One hit kills anything.  If the player one-shots a slime, the whole chain
// works end to end: module loaded, init passed, patcher mapped the writable
// alias, and the write reached the game's code.
//
// Both writes are guarded: the original words are read back first and the
// patch is skipped (logged) if they are not what this build expects, so a
// different game version fails quietly instead of corrupting random code.
//
// On top of that, the world map page is hooked to draw a marker for every
// character in the player's current location.  The coordinate comes from the
// game itself -- WorldMapManager.GetPositionData + MapAreaPosition.GetPixel --
// so no part of the map transform is reimplemented here.
//
// Environment notes, learned the hard way:
//   * module.specs must not export __rtld_runtime_resolve (--export-dynamic)
//   * .text+8 must be the offset of a version triple, not the NOPs upstream
//     leaves there -- 1.6.15.13's rtld reads it
//   * the process needs an npdm that grants svcMapProcessMemory, which the
//     game's own npdm does not

#include "lib.hpp"
#include "lib/nx/kernel/svc.h"
#include "lib/nx/result.h"
#include "lib/util/sys/rw_pages.hpp"
#include "program/birthday_table.hpp"
#include "program/npc_crop_table.hpp"
#include "program/worldmap_table.hpp"
#include "program/tooltip_rect_table.hpp"
#include "program/weather_icons.hpp"

#include "program/sdlog.hpp"
#include "program/offset_tables.hpp"


/* Mirror a line to both the debug output and sdmc:/sdv_map.log. */
#define LOGF(...) do { Logging.Log(EXL_LOG_PREFIX __VA_ARGS__); sdlog::Line(__VA_ARGS__); } while (0)

/* v281 DIAGNOSTIC: install no hooks at all.
 *
 * Every per-frame hook has been ruled out one group at a time, and the freeze
 * still happens with the naming menu on screen.  That points away from the
 * hooks entirely: this mod also patches game CODE at startup for the 48-slot
 * backpack (six instruction sites around 0x17A7D00-0x17AD900, and NamingMenu
 * lives at 0x17F3F70 in the same Menus region).  Setting this to true makes
 * HOOKF return null for every install, so the module loads, passes the
 * fingerprint check and then does nothing at all -- the only way to tell
 * "a hook did it" from "a code patch did it". */
static const bool kDiagNoHooks = false;
#define HOOKF(...) (kDiagNoHooks ? nullptr : exl::hook::Hook(__VA_ARGS__))

namespace {

    /* Module base, filled in exl_main. */
    uintptr_t g_base = 0;

    /* SpriteBatch.Draw(Texture2D, Rectangle, Color) -- 0xC1A80.
     * Rectangle arrives by value in two 64-bit registers (x|y<<32, w|h<<32). */
    constexpr uintptr_t kDrawRectOff    = SDV_OFF(kDrawRectOff, 0xC1A80);
    /* Game1.staminaRect: the 1x1 white texture the game itself draws with. */
    constexpr uintptr_t kStaminaRectOff = SDV_OFF(kStaminaRectOff, 0xC95BD88);

    /* Game1's static calendar and cursor atlas.
     *
     * Same static-field block as staminaRect above, from the same metadata
     * dump, and that dump is trusted here because it independently agrees with
     * kStaminaRectOff -- a value this code already shipped and verified on
     * hardware.  Every read below is still guarded: a season outside 0..3 or a
     * day outside 1..28 means the offsets moved, and the badge is simply not
     * drawn instead of drawing nonsense. */
    constexpr uintptr_t kMouseCursorsOff = SDV_OFF(kMouseCursorsOff, 0xC95BC60);  /* Texture2D */
    constexpr uintptr_t kSeasonOff       = SDV_OFF(kSeasonOff, 0xC95BE28);  /* Season, 0..3 */
    constexpr uintptr_t kDayOfMonthOff   = SDV_OFF(kDayOfMonthOff, 0xC95BEB0);  /* int32, 1..28 */

    using DrawRectFn = void (*)(void* batch, void* texture, u64 xy, u64 wh, u32 color);
    /* Draw(Texture2D, Rectangle dest, Rectangle source, Color) -- 0xC17E0. */
    using GetPosPtrFn = void* (*)(void* loc, u64 tile, void* opt);
    using GetPositionsFn = void* (*)(void* area);
    /* AnimatedSprite.get_Texture -- lazily loads the sheet via the content
     * manager stored at +0x80, then returns it. */
    /* ContentManager.Load<Texture2D>(string) as reached from
     * AnimatedSprite.LoadTexture, plus Game1.content's static slot. */
    using LoadTexFn = void* (*)(void* contentManager, void* name);
    /* AnimatedSprite.LoadTexture(this, string name, bool loadOverride) */
    using LoadSpriteFn = void (*)(void* sprite, void* name, int flag);
    constexpr uintptr_t kLoadTextureOff = SDV_OFF(kLoadTextureOff, 0x1193200);
    constexpr uintptr_t kGame1ContentOff = SDV_OFF(kGame1ContentOff, 0xC95BA80);
    constexpr uintptr_t kObjectSheetOff = SDV_OFF(kObjectSheetOff, 0xC95BBE0);

    /* Draw(Texture2D, Rectangle dest, Rectangle source, Color) -- 0xC17E0. */
    /* Draw(Texture2D, Rectangle dest, NullableRectangle* source, Color).
     * 0xC17E0 reads a bool at source+0x10, so the 4th argument is a pointer,
     * not a packed rectangle. */
    struct NullableRect {
        int x, y, w, h;
        unsigned char present;
        unsigned char pad[7];
    };
    /* Full signature, as used by vanilla at 0x17E9B6C:
     *   x0 batch, x1 texture, x2/x3 dest, x4 source*, w5 color,
     *   s0 rotation, s1/s2 origin, s3 scale, x6 effects
     * Leaving the float arguments unset meant the scale came from whatever
     * happened to be in s3, which is why the markers came out tiny. */
    using DrawSpriteFn = void (*)(void* batch, void* texture,
                                  u64 dxy, u64 dwh, void* source, u32 color,
                                  float rotation, float originX, float originY,
                                  float scale, int effects);

    using GetSpriteTexFn = void* (*)(void* sprite);
    /* The community Switch port reads npc.Sprite through virtual slot 0xE0,
     * not through a field -- NPC may override the property. */
    constexpr uintptr_t kNpcSpriteVirtualSlot = SDV_OFF(kNpcSpriteVirtualSlot, 0xE0);
    constexpr uintptr_t kCharNameOff = SDV_OFF(kCharNameOff, 0xA8);
    /* Character._displayName -- a *bare* managed string (0x9c8590), NOT a
     * NetString.  The pointer at +0xC0 is the string object itself, so unlike
     * +0xA8 there is no +0x68 unwrap.  This is the localised name the game
     * shows in dialogue and the friendship menu. */
    constexpr uintptr_t kCharDisplayNameOff = SDV_OFF(kCharDisplayNameOff, 0xC0);
    constexpr uintptr_t kLoadSpriteTextureOff = SDV_OFF(kLoadSpriteTextureOff, 0x11930F0);
    constexpr uintptr_t kGetSpriteTexOff = SDV_OFF(kGetSpriteTexOff, 0x1192BD0);
    constexpr uintptr_t kGetWorldPositionsOff = SDV_OFF(kGetWorldPositionsOff, 0x1BB91F0);
    constexpr uintptr_t kGetPosNoFallbackOff = SDV_OFF(kGetPosNoFallbackOff, 0x1BBDCB0);


    /* Game1.get_player, from the AOT metadata dump (see docs/offsets). */
    constexpr uintptr_t kGetPlayerOff = SDV_OFF(kGetPlayerOff, 0x1440570);
    /* Game1's static Farmer slot, read by that getter. */
    constexpr uintptr_t kPlayerSlotOff = SDV_OFF(kPlayerSlotOff, 0xC95BB50);

    /* GameLocation.damageMonster -- same words the overlay plugin uses. */
    constexpr uintptr_t kDmgMaxOff = SDV_OFF(kDmgMaxOff, 0x14EA504);
    constexpr uintptr_t kDmgMinOff = SDV_OFF(kDmgMinOff, 0x14EA508);
    constexpr u32 kDmgMaxOrig = SDV_OFF(kDmgMaxOrig, 0x2A0403F6u);   /* mov w22, w4 */
    constexpr u32 kDmgMinOrig = SDV_OFF(kDmgMinOrig, 0x2A0303F5u);   /* mov w21, w3 */
    constexpr u32 kDmgMaxHit  = SDV_OFF(kDmgMaxHit, 0x5284E1F6u);   /* mov w22, #9999 */
    constexpr u32 kDmgMinHit  = SDV_OFF(kDmgMinHit, 0x5284E1F5u);   /* mov w21, #9999 */

    /* MapPage.drawMiniPortraits(this, SpriteBatch b, float alpha) = 0x17E8ED0.
     * Vanilla walks the other farmers here and draws one mini portrait each,
     * at   mapBounds.xy + GetPixel(context) + (32,32). */
    constexpr uintptr_t kDrawMiniPortraitsOff = SDV_OFF(kDrawMiniPortraitsOff, 0x17E8ED0);

    /* 0x17E91F0: draws one mini portrait.  Signature from the call site at
     * 0x17E90FC -- x0 = character, w1 = index, s0/s1 = screen x,y.  Big enough
     * to hook safely (0x1BBC10, which is four instructions, is not). */
    constexpr uintptr_t kPixelMathOff = SDV_OFF(kPixelMathOff, 0x1BBB510);
    constexpr uintptr_t kDrawSpriteOff = SDV_OFF(kDrawSpriteOff, 0xC17E0);
    constexpr uintptr_t kDrawPortraitOff = SDV_OFF(kDrawPortraitOff, 0x17E91F0);
    using DrawPortraitFn = void (*)(void* who, int index, float x, float y);
    DrawPortraitFn s_origDrawPortrait = nullptr;

    /* MapPage fields, from docs/offsets/meta_mappage.json. */
    constexpr uintptr_t kMapPosOff   = SDV_OFF(kMapPosOff, 0x68);   /* Vector2 mapPosition */
    constexpr uintptr_t kMapBoundOff = SDV_OFF(kMapBoundOff, 0xA4);   /* Rectangle mapBounds  */
    /* MapPage.hoverText -- the display name of the location the cursor is
     * over, or an empty string.  The game itself computes this, so reading it
     * means we never have to know where the cursor is.  Bare managed string
     * (0x9c8590), same type as GameLocation._displayName. */
    constexpr uintptr_t kMapHoverTextOff = SDV_OFF(kMapHoverTextOff, 0xC0);

    /* The world-map chain that produces hoverText.  MapPage.ctor walks it at
     * 0x17E6D00 onward; every step is a plain field read:
     *
     *   [this + 0x90]      MapArea[]            (data at +0x10, length word
     *                                            pointer at +0x20)
     *   [area + 0x20]      CachedWorldPositions  (same container shape)
     *   [pos  + 0x38]      tooltip text  -- copied into the ClickableComponent
     *                                       at +0x28, which the hover loop then
     *                                       copies into hoverText
     *   [pos  + 0x40]      the id string -- the key of MapPage.points
     *   [area + 0x28]      MapArea.CachedScrollText, the area's own text
     *
     * Only the two containers share a shape; the two element types do not. */
    constexpr uintptr_t kMapAreasOff = SDV_OFF(kMapAreasOff, 0x90);
    /* MapArea has TWO cached lists and they are not interchangeable:
     *   +0x18 CachedTooltips        the hover hotspots -- this is the one the
     *                               ctor walks at 0x17E6D98
     *   +0x20 CachedWorldPositions  pixel maths only, no text in it
     * MapArea.GetTooltips (0x1BB8B50) starts with `ldr x0,[x19,#0x18]`, which
     * is what proves the mapping.  v130 walked +0x20 and matched nothing. */
    constexpr uintptr_t kMapAreaTooltipsOff = SDV_OFF(kMapAreaTooltipsOff, 0x18);
    constexpr uintptr_t kMapAreaScrollTextOff = SDV_OFF(kMapAreaScrollTextOff, 0x28);
    /* MapAreaTooltip: +0x38 = the tooltip text (becomes hoverText),
     * +0x40 = the namespaced id (the key of MapPage.points).  The metadata
     * dump only lists CachedPixelArea for this type, so these two come from
     * the ctor: 0x1bbbdd0 is `ldr x0,[x0,#0x38]` and 0x1bbbde0 is
     * `ldr x0,[x0,#0x40]`. */
    constexpr uintptr_t kMapTooltipTextOff = SDV_OFF(kMapTooltipTextOff, 0x38);
    constexpr uintptr_t kMapTooltipIdOff = SDV_OFF(kMapTooltipIdOff, 0x40);

    /* GameLocation.characters, read the way drawCharacters reads it:
     *     ldr x8,[loc,#0xB0]   -> NetCollection
     *     ldr x9,[x8,#0x58]    -> inner List<NPC>
     *     ldr w9,[x9,#0x18]    -> _size
     * and elements MUST come from the List indexer -- indexing the backing
     * array directly yields garbage pointers (that crashed the game twice). */
    constexpr uintptr_t kLocCharactersOff = SDV_OFF(kLocCharactersOff, 0xB0);
    constexpr uintptr_t kListGetItemOff   = SDV_OFF(kListGetItemOff, 0x1203A70);
    /* GameLocation._displayName -- localised name, bare managed string
     * (0x9c8590).  Same type as MapPage.hoverText, so the two can be compared
     * character by character with no conversion in between.  (+0x1A8 is the
     * NetString internal key; do not confuse the two.) */
    constexpr uintptr_t kLocDisplayNameOff = SDV_OFF(kLocDisplayNameOff, 0x1A0);
    /* GameLocation.name -- NetString internal key ("Town", "Farm").  This is
     * the one that is always there: the constructor initialises it at
     * 0x14C39CC (`bl 0x1186180` = new NetString), and GameLocation.get_Name
     * (0x14C6C10) is exactly `ldr x8,[x0,#0x1A8]; ldr x0,[x8,#0x68]; ret`.
     * Unlike _displayName at +0x1A0 it is never null, so it is the field the
     * hover probe matches against. */
    constexpr uintptr_t kLocNameOff = SDV_OFF(kLocNameOff, 0x1A8);

    /* GameLocation.get_DisplayName -- DO NOT CALL IT.
     *
     * The property itself is at 0x14C6B60 (the metadata's `body` addresses for
     * this class are all shifted by +0x100; `get_Name` really is 0x14C6C10,
     * `set_DisplayName` 0x14C6C20, so 0x14C6B60 is right):
     *
     *   ldr  x0, [x19, #0x1a0]     ; cached?
     *   cbz  x0,  slow
     *   ret                        ; yes
     *  slow:
     *   blr  [vtable + 0x60]       ; virtual GetDisplayName()
     *   str  x0, [x19, #0x1a0]     ; fill the cache
     *   ...
     *
     * v128 called it while hovering and the game crashed on hardware.  The
     * reason was NOT "managed getters are unsafe from the render hook": v128
     * passed 0x14C6B60, which is 0x100 below the real entry point and lands in
     * the tail of an unrelated function.  The metadata address is correct.
     */
    constexpr uintptr_t kLocDisplayNameGetOff = SDV_OFF(kLocDisplayNameGetOff, 0x14C6C60);

    /* ---- text drawing ---------------------------------------------------
     *
     * DrawString(SpriteFont, string, Vector2, Color) = 0xC1BB0.
     *
     * This address comes from the AOT metadata, not from reading assembly by
     * hand.  `tools/aot_type.py SpriteBatch` lists SpriteBatch's methods in
     * order, each with its body address:
     *
     *   .ctor 0xC0600    Begin 0xC0820    End 0xC0C80
     *   Draw 0xC0D40 0xC0F70 0xC1590 0xC1240 0xC1660 0xC17E0 0xC1950 0xC1A80
     *   DrawString 0xC1BB0 0xC26F0 0xC27B0 0xC2EB0 0xC3300 0xC33C0
     *
     * Cross-checked against a scan of every `bl` in the 130 MB text segment:
     * Begin 130 callers, End 129, 0xC1590 1237 (Draw(Texture2D, Vector2,
     * Rectangle?, Color) -- the most used overload in the game), 0xC1A80 153,
     * 0xC17E0 80 (the one the avatar pass already uses), 0xC1BB0 88, 0xC26F0
     * 83.  Every count agrees, so these body addresses are exact.
     *
     * !! The project believed for six versions that AOT metadata body addresses
     * are 0x100 too high.  They are not.  That rule came from 0x14C6D10 vs
     * 0x14C6C10, but 0x14C6D10 is the real get_Name (`ldr x8,[x0,#0x1a8];
     * ldr x0,[x8,#0x68]; ret`) and 0x14C6C10 is a function epilogue.  The
     * "correction" was itself the bug.  It cost v128, and then v133..v136,
     * which called 0x17A2BA0 -- an address with **zero callers**, i.e. not a
     * function at all, so three builds crashed on the way in.  Never shift a
     * metadata body address; verify it.
     *
     * Calling convention, verified two ways:
     *
     *   x0  SpriteBatch this   (the body reads [x0+0x48] and compares it to 2,
     *                           the Begin/End state check)
     *   x1  SpriteFont         (the body calls 0xC7250/0xC7260/0xC7280 on it
     *                           and turns the result into the line spacing)
     *   x2  string
     *   w3  Color
     *   s0  position.x         (Vector2 is an HFA: it takes the first two
     *   s1  position.y          float registers, whatever its ordinal)
     *
     * and at live call site 0x17A43B8:
     *
     *   ldr  w3, [x29, #192]   ; colour
     *   mov  x0, x26           ; batch
     *   mov  x1, x21           ; font
     *   mov  x2, x22           ; text
     *   bl   0xc1bb0
     *
     * Six arguments.  No outgoing stack arguments whatsoever. */
    constexpr uintptr_t kDrawStringOff = SDV_OFF(kDrawStringOff, 0xC1BB0);

    /* Game1's own fonts, from the AOT metadata (flags 0x16 = static field, and
     * the type id 0x45e9f0 is the same one SpriteFont has everywhere else).
     * Single dereference, and certainly initialised by the time the map is
     * open.  smallFont is also the semantically right choice: the map labels
     * are small text, and it is what NPCMapLocations uses on PC. */
    constexpr uintptr_t kGame1SmallFontOff    = SDV_OFF(kGame1SmallFontOff, 0xC95BDA0);
    constexpr uintptr_t kGame1DialogueFontOff = SDV_OFF(kGame1DialogueFontOff, 0xC95BD98);

    /* v398: HUD info overlay (UI Info Suite style).
     *
     * Drawing our own text needs a managed System.String, which nothing in this
     * tree created before.  String::.ctor(char*) at 0x137D0 allocates and copies
     * a null-terminated UTF-16 buffer; Marshal.PtrToStringUni (0x12B80) is a
     * three-instruction wrapper that ends in exactly this call with this=null,
     * so passing null is the shape the game itself uses. */
    constexpr uintptr_t kStringCtorCharsOff  = SDV_OFF(kStringCtorCharsOff, 0x137D0u);
    constexpr uintptr_t kGame1SpriteBatchOff = SDV_OFF(kGame1SpriteBatchOff, 0xC95BA88u);
    constexpr uintptr_t kWeatherTomorrowOff  = SDV_OFF(kWeatherTomorrowOff, 0xC95BE18u);
    /* Farmer.daysLeftForToolUpgrade is a NetInt at +0x5B8. */
    constexpr unsigned  kFarmerToolUpgradeDaysOff = 0x5B8u;
    /* Off while the feature is being proven on hardware. */
    constexpr bool kHudInfo = true;
    /* Temporary: draws a grid of 16x16 tiles from the cursors sheet so the
     * weather icons can be located by eye instead of guessed from a
     * disassembly of TV.draw. */
    constexpr bool kIconProbe = true;

    /* HUD layout.  The icon is 32x32 art drawn at an integer scale so the
     * rectangles stay on whole pixels -- 1.5x would put every run on a half
     * pixel and the art would look blurred.  2x = 64x64. */
    /* Layout.  Three rows on a fixed 72px pitch; each row is a 64px-tall band
     * and its icon is centred both in the band and on x=56.  That is what
     * makes the rows read evenly: v414 right-aligned the art, which lined the
     * right edges up but left the 40px luck icon 24px further in than the
     * 64px ones, and it looked shifted.
     *
     *   row 0 (weather) band 20..84   icon 64 tall -> y 20, centre 52
     *   row 1 (luck)    band 92..156  icon 56 tall -> y 96, centre 124
     *   row 2 (tool)    band 164..228 icon 64 tall -> y 164, centre 196
     *
     * Text sits at the band centre minus half a text line (12), and every row
     * starts at x=96. */
    constexpr int kHudIconCx = 56;
    constexpr int kHudBand = 64;
    constexpr int kHudRowPitch = 72;
    constexpr int kHudRowY = 20;
    constexpr float kHudTextX = 96.0f;

    constexpr int kHudIconX = kHudIconCx - 32;      /* 24, art is 64 wide */
    constexpr int kHudIconY = kHudRowY;             /* 20 */
    constexpr int kHudIconScale = 2;
    constexpr float kHudTextY = kHudRowY + 20.0f;   /* 40 */

    /* Daily luck.  The icon is Cursors (50,428,10,14) -- the exact region UI
     * Info Suite 2 uses for it -- drawn at 4x, also like the original.  It is
     * tinted by luck tier, so the colour alone tells you the answer. */
    constexpr int kHudLuckX = kHudIconCx - 20;      /* 36, art is 40 wide */
    constexpr int kHudLuckY = kHudRowY + kHudRowPitch + (kHudBand - 56) / 2;
    constexpr int kHudLuckScale = 4;
    constexpr float kHudLuckTextY = kHudLuckY + 16.0f;   /* 112 */

    /* Tool upgrade.  The icon is the tool's own sprite, taken from the game's
     * tool sheet -- that is what UI Info Suite 2 does, and what the player
     * expects (a copper axe while the axe is in the shop).  Our own hammer is
     * only the fallback for when the tool cannot be read. */
    constexpr int kHudToolX = kHudIconCx - 32;      /* 24, art is 64 wide */
    constexpr int kHudToolY = kHudRowY + 2 * kHudRowPitch;   /* 164 */
    constexpr int kHudToolScale = 2;
    constexpr float kHudToolTextY = kHudToolY + 20.0f;   /* 184 */

    /* Farmer.toolBeingUpgraded (NetRef<Tool>) -> Tool.indexOfMenuItemView
     * (NetInt) -> Game1.getSquareSourceRectForNonStandardTileSheet(sheet, 16,
     * 16, index).  Offsets from the AOT dump; the helper is the game's own, so
     * the sheet width never has to be assumed. */
    constexpr uintptr_t kFarmerToolUpgradingOff = 0x5B0u;
    constexpr uintptr_t kToolMenuIndexOff = 0xB0u;
    constexpr uintptr_t kToolSpriteSheetOff =
        SDV_OFF(kToolSpriteSheetOff, 0xC95BD48u);
    constexpr uintptr_t kGetSquareRectOff =
        SDV_OFF(kGetSquareRectOff, 0x1492D40u);


    /* Farmer.get_DailyLuck, from the AOT table (same dump as get_Stamina
     * 0x13B9270, which the tractor code has been using since v274).  It is a
     * property getter, so it returns a double in d0 and takes `this` in x0.
     * Calling it is cheaper and safer than re-deriving the value: the real
     * formula is clamp(team.sharedDailyLuck + 0.025 if the player owns the
     * Special Charm), and the charm lives in a mailReceived hash set that we
     * have no business reading by hand. */
    constexpr uintptr_t kGetDailyLuckOff = SDV_OFF(kGetDailyLuckOff, 0x13B85E0u);

    /* Game1.currentLocation / Character tile accessors. */
    constexpr uintptr_t kGetTileXOff = SDV_OFF(kGetTileXOff, 0x1224440);
    constexpr uintptr_t kGetTileYOff = SDV_OFF(kGetTileYOff, 0x1224470);
    /* Character.get_Tile body (update NSO E7F84509...).  Returns the tile as
     * two floats and refreshes the internal cache, so it works for every NPC
     * instead of only the ones the game happened to query this frame. */
    constexpr uintptr_t kGetTileBodyOff = SDV_OFF(kGetTileBodyOff, 0x1220380);

    /* WorldMapManager.GetPositionData(GameLocation, Point, null, out).
     * The out parameter is a 32-byte MapAreaPositionWithContext returned
     * through x8 -- declaring the return type as a 32-byte struct makes the
     * compiler emit exactly that, which is why the earlier attempt (passing a
     * buffer as a normal argument) produced all zeros.  Byte 0x18 is the
     * "this location has a position" flag. */
    constexpr uintptr_t kGetPositionDataOff = SDV_OFF(kGetPositionDataOff, 0x1BBD930);

    /* 0x1BBC10: MapAreaPositionWithContext -> Vector2 map pixel.
     *     ldp x8,x1,[x0] ; ldr x2,[x0,#0x10] ; b 0x1BBB510
     * and 0x1BBB510 does  rect.xy + rect.wh * normalisedTile. */
    constexpr uintptr_t kPixelPosOff = SDV_OFF(kPixelPosOff, 0x1BBC10);

    struct Ctx { u64 w0, w1, w2, w3; };
    struct Vec2 { float x, y; };

    using GetTileFn = int (*)(void* self);
    using GetItemFn = void* (*)(void* list, int index);
    using GetPosFn  = Ctx  (*)(void* location, u64 tile, void* opt);
    using GetLocOfFn = void* (*)(void* self);
    using GetTilePointFn = u64 (*)(void* self);

    /* Character.get_currentLocation / get_TilePoint -- both are what vanilla
     * drawMiniPortraits calls (0x17E9034, 0x17E8FB4). */
    constexpr uintptr_t kGetCurrentLocationOff = SDV_OFF(kGetCurrentLocationOff, 0x1220660);
    constexpr uintptr_t kGetTilePointOff      = SDV_OFF(kGetTilePointOff, 0x1220460);
    using GetPixFn  = Vec2 (*)(void* ctx);

    /* Game1.get_locations(): List<GameLocation>, items at +0x10, size at +0x18. */
    constexpr uintptr_t kGetLocationsOff = SDV_OFF(kGetLocationsOff, 0x1440790);
    using GetLocationsFn = void* (*)();
    using DrawMiniFn = void (*)(void* self, void* batch, float alpha);
    using GetPlayerFn = void* (*)(void*);

    /* 0xC1BB0 DrawString(SpriteFont, string, Vector2, Color).
     *
     * Six arguments, no stack arguments.  The 26-argument signature this file
     * carried up to v136 described a function that does not exist -- see the
     * note next to kDrawStringOff. */
    using DrawStringFn = void (*)(
        void* batch, void* font, void* text,
        unsigned color, float x, float y);

    GetPlayerFn s_origGetPlayer = nullptr;
    DrawMiniFn  s_origDrawMini  = nullptr;

    /* List<T>.get_Item, confirmed by disassembly at 0x5d3c8c:
     *   ldr w8, [x0, #0x18]   ; count
     *   ldr x8, [x0, #0x10]   ; wrapper
     *   ldr x9, [x8, #0x20]   ; length holder
     *   ldr x8, [x8, #0x10]   ; the real element array
     *   ldr x0, [x8, w1, sxtw #3]
     * Hooking it makes the game hand over the lists it walks, which is the only
     * way left to learn an offset without guessing at structure. */
    constexpr uintptr_t kGetItemOff = SDV_OFF(kGetItemOff, 0x5D3C8C);
    using GetItemFn = void* (*)(void* list, int index);
    GetItemFn s_origGetItem = nullptr;
    void* s_gotLists[8] = {nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr, nullptr};
    int   s_gotCount = 0;
    void* s_gotElem = nullptr;

    void* HookGetItem(void* list, int index) {
        void* r = s_origGetItem(list, index);
        s_gotElem = r;
        if (s_gotCount < 8 && list != nullptr) {
            bool known = false;
            for (int i = 0; i < s_gotCount; i++) {
                if (s_gotLists[i] == list) {
                    known = true;
                    break;
                }
            }
            if (!known) {
                s_gotLists[s_gotCount] = list;
                s_gotCount++;
            }
        }
        return r;
    }
    unsigned s_hits = 0;
    unsigned s_mapDraws = 0;
    /* Marker diagnostics: log the first N markers of a session so a wrong
     * position can be traced to a specific NPC + location + tile without
     * guessing from screenshots.  Capped so the hot path stays quiet. */
    unsigned s_frame = 0;
    unsigned s_round = 0;


    /* Hardware bisect switch for the world-map feature.
     *   0 = full behaviour
     *   1 = hook installed, original called, no marker work (isolation test)
     *  91 = full behaviour + the compact diagnostic rows (see below)
     * Retail crashes when the map page opens, so this lets one build decide
     * whether the hooked function is wrong or our marker code is. */
    constexpr int kMapStage = 327;
    /* The avatar block and its build tag both derive from this one constant, so
     * the switch and the gate can never disagree again.  Twice now (stage 44 and
     * stage 95) a stage number was bumped while the `if (kMapStage == N)` was
     * left behind, which silently disabled the new code and fell through to an
     * older readout -- the readings then looked like "nothing changed". */
    constexpr int kStageAvatar = 327;
    /* v207 guard.  v206 shipped with kMapStage = 205 and kStageAvatar = 206,
     * so `kMapStage >= kStageAvatar` was false and the ENTIRE avatar block --
     * badges, hover and the hex readout -- silently disappeared, while the
     * map-page marker that lives outside the gate kept drawing.  The trap is
     * always the same direction: the new constant is bumped, the old one is
     * not.  kMapStage only needs to differ from kStageAvatar for the 0/1/91
     * hardware bisect builds, so anything >= 100 must match. */
    static_assert(!(kMapStage >= 100 && kMapStage != kStageAvatar),
                  "kMapStage and kStageAvatar must be bumped together");
    /* hexnum prints hexadecimal digits, so a stage number has to be converted to
     * "decimal digits read as hex" before it is drawn: 95 must show as 95, not
     * as 5F.  Deriving it from kStageAvatar keeps the gate and the tag in sync.
     * Digit count is whatever the stage needs, so passing 101 to a three-digit
     * hexnum prints 101 rather than the low two digits. */
    constexpr unsigned StageTag(int n) {
        unsigned out = 0;
        unsigned mul = 1;
        if (n <= 0) {
            return 0;
        }
        while (n > 0) {
            out += static_cast<unsigned>(n % 10) * mul;
            mul *= 16;
            n /= 10;
        }
        return out;
    }

    /* Marker placement offset, in the same unit as the interpolated px/py.
     *
     * Was hardcoded to +32 on both axes, inherited from the vanilla formula
     * `screen = bounds + pixel + 32`.  An on-console screenshot showed every
     * avatar landing (+18, +22) *screen* pixels away from where the world-map
     * table puts it, which is ~7x8 texture pixels -- enough to drop Willy from
     * the fish shop into the sea.  Rather than keep guessing the unit
     * conversion, the offset is now a named knob so one build can measure it.
     *
     * kStage95 calibration build: 0 / 0.  Compare the resulting screenshot
     * against the expected anchors, then set these to the residual. */
    /* Birthday toast state.  Namespace scope so both the map-page readout and
     * HookDrawHud (defined later in this file) can reach it. */
    static unsigned g_hudState = 0;
    static unsigned g_hudChars = 0;
    static int g_bdayNotified = -1;
    /* FastAnimations slice 1: extra advances issued this frame / total. */
    static unsigned g_animExtra = 0;
    static unsigned g_animTotal = 0;
    static unsigned g_animState = 0;   /* 1 = player+sprite ok, 2 = no player */
    /* Animation multiplier, driven by the checkbox we add to the options page.
     *
     * v156/v157/v158 changed it live and froze the game.  The likely reason is
     * that the change landed WHILE an animation was mid-flight, so the number of
     * advances per frame changed under the state machine.  So the new value is
     * only latched when the current single animation has finished
     * (self+0xA4 == 0), never in the middle of one. */
    /* Both start at 1 = OFF.  They used to start at 2, which meant every
     * feature ran accelerated from boot until the player happened to open the
     * options page (only then does HookDrawHud see g_ourElem and start
     * mirroring the checkbox) and finished one animation.  A player who went
     * straight from loading a save to the bus got a fast bus with the box
     * unticked.  "Unticked means off" has to hold from the first frame. */
    static int g_animFast = 1;
    static int g_pendingFast = 1;
    /* v305: the Mod page is a master switch with children.
     *
     *   row 0     动画加速2倍     which = kOptIndex,  state from Options+0x24
     *   row 1     启用拖拉机       the master
     *   row 2..4  子开关           shown only while the master is on
     *
     * Every row is a separate OptionsCheckbox appended to the vanilla options
     * list and swapped into the front of it while the Mod tab is current.  The
     * rows below the master are hidden by swapping them back out, not by
     * rebuilding the page -- that keeps the "expand" instant and allocation
     * free. */
    /* v312: one row per tool family, plus an independent map row.
     *
     *   row 0      动画加速2倍     which 35 -> Options+0x24 (the game's own)
     *   row 1      地图增强         independent of the master
     *   row 2      启用拖拉机       master
     *   row 3..10  子开关           shown only while the master is on
     *
     * v313: 地图增强 moved from the bottom to row 2.  The page draws a fixed
     * number of rows and only scrolls if the game's own scroll state is left
     * alone; putting the independent switch third means it is visible whether
     * the children are expanded or not.
     *
     * v313 also STOPPED borrowing Options fields for the eight children.  Two
     * of the ten we used were not inert on a console: `_hardwareCursor` (+0x2F)
     * is what Game1 checks before drawing the finger cursor
     * (`if (!freezeControls && !options.hardwareCursor)`), so seeding it to 1
     * hid the cursor; `windowedBorderlessFullscreen` / `vsyncEnabled` are read
     * by the display setup.  Only the two multiplayer booleans below are
     * borrowed now -- they are read when hosting and never in single player,
     * and v311 already ran them on hardware without trouble.
     *
     * The eight children therefore live in memory only: they keep their state
     * while the game is running (including across menu opens) and start ON
     * again after a restart.  That is the honest trade: a switch is not worth
     * a hidden cursor or a hang. */
    constexpr int kRowCount = 12;
    constexpr int kRowMap = 1;
    constexpr int kRowMaster = 2;
    constexpr int kRowFirstSub = 3;
    constexpr int kRowLastSub = 10;
    constexpr int kRowHoe = 3;
    constexpr int kRowWater = 4;
    constexpr int kRowAxe = 5;
    constexpr int kRowPick = 6;
    constexpr int kRowScythe = 7;
    constexpr int kRowCombat = 8;
    constexpr int kRowMilk = 9;
    constexpr int kRowPlant = 10;
    /* v372: automation on/off.  Top-level row, after the tractor group. */
    constexpr int kRowAutomate = 11;
    /* v314: THE WHOLE BITMAP LIVES IN ONE INT FIELD.
     *
     * Borrowing one bool per switch is what broke v312: two of the ten were
     * read by the console build (one of them hid the cursor).  Every bool in
     * Options is read somewhere, so there is no set of ten inert ones.
     *
     * `preferredResolutionX` (+0x70, int) is different: the only reader in the
     * game is Game1.toggleNonBorderlessWindowedFullscreen, a PC window-mode
     * function that does not exist in this build at all (the Switch Game1
     * metadata has 527 methods and no such name; SetWindowSize,
     * refreshWindowSettings and CheckValidFullscreenResolution do not touch
     * +0x70 either).  An int also holds all ten bits at once, so it is ONE
     * field instead of ten.
     *
     * Encoding: kMaskTag | bits.  Anything without the tag is "never written"
     * (first run, or a real resolution the game put there), which means all
     * switches on. */
    constexpr unsigned kMaskOff = 0x70u;
    /* v373: List<OptionsElement>.Add.  Found by counting call targets inside
     * OptionsPage..ctor -- it is the one called 41 times, and its body is
     *     count = list[0x18]; capacity = *(list[0x10][0x20]);
     *     if (count == capacity) Grow(1);
     * which is exactly Add.  We need it because AppendToList deliberately never
     * grows, and the page's list is already full (game options + our 11 rows). */
    constexpr uintptr_t kOptListAddOff = SDV_OFF(kOptListAddOff, 0x170D370u);
    /* v375: the list's Grow.  Add calls it as `Grow(1)` when count == capacity
     * (0x170D3B8).  We call Grow ourselves and then do the store, so we do not
     * depend on Add's argument order at all. */
    constexpr uintptr_t kOptListGrowOff = SDV_OFF(kOptListGrowOff, 0x1711430u);
    constexpr unsigned kMaskTag = 0x4000u;
    constexpr unsigned kMaskBits = 0xFFEu;   /* bits 1..11 */
    static unsigned s_fileMask = kMaskBits;
    static unsigned s_fileMaskOk = 0;
    /* v385: serialisation-chain diagnostic (read-only). */
    static unsigned s_dbgHookRuns = 0;
    static unsigned s_dbgCoIsLive = 0;
    static unsigned s_dbgCoVal = 0;
    static unsigned s_dbgWrote = 0;
    static unsigned s_dbgLoadRuns = 0;
    static unsigned s_dbgDisc = 0xFFFFFFFFu;
    static unsigned s_dbgLiveVal = 0;
    /* v390: disconnectDay mask read throttle. */
    static unsigned s_discFrame = 0;
    static unsigned s_discDirty = 0;    /* a switch changed; write it next Update */
    /* The value to inject into the SERIALISED copy at save time: the live
     * Options is not the object the serializer reads. */
    static unsigned g_savedMask = kMaskTag | kMaskBits;
    /* v379: frames since the last save request we issued for a switch change. */
    static unsigned s_saveAskFrame = 300u;
    /* v382: a switch changed; one save request goes out 2 s later. */
    static unsigned s_maskDirty = 0;
    static unsigned s_maskFrame = 0;
    static unsigned char s_rowPrev[kRowCount] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    static unsigned s_rowPrevInit = 0;
    static unsigned s_rowsApplied = 0;
    static void* g_ourElems[kRowCount] = {};
    static unsigned g_ourCount = 0;
    /* v317: the options list rebuilt rather than shuffled.  s_origOrder holds
     * the list exactly as the game built it, so leaving the tab is a plain
     * write-back and cannot leave a permutation behind. */
    constexpr int kMaxOptSlots = 64;
    static void* s_origOrder[kMaxOptSlots] = {};
    static int s_origCount = 0;
    static unsigned s_shrinkState = 0;   /* 0 = not shrunk, 1 = shrunk */
    /* v326: how many rows the current shrink was built for.  The row set
     * changes when 启用拖拉机 is toggled, and the list has to be rebuilt for
     * the new count -- that is the collapse/expand. */
    static int s_shrinkCount = -1;
    /* v318: what the tab/page plumbing looks like right now, for the readout.
     * pages = entries in GameMenu.pages, pageIdx = where our duplicated page
     * sits in it (-1 = not there). */
    static int g_dbgPages = -1;
    static int g_dbgPageIdx = -2;
    static int g_dbgCur = -1;
    static int g_dbgTabIdx = -2;   /* where our tab sits in `tabs` */
    static int g_lastTabRet = -1;  /* last value our name hook returned for it */
    static int g_dbgNameMatch = -1; /* does the tab's name pointer equal s_modTabName? */
    /* Latched while the Mod tab is actually current -- the readout is drawn on
     * the map page, where currentTab is 3 and none of this is live. */
    static int g_sawModShrink = -1;  /* s_shrinkState seen on the Mod tab */
    static int g_sawModCount = -1;   /* g_ourCount seen on the Mod tab */
    static int g_sawAppend = -1;     /* g_appendState seen on the Mod tab */
    /* Capacity of the options backing array, captured when we appended.  The
     * readout shows it because "only some rows appeared" is a capacity answer,
     * not a code answer. */
    static int g_listCap = 0;
    /* Per-row state, read straight out of +0x51 each frame.  Rows 1..10 are
     * seeded from the file at boot; row 0 is the game's own option. */
    static unsigned g_rowOn[kRowCount] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    static void* g_ourElem = nullptr;   /* row 0, the HUD readout reads this */
    /* Mirror of g_rowOn[kRowMaster], used by every tractor hook. */
    static unsigned g_tractorOn = 1;
    /* Mirror of g_rowOn[kRowMap], used by the map draw hook. */
    static unsigned g_mapOn = 1;
    /* Stage 185: the checkbox state is stored in the game's own Options
     * object instead of a file.
     *
     * The file channel is dead on retail: svcConnectToNamedPort("sm:") comes
     * back as KernelError_OutOfResource (0x10801), so fs never initialises and
     * neither the SD card nor the save data can be opened.  Options, on the
     * other hand, is already persisted by the game -- the player's UI zoom
     * survives a restart -- so borrowing one of its bools costs no file I/O at
     * all and is saved through exactly the same path as a real option change.
     *
     * Field table from .tmp/meta_options.json (StardewValley.Options, size
     * 0x170, factory 0x7333058):
     *   0x24 showMPEndOfNightReadyStatus -- public, so the game's serializer
     *        picks it up, and it only drives the "everyone ready" marker at the
     *        end of a multiplayer night, so it does nothing in single player.
     *        _hardwareCursor (0x2f) would be even more inert but is private, and
     *        the settings serializer only writes public fields.
     *   0x158 optionsDirty -- set by changeCheckBoxOption after every store.
     *        Nothing in the binary reads it (verified), so it is not what makes
     *        the file appear; it is set here only because the game sets it.
     */
    constexpr unsigned kOptFlagOff = SDV_OFF(kOptFlagOff, 0x24u);
    constexpr unsigned kOptDirtyOff = SDV_OFF(kOptDirtyOff, 0x158u);
    constexpr unsigned kOptSize = SDV_OFF(kOptSize, 0x170u);
    /* THE checkbox index.  v185 passed 0 here and that was the whole bug.
     *
     * The two halves of a checkbox are both switched on OptionsElement
     * `whichOption` (+0x28), and they are exactly symmetric -- both jump tables
     * decoded end to end, index -> Options offset:
     *
     *   index 0  -> 0x10 autoRun            index 24 -> 0x20 screenFlash
     *   index 3  -> 0x11 dialogueTyping     index 26 -> 0x2f _hardwareCursor
     *   index 5  -> 0x28 windowedBorderless index 29 -> 0x22 snappyMenus
     *   index 7  -> 0x12 showPortraits      index 30 -> 0x2c ipConnections
     *   ...                                 index 35 -> 0x24 <-- ours
     *   index 46 -> 0x2b useAlternateFont   index 43 -> 0x25 muteAnimalSounds
     *
     *   read : Options.getCheckBoxOption  0x1a54e60  (Options in x0, elem in x1)
     *          ldr w8,[x1,#0x28] -> jump table 0x98b7236 (BYTE entries)
     *          -> ldrb w8,[x0,#<off>] ; strb w8,[x1,#0x51]
     *   write: Options.changeCheckBoxOption 0x1a530f0
     *          ldr w1,[elem,#0x28] -> jump table 0x98b7188 (HALFWORD entries)
     *          -> strb w8,[x0,#<off>] ; strb 1,[x0,#0x158]
     *
     * Index 35's write arm is 0x1a53198 and it is two instructions: store the
     * byte, set optionsDirty, return.  No side effects, no other field touched.
     *
     * So passing 35 to OptionsCheckbox..ctor makes the GAME do all of it:
     *   - the ctor's last act is getCheckBoxOption(options, this), which loads
     *     isChecked straight out of Options+0x24.  Read-back is free.
     *   - receiveLeftClick 0x17f6920 flips +0x51 itself, then calls
     *     changeCheckBoxOption(options, whichOption, +0x51).  Write is free.
     *
     * v185 had no such help.  It passed 0, so the ctor seeded isChecked from
     * Options+0x10 (autoRun) and a click wrote autoRun -- while our own
     * hand-rolled sync in SyncFastFromOptions was supposed to copy +0x51 into
     * +0x24 afterwards.  Passing 35 deletes that whole sync, and with it the
     * risk of silently flipping a real game setting. */
    constexpr int kOptIndex = 35;
    /* v303: the second checkbox needs its own `which`.  Both 35 and 31 are the
     * default branch of BOTH game tables (getCheckBoxOption reads nothing,
     * changeCheckBoxOption only sets optionsDirty), so neither writes a real
     * option -- which is exactly what we want: the state is read straight out
     * of the element's +0x51 and acted on by us. */
    constexpr int kOptIndex2 = 31;
    /* StartupPreferences.loadPreferences 0x1adf4a0.  Hooked only to capture
     * `this`: the boot path at 0x181a1c4 does
     *
     *     loadPreferences()                    (x0 = the singleton)
     *     clientOptions (+0x50) = Game1.get_options()
     *     savePreferences()                    (same object)
     *
     * so the instance loadPreferences sees is exactly the one savePreferences
     * must be called on.  Capturing it from the hook is free and cannot be
     * wrong, unlike hunting for a static that holds it. */
    constexpr uintptr_t kLoadPrefsOff = SDV_OFF(kLoadPrefsOff, 0x1ADF4A0u);
    constexpr uintptr_t kSavePrefsOff = SDV_OFF(kSavePrefsOff, 0x1ADEE90u);
    /* v199 probe: writeSettings is the serialise step inside
     * _savePreferences (call site 0x1ADF20C, `bl #0x1adf2f0` with
     * (this, writer)).  _savePreferences itself is kSavePrefsImplOff, defined
     * further down next to the direct-call experiment that introduced it.
     * Bracketing the two separates "cannot open the file" from "cannot
     * serialise", which is the fork we are stuck on. */
    constexpr uintptr_t kWriteSettingsOff = SDV_OFF(kWriteSettingsOff, 0x1ADF2F0u);
    /* v206: the READ side of the round trip.
     *
     * v205 settled the write: row 4 came back 7F202011, so _savePreferences
     * AND writeSettings both returned and the object handed to the serializer
     * carried +0x24 = 1.  The checkbox still does not survive a restart, so
     * the value dies somewhere between the file and the live Options.  These
     * two functions are the whole of that path:
     *
     *   readSettings 0x1ADFC30  (this, reader).  Deserialises the file into a
     *       StartupPreferences and copies +0x24 with
     *           ldr w8,[x0,#0x24] ; str w8,[x19,#0x24]
     *       It never touches the live Options -- reading +0x50 after it
     *       returns therefore reports what the FILE said.
     *
     *   set_options 0x1443540   (opts).  Five instructions:
     *           adrp/add/ldr -> Game1 instance, str x0,[x8,#0x90], ret
     *       The boot wiring at 0x178ADC4 calls it with prefs+0x50.  Reading
     *       its argument reports what the game actually INSTALLED.
     *
     * Both are cheap, allocation-free and non-blocking, which matters: they
     * run on the boot thread before the first HUD draw. */
    /* v211: ShippingMenu.update -- the end-of-day earnings screen.
     *
     * The intro that shows each category one at a time is gated by a single
     * millisecond countdown at instance +0xE8:
     *
     *     0181d850: mov  w8, #0x7d0        ; 2000 ms, armed per category
     *     0181d854: str  w8, [x19, #0xe8]
     *     0181d89c: ldr  w21, [x19, #0xe8] ; timer
     *     0181d8a0: cmp  w21, #1
     *     0181d8a4: b.lt ...               ; < 1 -> skip (already done)
     *     0181d8c4: subs w8, w21, w0       ; timer - elapsed
     *     0181d8c8: str  w8, [x19, #0xe8]  ; write back
     *     0181d8cc: b.gt ...               ; still > 0 -> nothing yet
     *     0181d8d4: strb w8, [x19, #0x118] ; _hasFinished = 1
     *
     * That is the same shape as the dialogue typewriter (DialogueBox+0xC0):
     * a countdown, so subtracting an extra frame's worth makes it expire
     * sooner.  The 2000 ms is an INLINED immediate -- the metadata's
     * `timePerIntroCategory` static has no code reference at all (checked with
     * three different adrp+add scan patterns, and there is not one `str s` to a
     * static float anywhere in the text), so writing the static is impossible
     * and this countdown is the lever instead.
     *
     * Only +0xE8 is touched.  +0xEC is a 50 ms timer that re-arms itself
     * (`mov w8,#0x32` at 0181db54) for the smoke puffs -- shortening that
     * changes the effect, not the waiting.  +0xE0 / +0xE4 / +0xF0 are separate
     * phases with different expiry handling and are left alone until this one
     * is measured. */
    constexpr uintptr_t kShippingUpdateOff = SDV_OFF(kShippingUpdateOff, 0x181D1F0u);
    constexpr unsigned kShippingTimerOff = SDV_OFF(kShippingTimerOff, 0xE8u);
    constexpr unsigned kShippingFinishedOff = SDV_OFF(kShippingFinishedOff, 0x118u);
    constexpr unsigned kShippingActivatedOff = SDV_OFF(kShippingActivatedOff, 0x119u);
    /* ---- v212: our own tab in the GameMenu -----------------------------
     *
     * Everything below was verified against E7F84509 before a line was written:
     *
     *   OptionsPage..ctor 0x17F8C80  (this=0 allocates) x, y, w, h
     *       prologue: w23=w1 w22=w2 w20=w3 w21=w4, cbnz x0 -> allocate
     *   the game's own call at 0x179695C passes
     *       x=[GameMenu+0x28] y=[+0x2c] w=[+0x30]+w22 h=[+0x34]
     *   -- so we copy the geometry off the LIVE options page instead of
     *   reproducing w22 (0x60 / 0xc0 / 0, gamepad-dependent).
     *
     *   ClickableComponent..ctor 0x175A0D0  (this=0 allocates)
     *       body: stp x22,x21,[x23,#0x10]   <- bounds, 16 bytes in x1:x2
     *             stp x20,x19,[x23,#0x20]   <- name@+0x20, label@+0x28
     *       i.e. (this, Rectangle bounds, string name, string label).  A
     *       16-byte struct goes in two GP registers, so we pack two u64s.
     *
     *   String.CreateString 0x3A0100  (char16* value, int start, int length)
     *       `add x1, x21, w20, uxtw #1` -- stride 2, so UTF-16.
     *
     *   getTabNumberFromName 0x1797E00  instance, name in x1
     *   getLabelOfTabFromIndex 0x1797FB0  STATIC, index in w0
     *
     * Lists: `tabs` @+0x80, `pages` @+0x88, both 10 used / capacity 16 (the
     * ctor adds 10 to a default List, so the growth sequence 0->4->8->16).
     * Appending is the same manual slot write the options page already uses;
     * List<T>.Add is deliberately NOT called from native code.
     *
     * If any step fails the mod leaves the tab alone and keeps appending its
     * checkbox to the vanilla options page -- see the kModTab gate in the
     * append block.  A failed experiment must cost nothing. */
    /* v216: STAGED BISECT.  The tab is back ON, the page is still OFF.
     *
     * v212 and v213 both froze the console when the menu opened, and two
     * hypotheses have already died: the neighbour chain (GameMenu.update does
     * not walk the tabs at all -- see below) and the List<T> layout.  Two
     * boots have produced no readings, because a frozen frame is never
     * presented, so the state byte was never visible.
     *
     * Static analysis (2026-10-27) says the game code cannot spin on our
     * extra entry:
     *
     *   GameMenu.update 0x17981F0 is 12 instructions: `bl 0x179fc40` (a bare
     *     `ret` stub) and then a TAIL CALL to `pages[currentTab].update()`.
     *     It never reads `tabs`, `numberOfTabs` or the neighbours.
     *   GameMenu.draw's tab loop is an enumerator bounded by `tabs._size`;
     *     a 3-char name that does not equal "map" falls straight through the
     *     length switch (0x179876c: `sub w8,w0,#3; cmp w8,#8; b.hi`) and
     *     draws nothing -- no icon, no label, no loop.
     *   `numberOfTabs` (0xC95E840) has exactly ONE reference in the whole
     *     text: 0x1797A7C, inside receiveGamePadButton.  Writing 11 there is
     *     nearly inert.
     *
     * So a hang is more likely a heap problem than a spin: an object the GC
     * cannot walk makes the mark phase die, which looks exactly like "game
     * frozen, console alive, music stopped", and it would fire precisely at
     * the allocation burst of OptionsPage..ctor.
     *
     * That is why this build also drops the one API the tab code depended on
     * that nothing else ever used: String.CreateString 0x3A0100.  The
     * checkbox that has shipped since v168 does NOT use it -- it takes the
     * player's name, PadLeft+Substring to get a fresh string, and overwrites
     * the characters in place (see the append block).  MakeFreshString() below
     * is that proven recipe, so this build has no unproven string path left.
     *
     *   kModTab  = true   the tab component, its append, numberOfTabs
     *   kModPage = false  NO OptionsPage, NO element, NO append to `pages`
     *
     * getTabNumberFromName maps our name to 8 (the vanilla Options page) in
     * this stage, so clicking the tab opens a real page and the checkbox is
     * still there.  Nothing is lost if the experiment fails.
     *
     * ---- RESULT (v216-f0efcc22, read on hardware 2026-10-27) ----
     *   row 1 = 01 01 23 00  ->  s_tabState = 1, appendState = 1, count = 35,
     *   menu did not freeze.  So an 11th ClickableComponent in `tabs`, its
     *   neighbour fields and the manual append are all safe, and the freeze
     *   that killed v212/v213 is in the PAGE path (OptionsPage..ctor, the
     *   element, the `pages` append, or numberOfTabs).
     *
     * ---- v217: the page is back on, and it is the only new variable ----
     *   kModPage = true.  Everything that v216 proved stays exactly as it was.
     *   Two ordering changes, neither of which is observable on its own:
     *     - the tab is built and appended BEFORE the page, so s_modTabName is
     *       behind a real GC root when OptionsPage..ctor's allocation burst
     *       runs (a native static is not a root);
     *     - s_pageReady gates both numberOfTabs and the name->index mapping,
     *       so a failure halfway leaves the tab pointing at page 8.
     *
     * If v217 freezes, the next bisect is "page exists but is unreachable":
     * build and append the page, keep s_pageReady at 0.  That splits
     * OptionsPage..ctor from the reachability path in one boot.
     *
     * ---- RESULT (v217-04a9d88d, seen on hardware 2026-10-27) ----
     *   The tab is there, the page opens, the only checkbox on it is ours,
     *   nothing freezes, and the strip already shows the text "Mod".  What it
     *   does NOT show is an icon, because GameMenu.draw picks the icon by
     *   name and "Mod" is not in its table.
     *
     * ---- v218 was an attempt to paint that icon, and it froze the game ----
     *   It hooked GameMenu.draw and called DrawString at its tail.  v219 is
     *   v217 with that hook removed -- nothing else differs, so the tab
     *   behaviour here is exactly the build that was verified on hardware.
     *   See the note next to kGameMenuUpdateOff for why the idea was wrong.
     *   If an icon is ever wanted it must be drawn by a hook that owns its
     *   own Begin/End pair, not inside GameMenu.draw's open batch.
     *
     * ---- v219 removed that hook, and the game still froze on the page ----
     *   Correction, and it matters: v217 did NOT freeze.  The user has a
     *   screenshot of the mod page open, checkbox label and all.  So v217
     *   works, v219 (the same code plus a stage number) does not, and v221
     *   (string lifetimes fixed) does not either.  A page that opens on one
     *   boot and freezes on another is not a logic bug we can read off the
     *   source; four builds have now gone into guessing at it.
     *
     * ---- v222: stop building the page, point the tab at the game's own ----
     *   The tab, its append, numberOfTabs and the pages append are all shared
     *   with v217, which worked.  The only thing v217 does that the game never
     *   does is hand the menu a page WE constructed.  So v222 removes exactly
     *   that: pages[10] is the vanilla Options page object itself, appended a
     *   second time.  Nothing is constructed, nothing is truncated, no
     *   OptionsPage..ctor runs -- and the checkbox is still there, because the
     *   v211 append path puts it in the vanilla page anyway.
     *
     *   It also bisects cleanly: if this still freezes, the page object was
     *   never the problem and the fault is in switching to tab 10 at all
     *   (numberOfTabs / the pages append / the neighbour ids), which means the
     *   separate page is not reachable by this route and v215 is where this
     *   ends.  If it works, the dedicated page is a drawing problem, not a
     *   menu-structure problem, and it can be attacked with a draw hook that
     *   owns its own Begin/End.
     *
     * Set kModTab = false for the known-good build (identical to v215). */
    constexpr bool kModTab = true;
    /* ---- v226: THE CTOR IS THE LAST SUSPECT STANDING ----
     *
     * kModPage = false / kDupPage = true is v222, verified on hardware: the Mod
     * tab opens the game's own Options page, our checkbox is in it, nothing
     * freezes.
     *
     * The v224 note used to say the freeze came from the vanilla OptionsPage
     * code driving a page whose `options` list (1) is shorter than its
     * `optionSlots` (7).  THAT IS WRONG and has been removed from the record.
     * Both draw (0x17FBA90) and performHoverAction (0x17FB880) bounds-check
     * `slot + currentItemIndex` against `options._size` and skip the slot when
     * it is out of range, so a short list is safe:
     *
     *     017fb8f4: ldr  w8, [x21, #0x78]   ; currentItemIndex
     *     017fb8f8: tbnz w8, #0x1f, -> skip ; negative: skip the whole pass
     *     017fb8fc: ldr  x0, [x21, #0xa0]   ; options
     *     017fb900: add  w1, w22, w8        ; slot + currentItemIndex
     *     017fb904: ldr  w8, [x0, #0x18]    ; options._size
     *     017fb908: cmp  w1, w8
     *     017fb90c: b.ge -> skip            ; out of range: this slot is not used
     *
     * What the version table actually separates is simpler, and it was in
     * front of us the whole time:
     *
     *   froze   v212 v213 v217 v218 v219 v220 v221 v223 v224  -- all call the ctor
     *   worked  v215 v216 v222 v225                          -- none call the ctor
     *
     * The game itself constructs an OptionsPage in exactly ONE place in the
     * whole 130 MB text section -- `bl 0x17F8C80` at 0x179695C, inside
     * GameMenu..ctor (the other hit, 0x7229AE0, is the reflection invoker).
     * Calling it later, from GameMenu.update, is a path the game never takes.
     *
     * And it is not a small call.  From docs/offsets/meta_optionspage.json the
     * ctor is 0x1760 bytes with 187 calls: 41 x options.Add (0x170D370),
     * 18 x OptionsCheckbox..ctor (0x17F6780), ~60 allocations through
     * il2cpp_object_new (0x780), SEVEN lazy class-init pairs
     * (0x7C438F0/0x7C43900) and SIX exception-handler regions whose landing
     * pads are 0x7C43910/0x7C43920.  This project's own rule (TECHNIQUES.md
     * and BRIEF_FOR_GPT.md) is that a managed exception thrown from inside a
     * native hook hangs the console -- "游戏卡死、系统正常、音乐停" is exactly
     * that signature, nine times.
     *
     * So v226 asks one question, and only one: DOES CALLING THE CTOR AT ALL
     * KILL IT?  The probe below calls it and throws the result away -- no
     * append, no `pages` entry, no numberOfTabs change.  The tab still points
     * at the vanilla page, so if the menu opens at all it behaves exactly like
     * v222.  See kModProbe.
     *
     * The v223/v224 code is kept below, gated, because it is the only place
     * that records the rooting order that turned out to be right:
     * construct with a placeholder -> append to the managed list -> create the
     * string -> write it in. */
    /* v302: build and append the checkbox from TryBuildModTab instead of from
     * the HUD draw path.  The HUD path made the element's existence depend on
     * the Options page having been DRAWN at least once, and it tangled the
     * append with a per-frame guard.  Set to false to fall back to the old
     * behaviour without touching anything else. */
    constexpr bool kAppendInTab = true;
    constexpr bool kModPage = false;
    constexpr bool kDupPage = true;
    /* ---- v226 probe: call OptionsPage..ctor once, discard the object ----
     *
     * Outcomes, and what each one means:
     *
     *   menu freezes as it opens          -> the ctor call is the killer.  The
     *                                        dedicated page is then only
     *                                        reachable by hooking draw
     *                                        (0x17FBA90) and painting it
     *                                        ourselves; no ctor involved.
     *   menu opens, Mod tab = vanilla page -> the ctor is harmless, and the
     *   and the checkbox still works          killer is in what we do with the
     *                                        page afterwards (append + switch).
     *                                        The page route reopens with a much
     *                                        narrower question.
     *   intermittent                      -> not an exception, a GC-timing
     *                                        problem; the ~60 allocations inside
     *                                        the ctor are the place to look.
     *
     * s_tabState is the marker that the probe was REACHED: the probe runs
     * immediately before `s_tabState = 1`, so a reading of `1` on row 1 proves
     * the ctor returned.  A null return reports 26 instead. */
    constexpr bool kModProbe = false;
    /* ---- v227: LET THE TAB ACTUALLY RESOLVE TO 10 ----
     *
     * v226 answered its question on hardware: row 1 read `01 01 23 00`, i.e.
     * s_tabState = 1 -- the ctor ran, returned a valid object, and nothing
     * froze.  THE CONSTRUCTOR IS NOT THE KILLER.  That whole line of reasoning
     * is closed.
     *
     * What the same test also exposed is the thing v222 has been hiding all
     * along.  GameMenu.changeTab does NOT take the tab index as the new
     * currentTab.  It reads the tab's NAME and looks the number up
     * (0x1796D60):
     *
     *     ldr x0,[x19,#0x80]      ; tabs
     *     bl  0x170e6c0           ; tabs.get_Item(index)
     *     ldr x1,[x0,#0x20]       ; tab.name
     *     bl  0x1797e00           ; getTabNumberFromName(name)
     *     str w0,[x19,#0x68]      ; currentTab = that number
     *
     * Our HookTabNumFromName returns kVanillaOptsTab (8) while kModPage is
     * false, so in v222/v225/v226 pressing onto the Mod tab sets currentTab to
     * 8: the game shows the Options page and the tab strip snaps back.  That
     * is the "auto jump to the settings page" the user saw in v226 -- it is by
     * design, not a fault.
     *
     * The consequence is that NO WORKING BUILD HAS EVER HAD currentTab == 10.
     * Every build that froze (v217-v224) is also every build where the tab
     * resolved to 10, so "our page object is broken" and "index 10 as the
     * current tab is broken" have never been separated -- the same mistake as
     * the ctor, one layer up.
     *
     * kTabToTen separates them, with no page of ours anywhere: pages[10] stays
     * the game's own Options page (kDupPage), the only change is that the hook
     * now answers 10 instead of 8.  So the object at index 10 is a page the
     * GAME built and fully initialised.
     *
     *   freezes -> index 10 being current is the killer, and it has nothing to
     *              do with our page.  The dedicated page is then unreachable by
     *              this route and has to come from a draw hook.
     *   works   -> index 10 is fine, the tab strip stays on Mod, and the freeze
     *              is specific to the page object after all. */
    constexpr bool kTabToTen = true;
    /* ---- v228: shrink the game's own page instead of building one ----
     *
     * v227 proved that currentTab == 10 is harmless when pages[10] is a page
     * the game built.  v226 proved calling OptionsPage..ctor is harmless.  So
     * the only thing left that separates a freezing build from a working one
     * is the PAGE OBJECT ITSELF -- and that splits into two halves that have
     * still never been tested apart:
     *
     *   (a) the object was created by us, outside GameMenu..ctor
     *   (b) its `options` list has 1 entry where the game's has 41
     *
     * kTruncOnMod tests (b) on its own, with no object of ours anywhere: while
     * the Mod tab is current, the game's own page is made to report ONE option
     * by writing its list's `_size` (`+0x18`), then the old count is put back
     * as soon as the tab is left.  Nothing is constructed, nothing is
     * allocated, no managed method is called -- two integer writes.
     *
     * What the player sees on the Mod tab: the page with a single entry, which
     * is the FIRST vanilla option rather than ours.  That is expected; this
     * build is a question, not the feature.  If it survives, swapping that one
     * array slot for our checkbox is the next (small) step.
     *
     *   freezes -> a short `options` list kills the page even when the page is
     *              the game's own, so (b) is the killer and the dedicated page
     *              is a list problem, not an object problem.
     *   works   -> (b) is innocent, which leaves (a): the object being ours is
     *              what kills it, and the dedicated page has to come from a
     *              draw hook instead. */
    constexpr bool kTruncOnMod = true;
    /* ---- v229: and put OUR checkbox in the slot that survives ----
     *
     * v228 answered its question on hardware: the Mod tab showed one row (the
     * first vanilla option, "概述："), the Options tab came back with all 41 and
     * scrolled normally, and going back to Mod showed one row again.  So (b) is
     * innocent -- a one-entry `options` list does not kill the page, even when
     * the page is the game's own.  That leaves (a), the object being ours.
     *
     * But there is no need to chase (a) any more, because every ingredient of
     * the feature has now been proven separately:
     *
     *   - the game's page can be truncated to one row and restored   (v228)
     *   - our OptionsCheckbox can be drawn and toggled by this very page,
     *     because v211 appends it to this page's list and the player has been
     *     ticking it since v211                                       (v227)
     *
     * So the Mod page can be built out of the game's page plus one pointer
     * swap, with no object of ours anywhere near the page itself.
     *
     * The swap is a SWAP, not an overwrite: `array[0] <-> array[ourIndex]`.
     * Both elements therefore stay referenced by the managed array across the
     * write, and a GC between the two builds cannot collect either one.  An
     * overwrite would leave the displaced vanilla element referenced only by a
     * native static, and a C++ static is not a GC root -- that is precisely
     * the mistake that produced nine freezing builds.
     *
     * The player sees, on the Mod tab, a single row: our "动画加速2倍" checkbox.
     * The Options tab is untouched: same 41 vanilla rows plus the checkbox at
     * the end, exactly as in v227.
     *
     * Set this to false to get v228's behaviour back (one vanilla row). */
    constexpr bool kOurElemOnMod = true;
    constexpr uintptr_t kGameMenuUpdateOff = SDV_OFF(kGameMenuUpdateOff, 0x17981F0u);
    /* v219: there is deliberately NO hook on GameMenu.draw 0x17984B0.
     *
     * v218 hooked it and called DrawString at its tail to paint our tab's
     * label.  On hardware that froze the game the moment our tab became the
     * SELECTED tab, i.e. as soon as the mod page opened.  Two things were
     * wrong with the reasoning, and both are worth keeping written down:
     *
     *   - The premise was false.  GameMenu.draw picks the ICON by name and
     *     skips it when nothing matches, but the tab's NAME TEXT is drawn
     *     regardless -- the user confirmed v217 already showed "Mod".  So
     *     there was nothing to paint; only the little picture was missing.
     *   - "The batch is still open when GameMenu.draw returns, so drawing
     *     here is free" was wrong in the direction that matters.  An open
     *     batch belongs to whoever will End it, and pushing our sprites into
     *     it changes what their End does.  It survives the idle tab strip and
     *     dies on the selected-tab path.
     *
     * The lesson generalises: a batch we did not Begin is not ours to draw
     * into, no matter what state flag it reports.  If a tab icon is ever
     * wanted, it has to be drawn from a hook that owns its own Begin/End
     * pair, outside this function entirely. */
    constexpr uintptr_t kPageCtorOff = SDV_OFF(kPageCtorOff, 0x17F8C80u);
    constexpr uintptr_t kTabCtorOff = SDV_OFF(kTabCtorOff, 0x175A0D0u);
    constexpr uintptr_t kCreateStringOff = SDV_OFF(kCreateStringOff, 0x3A0100u);
    /* v216: the proven string-building pair, copied from the checkbox path
     * (HookDrawHud's append block) rather than re-derived. */
    constexpr uintptr_t kPadLeftOff = SDV_OFF(kPadLeftOff, 0x39AF40u);
    constexpr uintptr_t kSubstrOff = SDV_OFF(kSubstrOff, 0x395F40u);
    constexpr uintptr_t kTabNumFromNameOff = SDV_OFF(kTabNumFromNameOff, 0x1797E00u);
    constexpr uintptr_t kTabLabelFromIdxOff = SDV_OFF(kTabLabelFromIdxOff, 0x1797FB0u);
    /* v232: String.Equals(a, b) 0x3943F0.  `b 0x394280`, which compares the
     * two references and then the length and characters; returns bool in w0.
     * Hooked only so the tab icon lookup matches our tab -- see
     * HookStringEquals. */
    /* ---- v237: a 48-slot backpack ----
     *
     * InventoryPage.draw iterates the `inventory` list, so 48 slots in the list
     * means 48 slots drawn -- the drawing needs nothing from us.  What is
     * hardcoded is the slot COUNT (36) and the Y offset of the equipment icons
     * (256), both as immediate constants:
     *
     *   0x17A7DF4  mov  w8, #0x24       the layout default (rows = count/12,
     *                                   and the page height follows it)
     *   0x17A8114  cmp  w0, #0x24       the slot-building loop bound
     *   0x17AD730  add  w2, w8, #0x100  equipment icons' Y, four sites
     *   0x17AD8B4  add  w1, w8, #0x100
     *   0x17AD93C  add  w1, w8, #0x100
     *   0x17AD9BC  add  w1, w8, #0x100
     *
     * Each patch verifies the ORIGINAL word before writing, so on a different
     * build every site simply refuses and nothing is touched.
     *
     * The growth itself is the game's own routine: 0x13C3E50(player, n) reads
     * maxItems (+0x570 NetInt, value at +0x68), sets it to old+n through the
     * NetInt's setter, then pads the items list (+0x1E0) with nulls until
     * Count == maxItems. */
    constexpr bool kBiggerBackpack = true;
    constexpr uintptr_t kGrowInventoryOff = SDV_OFF(kGrowInventoryOff, 0x13C3E50u);
    constexpr unsigned kMaxItemsOff = SDV_OFF(kMaxItemsOff, 0x570u);
    constexpr int kVanillaMaxItems = 36;
    constexpr int kBigMaxItems = 48;
    constexpr uintptr_t kStrEqualsOff = SDV_OFF(kStrEqualsOff, 0x3943F0u);
    /* v235: the address range inside GameMenu.draw that compares tab names
     * against the game's own, i.e. the icon lookup.  HookStringEquals only
     * answers "equal" when it was called from in here. */
    constexpr uintptr_t kIconLookupLo = SDV_OFF(kIconLookupLo, 0x1798790u);
    constexpr uintptr_t kIconLookupHi = SDV_OFF(kIconLookupHi, 0x1798A40u);
    constexpr uintptr_t kNumberOfTabsOff = SDV_OFF(kNumberOfTabsOff, 0xC95E840u);
    constexpr unsigned kGmTabsOff = SDV_OFF(kGmTabsOff, 0x80u);
    constexpr unsigned kGmPagesOff = SDV_OFF(kGmPagesOff, 0x88u);
    /* v220: `GameMenu.update 0x17981F0` is three loads and a tail call:
     *     ldr x0,[x20,#0x88]   ; pages
     *     ldr w1,[x20,#0x68]   ; currentTab   <-- this
     *     bl  0x13C0890        ; pages.get_Item(currentTab)
     * so +0x68 is currentTab, and get_Item is the one call in this path that
     * throws when the index is past the end of the list. */
    constexpr unsigned kGmCurrentTabOff = SDV_OFF(kGmCurrentTabOff, 0x68u);
    /* OptionsPage's two static scroll fields.  They are shared by every
     * instance, so a value left behind by the vanilla page (which has 41
     * entries) is out of range for our page (which has 1) -- docs/
     * OPTIONS_PERSIST_PLAN.md §9.2 already called this the one residue to
     * clear when taking the page over, and until v220 it never was. */
    constexpr uintptr_t kLastSelectedIndexOff = SDV_OFF(kLastSelectedIndexOff, 0xC95EB64u);
    constexpr uintptr_t kLastCurrentItemIndexOff = SDV_OFF(kLastCurrentItemIndexOff, 0xC95EB68u);
    constexpr unsigned kPageCurrentItemIndexOff = SDV_OFF(kPageCurrentItemIndexOff, 0x78u);
    /* OptionsPage.options, from docs/offsets/meta_optionspage.json.  List<T>
     * layout, proven by AppendToList: `_size` at +0x18, `_version` at +0x1C. */
    constexpr unsigned kPageOptionsOff = SDV_OFF(kPageOptionsOff, 0xA0u);
    constexpr unsigned kListSizeOff = SDV_OFF(kListSizeOff, 0x18u);
    constexpr int kModTabIndex = 10;
    /* Index of the vanilla Options page/tab, from GameMenu..ctor's add order
     * (inventory 0 .. options 8, exit 9).  Used as the destination while the
     * tab-only bisect stage has no page of its own. */
    constexpr int kVanillaOptsTab = 8;
    constexpr uintptr_t kReadSettingsOff = SDV_OFF(kReadSettingsOff, 0x1ADFC30u);
    constexpr uintptr_t kSetOptionsOff = SDV_OFF(kSetOptionsOff, 0x1443540u);
    /* StartupPreferences layout, from .tmp/meta_startuppreferences.json
     * (size 0x70).  Every offset below is a declared field, not a guess:
     *
     *   0x10..0x16  7 bools   startMuted, levelTen*, skipWindowPreparation,
     *                        sawAdvancedCharacterCreationIndicator
     *   0x18 timesPlayed     0x1c windowMode   0x20 displayIndex
     *   0x24 gamepadMode     0x28 playerLimit
     *   0x38 lastEnteredIP   0x40 safeRegionSize
     *   0x48 languageCode (string)
     *   0x50 clientOptions (Options) <-- the member the serializer writes
     *   0x58 isLoaded  0x59 _isBusy  0x5a _pendingApplyLanguage
     *   0x60 _task     0x68 _resetViewportAfterLoadSettings
     *
     * These are what LooksLikeStartupPrefs fingerprints. */
    constexpr unsigned kPrefsLangOff = SDV_OFF(kPrefsLangOff, 0x48u);
    constexpr unsigned kPrefsOptsOff = SDV_OFF(kPrefsOptsOff, 0x50u);
    constexpr unsigned kPrefsSize = SDV_OFF(kPrefsSize, 0x70u);
    /* v204: the StartupPreferences constructor.  Called as `mov x0,xzr; bl`
     * -- x0 = 0 means "allocate it yourself", and the new object comes back in
     * x0 (same convention as OptionsCheckbox..ctor). */
    constexpr uintptr_t kPrefsCtorOff = SDV_OFF(kPrefsCtorOff, 0x1ADE8C0u);
    /* StartupPreferences..ctor factory 0x1ADE8C0 (no arguments, returns the
     * new instance; the game calls it as `mov x0,xzr; bl`).  Not used yet --
     * kept here because the fresh-instance recipe is the fallback the next
     * build needs if the captured pointer turns out to be dead. */
    /* The body that does the work, reached from savePreferences's Task:
     *
     *     _savePreferences(this):
     *         bl 0x19d26c0                 ; static init / instance lookup
     *         bl 0x2274f0                  ; path building
     *         bl 0x217430
     *         bl 0x1adf2f0                 ; writeSettings -> XmlSerializer
     *
     * No Monitor.Enter, no Task, no wait -- it is a straight line that returns
     * after the file is on disk.  The game itself runs it on the main thread at
     * boot (0x1819EAC tail-calls savePreferences(0,0), which spawns a task and
     * waits for exactly this), so the work is already proven safe on the render
     * thread's context.  We call it directly: one code path, deterministic, and
     * the caller knows the file was written when it returns.
     *
     * savePreferences (0x1ADEE90) is NOT used: its w1 = 0 path takes a Monitor
     * lock and then Task.Wait()s while holding it, which froze the console. */
    constexpr uintptr_t kSavePrefsImplOff = SDV_OFF(kSavePrefsImplOff, 0x1ADF190u);
    /* Static slot holding the Game1 instance (`*(base + 0xC95C228)`, one
     * dereference).  Used as a fallback source for the instance pointer:
     * drawHUD has no direct caller, so it is reached through a vtable slot and
     * its x0 is only *presumably* the Game1 instance.  Validating whatever we
     * get means a wrong x0 costs us nothing worse than falling through to this.
     */
    constexpr uintptr_t kGame1SlotOff = SDV_OFF(kGame1SlotOff, 0xC95C228u);
    /* Resolved every HUD frame from `*(Game1 + 0x90)`, not from the ctor.
     *
     * Game1.drawHUD is an instance method, so its x0 IS the Game1 instance and
     * instanceOptions is a plain instance field -- one dereference, no static
     * slot, no ctor hook.  The ctor route is a dead end anyway: `.ctor` returns
     * void and allocates internally, so the new object is never visible to us.
     *
     * Kept as a cache and re-validated every frame: the first HUD draws happen
     * before a save is loaded, when the field can still be null, and opening a
     * save may replace the object. */
    static void* g_options = nullptr;          /* live Options, from Game1+0x90 */
    static int g_hudFrames = 0;                /* HUD draws since boot */
    static unsigned char g_bootLatched = 0;    /* s_optBoot taken from +0x24 */
    static unsigned char g_wroteFlag = 0;      /* +0x24 differs from its boot value */
    /* Diagnostic snapshots for the on-screen readout.  HookDrawHud runs every
     * frame but the hex rows are drawn from the map hook, so the values have to
     * travel through file-scope statics.
     *
     * s_optBoot is the number that answers the only open question left.  It is
     * latched on the first frame Options is readable, i.e. BEFORE the player can
     * possibly click anything, so after a restart it reports what came out of
     * startup_preferences rather than what we just put in. */
    static unsigned s_optFlags = 0;   /* bit0 resolved, bit1 looks-like,
                                       * bit2 element appended, bit3 +0x24 moved */
    static unsigned s_optBit = 0;      /* Options+0x24 right now */
    static unsigned s_optBoot = 0;     /* Options+0x24 as found at boot */
    static unsigned s_optDirty = 0;    /* Options+0x158, sticky */
    static unsigned s_elemWhich = 0;   /* element +0x28, must read 0x23 */
    static unsigned s_elemChecked = 0; /* element +0x51, must track +0x24 */
    /* The mirror-slot fan-out.  g_desired is the single source of truth: the
     * game's own click handler writes +0x24, we notice the change and copy it
     * to the two mirror slots; at boot we recover from whichever mirror
     * survived and write it back into +0x24 so the checkbox shows it.
     *
     * Without g_desired the OR of the three fields would fight itself: turning
     * the box off clears +0x24 while a surviving mirror still reads 1, so the
     * recovery branch would immediately switch it back on. */
    static unsigned char g_desired = 0;
    static unsigned char g_desiredInit = 0;
    static unsigned char g_saveWanted = 0;   /* ask for one save, on next frame */

    /* v346: last-action trace, persisted through the game's own preferences
     * writer.
     *
     * There is no log file: sdlog cannot open an fsp-srv session in this
     * process, and the on-screen readout dies with the crash.  But the game
     * itself CAN write -- savePreferences() returns success (v205 probe) and
     * the switch bitmap really does survive a restart -- so the trace rides in
     * StartupPreferences.timesPlayed (+0x18, an int the game only uses as a
     * play counter) and is written by the same async save the toggles use.
     *
     * Encoding: 0xA5 | step | sprite index, so a value that is not ours is
     * obvious at a glance. */
    static unsigned s_trace = 0;
    static unsigned s_tracePrev = 0;   /* what the file said at boot */
    static unsigned char s_traceDirty = 0;
    static unsigned char s_traceHot = 0;   /* an action happened: flush soon */
    static unsigned s_traceFrame = 0;
    static unsigned s_traceSeq = 0;
    static unsigned char s_traceBaseline = 0;  /* one idle write per session */
    constexpr unsigned kTraceMagic = 0xA5000000u;
    static void* g_startupPrefs = nullptr;   /* captured by the loadPrefs hook */
    /* v203: the instance the save was ACTUALLY issued on, so the busy watch and
     * the _isBusy release do not depend on which candidate won. */
    static void* g_savePrefs = nullptr;
    /* v203: the Game1+0x150 candidate and its autopsy, refreshed every frame
     * from the update hook so the readout can report it without a cross-scope
     * forward declaration. */
    /* v204: the last VALID snapshot of the captured instance, plus how many
     * frames it stayed valid.  The captured wrapper dies at some point after
     * boot (v202 proved it), so the scalars it holds -- windowMode,
     * displayIndex, playerLimit, timesPlayed, fullscreenResolution -- are only
     * available for a short window.  Copy them while they are still readable,
     * so the instance we build ourselves does not reset the player's display
     * settings to constructor defaults. */
    static unsigned char s_prefsSnap[kPrefsSize];
    static unsigned s_fpFailCap = 0xFu;  /* autopsy of the captured candidate */
    static unsigned s_snapOk = 0;
    static unsigned s_snapFrames = 0;
    static void* g_ourPrefs = nullptr;   /* the instance we constructed */
    /* The value that has to end up in the FILE, and the Options instance the
     * serializer actually reads.
     *
     * v195 wrote +0x24 on the live Options (Game1+0x90) and then asked the game
     * to serialise StartupPreferences.  That was the wrong object.  The game's
     * serializer writes the preferences instance's OWN clientOptions member
     * (+0x50), and that member is a DIFFERENT Options instance from Game1's:
     *
     *   0x1ADE938  StartupPreferences..ctor  -> clientOptions = new Options()
     *   0x178ADC0  Game1.set_options(prefs+0x50)   (branch A of the boot path)
     *   0x181A1AC  prefs+0x50 = Game1.get_options()  (the boot save chain)
     *
     * so at runtime the instance we captured has its own copy, and the v195
     * readout confirmed it: the "prefs+0x50 == live Options" bit came out 0.
     * Writing the live copy therefore never reached startup_preferences, which
     * is exactly the symptom we saw.
     *
     * The value now goes into BOTH copies, and the save is issued against the
     * instance whose +0x50 the serializer reads. */
    static unsigned char g_savedFlag = 0;    /* the value to persist */
    static void* g_serializedOptions = nullptr;  /* prefs+0x50, resolved at save */
    static unsigned s_serBit = 0;            /* +0x24 of the serialised copy */
    /* The queued save task, so its outcome can be watched.
     *
     * savePreferences stores the Task it started at +0x60 and, in the waiting
     * path only, clears it again.  We do NOT wait (v196 froze the console that
     * way: the main thread sat in Task.Wait() and never came back, music and
     * all), so the task is left running on the thread pool and we watch it
     * instead.  Task's state lives at +0x48 and Task.get_IsCompleted is
     * literally "state > 4" (0x3CA090: ldr w8,[x0,#0x48]; cmp w8,#4; cset).
     *
     * TaskStatus, read off the accessors rather than remembered -- v197's note
     * had 6 and 7 the wrong way round and that turned "the write threw" into
     * "the task was cancelled", which sent the whole post-mortem down the wrong
     * branch:
     *
     *   get_IsCanceled 0x3CA150: ldr w8,[x0,#0x48] ; cmp w8,#6 ; cset eq
     *   get_IsFaulted  0x3CA160: ldr w8,[x0,#0x48] ; cmp w8,#7 ; cset eq
     *   get_IsCompleted0x3CA090: ldr w8,[x0,#0x48] ; cmp w8,#4 ; cset gt
     *   get_Status     0x3CA170: ldr w0,[x0,#0x48]
     *
     * so: 0 Created, 1 WaitingForActivation, 2 WaitingToRun, 3 Running,
     * 4 WaitingForChildrenToComplete, 5 RanToCompletion, 6 Canceled,
     * 7 Faulted.  v197 reported 7, i.e. _savePreferences THREW. */
    static void* g_saveTask = nullptr;        /* the Task savePreferences queued */
    static int s_taskState = -1;              /* its status, -1 = none */
    static unsigned s_saveCalls = 0;         /* savePreferences calls we made */
    static unsigned s_saveDbg = 0;           /* why the call did or did not run */
    static unsigned s_busyVal = 0;           /* StartupPreferences+0x59 as read */
    static unsigned s_busySeen = 0;          /* bit0 armed, bit1 saw busy, bit2 saw it clear */
    /* v199 write-path probe.  See s_origSavePrefsImpl for why these exist.
     *
     * s_wrEntered / s_wrWrite are CUMULATIVE (clamped at 15) so they double as
     * proof that the hooks are installed: the game runs the same path at boot.
     * s_wrBack / s_wrWriteBack are cleared immediately before our own call, so
     * "entered but not back" means the function threw. */
    static unsigned s_wrEntered = 0;     /* _savePreferences entries */
    static unsigned s_wrWrite = 0;       /* writeSettings entries */
    static unsigned s_wrEntered0 = 0;    /* the above, snapshotted before our call */
    static unsigned s_wrWrite0 = 0;
    static unsigned s_wrBack = 0;        /* _savePreferences returned, this attempt */
    static unsigned s_wrWriteBack = 0;   /* writeSettings returned, this attempt */
    /* CUMULATIVE returns, and this is the whole point of v201.
     *
     * The game runs this exact path at boot (0x181A1C4 -> savePreferences(prefs,0,0)
     * -> Task -> _savePreferences).  So right after a restart, with the player
     * having touched nothing, these two flags answer the question every version
     * since v191 has been dancing around:
     *
     *   entered >= 1 and backAll == 0  ->  the GAME's own save throws too.
     *                                      The file channel is dead in this
     *                                      process and NO amount of fixing our
     *                                      call will ever write the file.
     *   entered >= 1 and backAll == 1  ->  writing works.  Our failure is about
     *                                      the instance or the moment, not the
     *                                      capability.
     *
     * That single bit invalidates or validates the entire approach, and it
     * costs the player nothing but a boot. */
    static unsigned s_wrBackAll = 0;
    static unsigned s_wrWriteBackAll = 0;

    /* v202.  "armed" is latched the first time we actually issue a save.
     *
     * Without it the four "this attempt" bits in row 4 byte 3 are lies before
     * the player's first click: the arming snapshot is still 0, so the game's
     * own boot save makes s_wrEntered(1) > s_wrEntered0(0) true and all four
     * light up as if WE had run.  v201's first hardware readout (3F101000)
     * was exactly that, and it would have been read as "our save succeeded".
     * Bits 0..3 are now forced to 0 until this flag is set, and bit 6 reports
     * it, so "never attempted" (30) can never be mistaken for "succeeded" (7F). */
    static unsigned s_wrArmed = 0;

    /* v202.  Which of LooksLikeStartupPrefs' checks rejected the instance.
     *
     * 0 = null / not Plausible / not RangeReadable
     * 1 = method table outside the module
     * 2 = one of the seven bools at 0x10..0x16 is not 0/1
     * 3 = one of the three bools at 0x58..0x5a is not 0/1
     * 4 = timesPlayed (+0x18) out of 0..1000000
     * 5 = windowMode (+0x1c) out of 0..2
     * 6 = displayIndex (+0x20) out of -1..16
     * 7 = playerLimit (+0x28) out of -1..32
     * 8 = languageCode (+0x48) is not a module-local printable string
     * F = all checks passed
     *
     * v201 reported only "passed / not passed", which cannot distinguish a
     * dangling pointer from a check that is simply too strict for a live
     * object -- and those two demand opposite next moves. */
    static unsigned s_fpFail = 0xFu;
    /* v203: the same autopsy, but always for the instance found at
     * Game1+0x150.  Row `E` reports THIS one, so the reading always answers
     * the question the current version is actually asking. */
    /* Which instance the save was issued on:
     *   1 = Game1+0x150 (the one the game itself keeps)
     *   2 = the pointer captured from loadPreferences
     *   3 = neither passed the fingerprint, nothing was issued */
    static unsigned s_prefsSrc = 0;
    static unsigned s_bootUiBits = 0;  /* +0x64 at the exact latch frame */
    static unsigned s_desired = 0;
    /* OptionsCheckbox..ctor 0x17F6780 signature, read off the prologue:
     *
     *     w19 = w2, w21 = w3, w20 = w4, x22 = x1, x23 = x0(this)
     *     ... then it calls the base ctor 0x17f7c00 as
     *         (this, label, x=w21, y=w20, w=0x24, h=0x24, whichOption=w19)
     *
     * so the DECLARED order is (label, whichOption, x, y) -- whichOption is the
     * THIRD argument, not the second.  Getting that wrong is invisible: the
     * element still allocates and still draws, it just reads and writes the
     * wrong Options field.  The base ctor's own parameter order is
     * (label, x, y, width, height, whichOption) and lands whichOption in +0x28.
     *
     * x0 == 0 means "use the static type pointer" (cbz x0 -> class-init path),
     * and the object comes back in x0, so the game itself calls it as
     * (0, label, whichOption, x, y).  -1 for x/y means "use defaultX/defaultY". */
    /* Stage 166: the live OptionsPage, captured from its draw hook.  Its
     * `options` list is at +0xa0 (List<OptionsElement>); appending is only
     * attempted when the backing array still has spare capacity, so nothing is
     * ever reallocated from native code. */
    static void* g_optionsPage = nullptr;
    static void* g_appendedTo = nullptr;
    static unsigned g_appendState = 0;   /* 1 ok / 2 no capacity / 3 bad list */
    static unsigned g_appendCount = 0;
    void (*s_origOptionsDraw)(void*, void*) = nullptr;
    void (*s_origLoadPrefs)(void*, int, int, int) = nullptr;
    /* v199 probes on the write path itself.
     *
     * v198 proved the clientOptions repair worked -- the readout came back
     * 0x6F, i.e. bit7 = 0 (it WAS null) + bit2 = 1 (now the live Options) +
     * bit3 = 1 (written) -- and the task STILL reported status 7 = Faulted.
     * So the exception is not caused by a null clientOptions and the only way
     * to find it is to watch the write from the inside.
     *
     * _savePreferences 0x1ADF190 is the whole write, start to finish:
     *
     *     static-init check
     *     File.Create-ish  (0x2274F0)
     *     writer ctor      (0x217430)
     *     writeSettings    (0x1ADF2F0)  -> XmlSerializer.Serialize
     *     dispose, unlock
     *
     * so bracketing these two functions localises the throw to one of three
     * places, and the four bits in row 4 tell us which.  The counts are
     * cumulative on purpose: at boot the GAME calls this same path, so a
     * non-zero count right after a restart is proof the hook is live even
     * before the player touches anything. */
    void (*s_origSavePrefsImpl)(void*) = nullptr;
    void (*s_origWriteSettings)(void*, void*) = nullptr;
    void (*s_origShippingUpdate)(void*, void*) = nullptr;
    void (*s_origGameMenuUpdate)(void*, void*) = nullptr;
    int (*s_origTabNumFromName)(void*, void*) = nullptr;
    void* (*s_origTabLabelFromIdx)(int) = nullptr;
    /* v232: String.Equals 0x3943F0, hooked so the icon lookup matches our tab
     * without our tab carrying a game tab's name. */
    int (*s_origStrEquals)(void*, void*) = nullptr;

    /* v212 state.  0 = not attempted yet, 1 = tab installed, >=10 = the stage
     * that refused.  Reported in row 1 so a failure names itself instead of
     * just "the tab is missing". */
    static volatile unsigned s_tabState = 0;
    static void* s_modTabName = nullptr;   /* managed: the tab's NAME (identity key) */
    /* v232: the tab's LABEL ("Mod") -- what the game draws under the tab and
     * what the hover text shows.  It is a different field (+0x28) from the
     * name (+0x20), which is what the icon lookup reads, so the two can differ:
     * the label stays "Mod" while the name is built to make the lookup match. */
    static void* s_modTabLabel = nullptr;
    /* The game tab whose icon we borrow.  We do NOT copy its name -- that is
     * what froze the game in v231 -- we only point at the object so
     * HookStringEquals can recognise the one comparison that matters. */
    static void* s_modIconTarget = nullptr;
    static void* s_modTab = nullptr;       /* the tab component we appended */
    /* v217: set only after our page is really in `pages`.  The tab's name maps
     * to kModTabIndex only while this is 1; otherwise it maps to the vanilla
     * Options page, so a half-finished build degrades instead of pointing the
     * menu at an index that does not exist. */
    static volatile unsigned s_pageReady = 0;
    static void* s_modElemLabel = nullptr; /* managed "动画加速2倍" */
    static void* s_modElem2Label = nullptr; /* managed "自动打怪" (v303) */
    /* v220: our page object itself, so the per-frame guard can reach its
     * currentItemIndex without going through `pages`. */
    static void* s_modPage = nullptr;
    /* v228: the game's own Options page -- the object that `pages[10]` points
     * at while kDupPage is on.  It is rooted by `pages`, so keeping the
     * pointer here is safe for as long as the menu lives; the re-arm block
     * drops it with the rest. */
    static void* s_vanillaPage = nullptr;
    /* v228: how many entries the page's `options` list had before we shrank
     * it, or -1 when it is not currently shrunk.  Restoring the exact number
     * (rather than 41) matters because the v211 checkbox append already added
     * one, and the count is whatever the game built that time. */
    static int s_savedOptCount = -1;
    void (*s_origReadSettings)(void*, void*) = nullptr;
    void (*s_origSetOptions)(void*) = nullptr;

    /* v206.  The three probes below all answer one question each, and the
     * combination is what decides the next move.  0xF means "never observed",
     * so "the hook did not fire" can never be mistaken for "the value was 0".
     *
     *   s_fileBit       +0x24 of the Options readSettings deserialised out of
     *                   the file.  This is what the file said.
     *   s_appliedBit    +0x24 of the Options handed to Game1.set_options.
     *                   This is what the game installed.
     * The pipeline reads as:
     *   F/F/F  no hook fired at all -> the build is wrong, not the logic
     *   0/0/0  the file said false -> our write never landed in the file
     *   1/1/1  the round trip works -> the loss is in how our injected
     *          checkbox reads the value back
     *   1/1/0  the game applied it and then the live object lost it
     *
     * v208: a third probe ("what the game's own save serialised") was removed.
     * It had to be sampled inside writeSettings, which runs on a thread-pool
     * thread, and the only way to validate a pointer there was Plausible(),
     * whose page cache is unsynchronised and shared with the main thread.  The
     * live value at the first HUD frame answers the same question without the
     * race. */
    /* volatile: v206's first build proved these three get eliminated.
     * GCC dropped the store in HookReadSettings entirely -- the function ended
     * in a tail call to RangeReadable with the assignment gone -- because the
     * only reader is the diagnostics block, and cross-function value numbering
     * decided the store was dead.  The readout is drawn, so the reader is real;
     * volatile simply forbids the elimination and makes the probe observable. */
    static volatile unsigned s_fileBit = 0xFu;
    static volatile unsigned s_appliedBit = 0xFu;

    /* v209.  THE FIX, and it is the same idea the boot latch already had.
     *
     * v208's readout said the file holds +0x24 = 1 (s_fileBit = 1) but
     * set_options received 0 and the live object had 0.  So the write path is
     * fine and the value is lost at INSTALL.  The boot latch in
     * SyncFastFromOptions already tries to push the file's value into the live
     * Options, but it reads it from g_startupPrefs, and that wrapper is dead by
     * then (fingerprint check 2), so the push never happens.
     *
     * This does the push at the one moment the object is guaranteed live and
     * guaranteed to be the one about to be installed: inside set_options,
     * before the original stores it into Game1+0x90.
     *
     *   s_applyTarget  the value the file says we should have.  Latched from
     *                  s_fileBit as soon as readSettings has run, and updated
     *                  by the player's click, so an in-session change is not
     *                  undone by a later install.
     *   s_forced       how many times we actually had to write it.  0 means the
     *                  game installed the right value on its own.
     *
     * This is safe to do: set_options is a bare `str x0,[x8,#0x90]` and its
     * argument is always prefs+0x50 or new Options(), i.e. a real Options. */
    static volatile unsigned s_applyTarget = 0xFu;
    static volatile unsigned s_forced = 0;
    /* v209 diagnostics: the value seen at the first set_options call that
     * happened AFTER readSettings, i.e. the real install, plus how many times
     * set_options ran at all.  v208 could not tell "the boot install really got
     * a default Options" from "I latched an earlier unrelated call". */
    static volatile unsigned s_appliedAfterRead = 0xFu;
    static volatile unsigned s_setOptCalls = 0;

    /* v206.  Event order.  One shared counter, bumped by every hooked event,
     * with the value recorded the first time each one fires.  The ORDER of
     * "read the file" / "apply it" / "save it back" decides which fix is even
     * possible:
     *
     *   read < apply < save    the round trip is sound
     *   read < save  < apply   the boot save serialises the live Options
     *                          before they have been replaced, i.e. it writes
     *                          false over the file on every boot
     *   save < read            the same thing, one step earlier
     *
     * Without this the three bits above can only say "something is false",
     * never "who wrote the false". */
    static volatile unsigned s_evtSeq = 0;
    static volatile unsigned s_evtRead = 0xFFu;
    static volatile unsigned s_evtSetOpt = 0xFFu;
    static volatile unsigned s_evtWrite = 0xFFu;
    /* Stage 169: dialogue typewriter speed.
     *
     * DialogueBox.update (0x177BA70) is what advances the reveal --
     * it writes characterAdvanceTimer (str w8,[x19,#0xc0] at 0x177BF10) and
     * characterIndexInDialogue (str w0,[x19,#0xc4] at 0x177C13C).  Calling it
     * one extra time per frame therefore types twice as fast, the same lever
     * the player-animation hook uses.
     *
     * Input is NOT handled here (that is receiveKeyPress / receiveLeftClick),
     * so a second update only advances timers. */
    void (*s_origDialogueUpdate)(void*, void*) = nullptr;
    /* Game1.eventUp (0xC95BDC4, 46 references in the text) is true while an
     * event / cutscene is playing.  Double-updating the dialogue there froze the
     * game, and the spec wants cutscenes at normal speed anyway, so every extra
     * advance is skipped while it is set. */
    constexpr uintptr_t kEventUpOff = SDV_OFF(kEventUpOff, 0xC95BDC4);
    bool (*s_origCheckSingle)(void*, void*) = nullptr;

    constexpr float kMarkerOffX = 0.0f;
    constexpr float kMarkerOffY = 0.0f;

    /* Avatar footprint on screen.
     *
     * History, all of it hardware feedback:
     *   v95/v96  40x37  -- never complained about; the "over water" problem was
     *                      really the +32 placement bug (fixed in v96).
     *   v97      24x22  -- shrank it for the wrong reason; too small to read.
     *   v98      34x31  -- still read as a bit small.
     *   v99      40x37  -- back to the original size, which is where it stays.
     *
     * Offline render at in-game scale (.tmp/size_options*.png, no build
     * involved) -- overlapping pairs among the 37 known NPC positions:
     *
     *     24x22   34 pairs
     *     34x31   64 pairs
     *     38x35   75 pairs
     *     40x37   76 pairs
     *     44x41   80 pairs
     *
     * The source window stays 16x15 (see npc_crop_table.hpp); only the
     * destination rectangle changes, and drawing stays centred -- so changing
     * these two numbers never needs a placement re-calibration. */
    constexpr int kAvatarW = 40;
    constexpr int kAvatarH = 37;

    /* Head fan-out, for when two villagers stand on the same tile.
     *
     * Drawing a stack of identical heads hides all but the top one, so
     * overlapping markers get pushed apart.  Two knobs bound how far that is
     * allowed to lie:
     *
     *   kAvatarSpread    target minimum centre-to-centre distance.  18 is a
     *                    bit under half an avatar: enough that two heads are
     *                    individually readable, small enough that a head still
     *                    sits where its owner is.
     *   kAvatarMaxDrift  hard cap on how far a head may end up from its true
     *                    tile.  One avatar width would already be a quarter of
     *                    the town; half of it is the most that reads as "same
     *                    place".
     *
     * Chosen offline (.tmp/fanout_relax.py renders before/after at in-game
     * scale) over the 37 known positions: 76 overlapping pairs before, and
     * the worst exact stack drops 2 -> 1 while only 23 heads move at all.
     * Raising kAvatarSpread to 30 halves the pair count but drifts heads by
     * the full cap, which is why it is not the default. */
    constexpr float kAvatarSpread = 18.0f;
    constexpr float kAvatarMaxDrift = 20.0f;
    constexpr int kSpreadIters = 20;

    /* Birthday badge, top-right of the head.
     *
     * Source rect comes from NPC Map Locations, which draws
     * Game1.mouseCursors (147, 412, 10, 11) -- the red gift icon used on the
     * calendar.
     *
     * Scale history.  The mod draws it at 1.8x, but its portraits are 16px and
     * ours are 40x37, so on hardware 1.8 read as too small (v123) and 2.2 was
     * still too small (v124).  3.0x it is.
     *
     * At 3.0 the two badges together are wider than the head, so the gift has
     * to overhang further to stay clear of the "!" on the left:
     *   "!"    occupies dx+2  .. dx+17   (15 wide, kQuestBadgeInset 2)
     *   gift   occupies dx+18 .. dx+48   (30 wide, overhang 8)
     * That makes the badge pair slightly wider than the 40px face, which is
     * how the original mod reads too -- its icons are wider than its 16px
     * portrait.  .tmp/badge_size.png renders 2.2 / 2.6 / 3.0 / 3.4 on the same
     * town patch if this needs another pass. */
    constexpr int kBadgeSrcX = 147;
    constexpr int kBadgeSrcY = 412;
    constexpr int kBadgeSrcW = 10;
    constexpr int kBadgeSrcH = 11;
    constexpr int kBadgeW = 30;   /* kBadgeSrcW * 3.0, rounded */
    constexpr int kBadgeH = 33;   /* kBadgeSrcH * 3.0, rounded */
    constexpr int kBadgeOverhang = 8;  /* past the head's right edge */

    /* Quest badge -- the "!" the calendar uses for an outstanding errand.
     *
     * Same asset and scale as the gift icon: Game1.mouseCursors
     * (403, 496, 5, 14) at 3.0, i.e. 15x42.  NPC Map Locations puts it on the
     * right as well, but that collides with the gift icon when an NPC has both
     * a birthday and a quest, so this one sits on the left instead.  At 3.0 the
     * pair no longer fits inside the head (15 + 30 = 45 > 40), which is why the
     * gift's overhang went from 2 to 8. */
    constexpr int kQuestSrcX = 403;
    constexpr int kQuestSrcY = 496;
    constexpr int kQuestSrcW = 5;
    constexpr int kQuestSrcH = 14;
    constexpr int kQuestBadgeW = 15;   /* 5 * 3.0, rounded */
    constexpr int kQuestBadgeH = 42;   /* 14 * 3.0, rounded */
    constexpr int kQuestBadgeInset = 2;  /* in from the head's left edge */

    /* Quest object layout, from the AOT metadata (tools/aot_type.py).
     * Quest is 0xA8 bytes; the four subclasses that carry `target` put it at
     * three different offsets, so all three get probed at runtime. */
    constexpr uintptr_t kQuestLogOff = SDV_OFF(kQuestLogOff, 0x1B0);      /* Farmer.questLog */
    constexpr uintptr_t kQuestAcceptedOff = SDV_OFF(kQuestAcceptedOff, 0x30);
    /* Both are read now.  The PC mod's HasQuest is
     *     accepted && dailyQuest && !completed
     * and without them every accepted quest in the log -- finished ones and
     * story quests included -- contributed a target, which put badges on
     * villagers the player had no outstanding business with.  Each is a
     * NetBool: the wrapper pointer lives at the offset, the byte at +0x68.
     * `completed` at +0x38 is confirmed by the game's own hasVisibleQuest
     * loop (`ldr x8,[quest,#0x38]; ldrb w8,[x8,#0x68]`). */
    constexpr uintptr_t kQuestCompletedOff = SDV_OFF(kQuestCompletedOff, 0x38);
    constexpr uintptr_t kQuestDailyOff = SDV_OFF(kQuestDailyOff, 0x40);
    /* v116 tried `daysLeft` (+0x78, NetInt) as the daily/story test.  It read
     * 0 for every quest in the log, before *and* after accepting one, so it is
     * retired -- but the read was not the problem: the game's own
     * `Quest.GetDaysLeft` is literally
     *
     *     ldr x8, [x0, #0x78]      ; daysLeft wrapper
     *     ldr w0, [x8, #0x68]      ; int32 payload
     *     ret
     *
     * at 0x1a7a8e0, which confirms both the offset and the NetInt shape.
     * Whatever the field holds, it is 0 here. */
    constexpr uintptr_t kQuestDaysLeftOff = SDV_OFF(kQuestDaysLeftOff, 0x78);
    /* v117: `questType`, a NetInt at +0x70.  Same shape as `daysLeft`.
     *
     * This is the discriminator the reference mod actually uses -- it switches
     * on the subclass to decide where `target` lives -- and unlike the two
     * dead NetBools it is a field the game itself reads to dispatch. */
    constexpr uintptr_t kQuestTypeOff = SDV_OFF(kQuestTypeOff, 0x70);

    /* questType -> offset of the field that names a villager, or kNoTarget.
     *
     * The offsets come from the AOT metadata of every Quest subclass
     * (`.tmp/quest_types.json`); the *numbers* come from the subclasses' own
     * constructors, which end with `questType.Value = N`:
     *
     *     ldr x8, [x19, #0x70]      ; questType wrapper
     *     mov w1, #N
     *     ... ldr x10, [x9, #0x230] ; NetInt.set_Value
     *     blr x10
     *
     * Measured, not assumed:
     *
     *     ItemDeliveryQuest          0x1a70550   N = 3   -> target 0xB0
     *     SlayMonsterQuest           0x1a7cf20   N = 4   -> target 0xB8
     *     FishingQuest               0x1a6d294   N = 7   -> target 0xA8
     *     ResourceCollectionQuest    0x1a7ac20   N = 10  -> target 0xA8
     *     SocializeQuest             0x1a7f734   N = 5
     *     GoSomewhereQuest           0x1a702bc   N = 8
     *     ItemHarvest/LostItem/SecretLostItem    N = 9
     *
     * v117 shipped the declaration order of Quest's `type_*` statics (ItemDelivery
     * 2, Monster 3, Fishing 6, Resource 9) and every one of them was one too low,
     * so it read `ItemId` for an ItemDelivery quest and `targetMessage` for a
     * Monster quest.  The enum is not in declaration order; do not guess it
     * again -- these four numbers are what the constructors store.
     *
     * Everything not in the table is deliberately skipped.  Probing all three
     * offsets on every quest is what produced v115's "Coop" badge: that is
     * GoSomewhereQuest.whereToGo -- a *place* -- and GoSomewhereQuest is only
     * 0xB0 bytes, so probing +0xB0 read the next object on the heap. */
    constexpr int kNoQuestTarget = -1;
    constexpr int kQuestTypeCount = 11;
    constexpr int kQuestTargetByType[kQuestTypeCount] = {
        kNoQuestTarget,   /* 0  */
        kNoQuestTarget,   /* 1  */
        kNoQuestTarget,   /* 2  */
        0xB0,             /* 3  ItemDelivery  target       */
        0xB8,             /* 4  Monster       target       */
        kNoQuestTarget,   /* 5  Socialize     whoToGreet   */
        kNoQuestTarget,   /* 6  */
        0xA8,             /* 7  Fishing       target       */
        kNoQuestTarget,   /* 8  Location      whereToGo    */
        kNoQuestTarget,   /* 9  Harvest/LostItem           */
        0xA8,             /* 10 Resource      target       */
    };

    /* Newton square root.
     *
     * libm is not linked into the module and the spread loop is the only place
     * that needs a root, so carrying eight lines here beats pulling in a
     * dependency.  Inputs are squared distances between two points on a
     * 690-pixel-wide map, i.e. always under 1e5, where eight rounds converge
     * past float precision. */
    inline float SqrtF(float v) {
        if (v <= 0.0f) {
            return 0.0f;
        }
        float x = v;
        for (int k = 0; k < 8; k++) {
            x = 0.5f * (x + v / x);
        }
        return x;
    }

    /* Twelve unit vectors, 30 degrees apart.
     *
     * Only used for markers that landed on the *same* pixel, where there is no
     * direction to derive from the delta -- picking one from a table keeps the
     * result identical frame to frame instead of shimmering. */
    constexpr float kDirX[12] = {
        1.0f, 0.866f, 0.5f, 0.0f, -0.5f, -0.866f,
        -1.0f, -0.866f, -0.5f, 0.0f, 0.5f, 0.866f,
    };
    constexpr float kDirY[12] = {
        0.0f, 0.5f, 0.866f, 1.0f, 0.866f, 0.5f,
        0.0f, -0.5f, -0.866f, -1.0f, -0.866f, -0.5f,
    };

    /* Diagnostic rows over the map.
     *
     * Kept as a switch instead of deleting the calls: the only debug channel on
     * hardware is the 3x5 hex font, and re-adding it from scratch costs a build
     * round.
     *
     * Off in the public build, on locally.  SDV_RELEASE is set by
     * `make RELEASE=1` -- the same source, the same fingerprint probes, one
     * define apart.  Because it is constexpr, turning it off also compiles the
     * readouts (and the counters feeding them) out of the binary rather than
     * leaving dead draws behind. */
#ifdef SDV_RELEASE
    constexpr bool kShowDiagnostics = false;
#else
    constexpr bool kShowDiagnostics = true;
#endif
    /* v269 scythe diagnostic: a HIGH-WATER stage code, not a bitmask.
     * v268's bitmask read 00000000, which only said "not in the branch" --
     * it could not say WHERE it died, because AutoTractorUse has nine early
     * returns before the branch.  So each milestone now bumps a number and
     * never goes backwards:
     *
     *   0  never called
     *   1  player pointer bad
     *   2  not on our tractor
     *   3  CurrentTool is null (empty hand)
     *   4  usingTool still set (mid-swing)
     *   5  same tile as last frame (standing still)
     *   6  first sample after mounting
     *   7  tool became null
     *   8  FindDoFunction returned null
     *   9  current location null
     *  10  took the hoe/watering-can path
     *  11  took the SCYTHE path        <- what we want to see
     *  12  took the axe/pickaxe path
     *  13  terrainFeatures dictionary resolved
     *  14  TryGetValue found a TerrainFeature
     *  15  its vtable contained Grass.performToolAction
     *
     * s_scytheCalls counts issued Grass.performToolAction calls. */
    static unsigned s_scytheStage = 0;
    static unsigned s_scytheCalls = 0;
    /* v336 diagnostic: which gate refused the last qualifying chest hit
     * (Carry Chest).  One row on the world-map readout, eight nibbles:
     *   7 calls   how many times Chest.HandleChestHit reached our hook
     *   6         IsCarryableChest (130/232 + playerChest)
     *   5         args.ToolCanHit
     *   4         equipped tool's ItemId ends with "Pickaxe"
     *   3         chest is not empty
     *   2         addItemToInventoryBool returned true
     *   1         Farmer.getIndexOfInventoryItem found the chest
     *   0         objects.Remove removed it
     * A clean pickup reads FF111000.  Anything else names the gate that
     * gave up -- there is no log file on hardware, so this is the only way
     * to tell the gates apart.
     * v337 adds three more nibbles at the bottom for Automate Lite; see the
     * s_auto* block for those. */
    static unsigned s_carryCalls = 0;
    static unsigned s_carryState = 0;
    static unsigned s_carryAdd = 0;
    static unsigned s_carryInBag = 0;
    static unsigned s_carryRemoved = 0;
    /* v337: Automate Lite counters, saturating at 0xF.  Drawn in the three
     * spare nibbles of row 14 (see the s_carry* comment above for the rest):
     *   2 passes   how many scan passes have run
     *   1 output   machines whose product went into a chest
     *   0 input    machines that accepted an item from a chest
     * A pass counter that stays 0 means the tick hook never fired; a pass
     * counter that moves while output/input stay 0 means the scan found no
     * chest+machine pair. */
    static unsigned s_autoPasses = 0;
    static unsigned s_autoOutput = 0;
    static unsigned s_autoInput = 0;
    /* v340 row 15: what the last group actually looked like.
     *   regChests / regMachines   what the harvest produced this cycle
     *   grpChests / grpMachines   what one flood found
     *   grpNodes                  tiles that bridged the flood
     * A group of 1 machine while several are adjacent means the flood is
     * still not propagating; regMachines = 0 means the harvest hook is not
     * firing at all. */
    static unsigned s_autoGroups = 0;
    /* v343: file-store open attempts, drawn on the readout (rows 16/17). */
    static unsigned s_logTry = 0;
    static unsigned s_logFrame = 0;
    static unsigned s_dbgRegChests = 0;
    static unsigned s_dbgRegMachines = 0;
    static unsigned s_dbgGrpChests = 0;
    static unsigned s_dbgGrpMachines = 0;
    static unsigned s_dbgGrpNodes = 0;
    /* v359: wire state that the readout draws (row 14).  Declared here because
     * the draw code is far above the automation block. */
    static int   s_connCount = 0;
    static bool  s_connScan = false;
    /* v370: once a Flooring has been seen, its klass pointer is cached and every
     * later test is a single pointer compare -- no managed call, no allocation.
     * Same for Tree.  s_featureWrapped remembers whether the dictionary hands
     * back NetRef wrappers (1), bare features (2) or is still unknown (0), so
     * the unwrap stops calling Object.ToString() too. */
    static void* s_flooringKlass = nullptr;
    static void* s_treeKlass = nullptr;
    static unsigned s_featureWrapped = 0;
    /* Cumulative, so one reading tells the whole story:
     *   rebuilds = 0                -> the rebuild pass never ran
     *   lookups > 0, hits = 0       -> we asked the terrain dictionary and
     *                                  nothing there was a Flooring (so the
     *                                  "path" in that room is not tile data)
     *   hits > 0                    -> detection works; a grouping problem
     *                                  would then be the remaining suspect */
    static unsigned s_connRebuilds = 0;
    static unsigned s_connLookups = 0;
    static unsigned s_connHits = 0;
    /* Kept for future debugging, no longer drawn. */
    static unsigned s_connFound = 0;

    /* v362: an independent probe that does not involve the flood at all --
     * look up the tile the PLAYER is standing on.  0 = no terrain feature,
     * 1 = a feature that is not a Flooring, 3 = a Flooring.  Standing on the
     * wooden floor and reading 0 or 1 settles whether the floor is tile data
     * at all, without any of the grouping machinery in the way. */
    static unsigned s_probeState = 0;
    /* v364: which function the performToolAction slot (0x120) resolves to for
     * the feature under the player, as an offset from the module base, low 16
     * bits.  Flooring should read 5E30, the TerrainFeature base 3970.  This is
     * the number that tells us whether the slot assumption is right at all. */

    /* v270: v269 read stage 01, which means OnOurTractor was false for the
     * whole session -- but that cannot distinguish "the player was not riding
     * at all" from "Farmer.netMount did not hold OUR horse".  Two counters,
     * capped at 255, separate them:
     *   s_grassFound = tiles where terrainFeatures had an entry (mark 0x0E)
     *   s_grassCut   = tiles where Grass.performToolAction returned true
     * (the old mount counters both saturated at 0xFF once we knew we were on
     *  the tractor, so they were repurposed here.)
     * both 0 -> not riding anything; nonNull>0 && ours==0 -> riding something
     * that is not ours; ours>0 -> we were on the tractor. */
    static unsigned s_grassFound = 0;
    static unsigned s_grassCut = 0;

    /* v289 -- animal-page icon diagnostics.
     *
     * On retail hardware there is NO log file: svcConnectToNamedPort("sm:")
     * fails with OutOfResource inside the game process, so fs never comes up
     * and sdlog::Line writes nowhere (see the note in commit 8f26d02).  Every
     * LOGF in this file is therefore invisible on the console -- the only
     * channel is the on-screen readout on the world map page.
     *
     * These capture the FIRST FindAnimals build of the session, which is the
     * one that gets it wrong, so later rebuilds cannot overwrite the evidence.
     *   s_iconCalls   how many times the hook ran (>0 = the hook is reached)
     *   s_iconN       entries FindAnimals returned
     *   s_iconMt      entries whose MethodTable equals our horse's (real Horses)
     *   s_iconPtr     entries matching by pointer
     *   s_iconGuid    entries matching by HorseId Guid
     *   s_iconFixed   entries whose rect we actually moved
     *   s_iconY0      the first matched entry's rect Y before the shift
     *   s_iconTexH    our horse's sheet height (256 = tractor rows present)
     *   s_iconHorse   1 = we had a horse pointer at all */
    static unsigned s_iconCalls = 0;
    static unsigned s_iconN = 0;
    static unsigned s_iconMt = 0;
    static unsigned s_iconPtr = 0;
    static unsigned s_iconGuid = 0;
    static unsigned s_iconFixed = 0;
    static unsigned s_iconY0 = 0;
    static unsigned s_iconTexH = 0;
    static unsigned s_iconHorse = 0;

    /* v290 -- auto-attack counters (row 12 of the map readout).
     *   s_combatCalls = damageMonster calls issued while driving
     *   s_combatHits  = how many of them actually found a monster in range
     * Both saturate at 0xFF; they exist to answer "did the call do anything",
     * not to count kills. */
    static unsigned s_combatCalls = 0;
    static unsigned s_combatHits = 0;
    /* v291: tile-moves where the held tool WAS the scythe, so the attack was
     * skipped on purpose.  Non-zero here plus zero hits is the expected
     * picture when driving with a scythe. */
    static unsigned s_combatScythe = 0;

    /* v297 -- how the terrain features on the tractor's tiles were classified.
     *   s_grassSlot  = the performToolAction slot held the Grass body (exact)
     *   s_grassField = slot did not match, but grassType/numberOfWeeds did
     * Both 0 with tiles under the tractor means we never even looked at a
     * Grass, and the row-9 `cut` byte explains the rest. */
    static unsigned s_grassSlot = 0;
    static unsigned s_grassField = 0;
    /* v300 -- milk pail / shears plumbing.
     *   s_milkBegin = how many times beginUsing was called for those tools
     *   s_milkFound = how many of those left a non-null Tool.animal (+0x130)
     * s_milkFound > 0 with nothing in the inventory means the lookup works and
     * the animal simply had no produce; both 0 means beginUsing never ran. */
    static unsigned s_milkBegin = 0;
    static unsigned s_milkFound = 0;

    /* v293 -- walk-speed diagnostics.
     *
     * The 走路加速 cheat is an Atmosphere code patch on the float constant that
     * Farmer.getMovementSpeed builds at 0x13DA0BC / 0x13DA0C0 (see
     * docs/CHEATS.md).  Two words, and the multiplier is readable straight out
     * of them:
     *     vanilla  52856048 / 72A7B0E8   -> 0x3D872B02 = 0.066
     *     3x       52981068 / 72A7C948   -> 0x3E4AC083 = 0.198
     *
     * Reported symptom: walking feels un-boosted after dismounting the tractor,
     * and sleeping fixes it.  Sleeping cannot restore patched CODE, so the
     * interesting question is which of these is true when it happens:
     *
     *   words still 52981068/72A7C948 -> the cheat is intact, and the game is
     *       simply not taking that path.  getMovementSpeed 0x13DA0A8 checks
     *       Game1.CurrentEvent: when an event exists and
     *       Event.??? 0x130A7F0 answers false, the function returns
     *       Character.speed (0x12201A0) instead of the constant, which is
     *       exactly a speed that ignores the cheat.
     *   words back to vanilla -> something really did rewrite the code, and
     *       that is a much bigger problem.
     *
     * s_eventActive answers the first half on sight. */
    static unsigned s_eventActive = 0;
    constexpr uintptr_t kGetCurrentEventOff = SDV_OFF(kGetCurrentEventOff, 0x1442C10u);
    constexpr uintptr_t kSpeedConstWord0Off = SDV_OFF(kSpeedConstWord0Off, 0x13DA0BCu);
    constexpr uintptr_t kSpeedConstWord1Off = SDV_OFF(kSpeedConstWord1Off, 0x13DA0C0u);

    void* HookGetPlayer(void* a0) {
        void* player = s_origGetPlayer(a0);
        s_hits++;
        if (s_hits <= 3) {
            Logging.Log(EXL_LOG_PREFIX "stage2: get_player #%u arg=%p player=%p",
                        s_hits, a0, player);
        }
        return player;
    }

    /* Calibration state, derived from vanilla's own portrait placement.
     *
     * 0x1BBB430/0x1BBB510 compute, for a (location, tile):
     *
     *     rect   = position[0x10..0x20]        cached screen rect (texture * 4)
     *     t      = (tile.x / mapW, tile.y / mapH)
     *     pixel  = rect.xy + rect.wh * t
     *
     * and drawMiniPortraits places the portrait at mapBounds.xy + pixel + 32.
     * Vanilla does that once per frame for the player, so hooking the portrait
     * draw hands us (pixel, tile) -- two unknowns, one equation each, solved
     * for mapW/mapH.  From then on every character's position is arithmetic on
     * values we read directly; nothing else is called. */
    /* Scale from tile to map pixel, calibrated from the player's own portrait
     * -- the only position vanilla hands out for free in single player. */
    void* s_mapPage = nullptr;
    float s_kx = 0.0f, s_ky = 0.0f;
    int   s_rectX = 0, s_rectY = 0;

    /* 0x1BBB510(position, location, tile) -> Vector2 map pixel.  This is the
     * function that turns a tile into a map pixel, and vanilla calls it for the
     * player every frame the map is open, so hooking it hands us a free
     * calibration sample: (tile, pixel).  The portrait helper at 0x17E91F0 was
     * the obvious place, but it never fired -- the call must be inlined. */
    using PixelFn = Vec2 (*)(void* pos, void* loc, u64 tile);
    PixelFn s_origPixel = nullptr;

    Vec2 HookPixel(void* pos, void* loc, u64 tile) {
        Vec2 r = s_origPixel(pos, loc, tile);

        const int tx = static_cast<int>(static_cast<u32>(tile));
        const int ty = static_cast<int>(static_cast<u32>(tile >> 32));
        if (pos == nullptr || tx <= 0 || ty <= 0) {
            return r;
        }
        /* The rect is cached into the position object by the call above. */
        const int* rc = reinterpret_cast<const int*>(
            reinterpret_cast<const unsigned char*>(pos) + 0x10);
        if (rc[2] <= 1 || rc[3] <= 1) {
            return r;
        }
        const float kx = (r.x - static_cast<float>(rc[0])) / static_cast<float>(tx);
        const float ky = (r.y - static_cast<float>(rc[1])) / static_cast<float>(ty);
        if (kx > 0.0001f && ky > 0.0001f && kx < 100.0f && ky < 100.0f) {
            s_kx = kx;
            s_ky = ky;
            s_rectX = rc[0];
            s_rectY = rc[1];
        }
        return r;
    }

    /* v278 DIAGNOSTIC switches for the "naming a horse freezes" bug.
     *
     * Both default to false so a normal build is unchanged.  The diagnostic
     * builds flip one of them and keep everything else identical, which is the
     * only way to tell which hook is responsible: the naming path touches very
     * little of our code, and guessing has already cost two rounds.
     *
     *   kDiagNoStr  -- HookStringEquals defers to the original immediately.
     *                  Horse.nameHorse runs `n.Name == name` for every
     *                  villager, and that comparison goes through the same
     *                  op_Equality thunk we hook, so this is the one hook that
     *                  provably executes inside the naming flow.
     *   kDiagNoDraw -- drawHUD / drawMiniPortraits / drawPortrait /
     *                  OptionsPage.draw call the original and then return
     *                  without doing any of our work.  drawHUD is the
     *                  interesting one: it calls MaybeGrowBackpack() and
     *                  appends a checkbox to the options list, i.e. real work
     *                  inside a draw hook. */
    static const bool kDiagNoStr  = false;
    static const bool kDiagNoDraw = false;
    /* kDiagNoAnim -- HookCheckSingleAnimation calls the original once and
     * returns.  Turning the 2x checkbox off does NOT test this properly: the
     * hook still runs on every animated sprite every frame, still does the
     * Plausible/RangeReadable checks and still calls TryPerformSave.  Only
     * removing the hook tests the hook itself. */
    static const bool kDiagNoAnim = false;
    /* The two halves of HookGameUpdate, split so the culprit can be named.
     *
     * A naming menu pauses the world, so checkForSingleAnimation and the draw
     * hooks are not even reached -- but Game1.Update still runs every frame,
     * which makes HookGameUpdate the only code of ours that executes while the
     * menu is up.  If the menu is still on screen when the game freezes, this
     * is where to look. */
    static const bool kDiagNoAuto = false;   /* skip AutoTractorUse   */
    static const bool kDiagNoFix  = false;   /* skip FixTractorSprite */

    void HookDrawPortrait(void* who, int index, float x, float y) {
        s_origDrawPortrait(who, index, x, y);
        if (kDiagNoDraw) {
            return;
        }

        if (s_mapPage == nullptr) {
            return;
        }
        const auto* mb = reinterpret_cast<const unsigned char*>(s_mapPage);
        const int* bounds = reinterpret_cast<const int*>(mb + kMapBoundOff);

        void* pos = *reinterpret_cast<void**>(const_cast<unsigned char*>(mb) + kMapPosOff);
        if (pos == nullptr) {
            return;
        }
        const int* r = reinterpret_cast<const int*>(
            reinterpret_cast<const unsigned char*>(pos) + 0x10);
        const int rectX = r[0], rectY = r[1], rectW = r[2], rectH = r[3];
        if (rectW <= 1 || rectH <= 1) {
            return;
        }

        /* Tile of the player, stored in the same context. */
        const u64 tile = *reinterpret_cast<const u64*>(mb + kMapPosOff + 16);
        const int tx = static_cast<int>(tile & 0xFFFFFFFFu);
        const int ty = static_cast<int>(tile >> 32);
        if (tx <= 0 || ty <= 0) {
            return;
        }

        /* Undo what vanilla added: screen = bounds + pixel + 32. */
        const float px = x - static_cast<float>(bounds[0]) - 32.0f;
        const float py = y - static_cast<float>(bounds[1]) - 32.0f;

        const float kx = (px - static_cast<float>(rectX)) / static_cast<float>(tx);
        const float ky = (py - static_cast<float>(rectY)) / static_cast<float>(ty);
        if (kx > 0.0001f && ky > 0.0001f && kx < 100.0f && ky < 100.0f) {
            s_kx = kx;
            s_ky = ky;
            s_rectX = rectX;
            s_rectY = rectY;
        }
    }

    /* Defined next to Plausible(), below; declared here because the string
     * helpers above need it. */
    bool RangeReadable(const void* p, size_t bytes);

    /* Compare a managed UTF-16 string (length at +0x10, data at +0x14) with
     * an ASCII literal from the generated table. */
    bool NameEquals(const void* managed, const char* ascii) {
        if (managed == nullptr || ascii == nullptr) {
            return false;
        }
        if (!RangeReadable(managed, 0x14u)) {
            return false;
        }
        const int len = *reinterpret_cast<const int*>(
            reinterpret_cast<const unsigned char*>(managed) + 0x10);
        if (len <= 0 || len > 64) {
            return false;
        }
        if (!RangeReadable(
                reinterpret_cast<const unsigned char*>(managed) + 0x14,
                static_cast<size_t>(len) * 2u)) {
            return false;
        }
        const char16_t* src = reinterpret_cast<const char16_t*>(
            reinterpret_cast<const unsigned char*>(managed) + 0x14);
        for (int i = 0; i < len; i++) {
            if (src[i] != static_cast<unsigned char>(ascii[i]) || ascii[i] == 0) {
                return false;
            }
        }
        return ascii[len] == 0;
    }

    /* Length of a bare managed string, or -1 when the pointer is unusable.
     *
     * "Bare" means the field already points at the string object (length at
     * +0x10, UTF-16 at +0x14) -- MapPage.hoverText, GameLocation._displayName
     * and Character._displayName are all this shape, unlike the NetString
     * fields which need a +0x68 unwrap first. */
    int ManagedStrLen(const void* s) {
        if (s == nullptr || !RangeReadable(s, 0x14u)) {
            return -1;
        }
        const int len = *reinterpret_cast<const int*>(
            reinterpret_cast<const unsigned char*>(s) + 0x10);
        if (len < 0 || len > 256) {
            return -1;
        }
        return len;
    }

    /* Compare two bare managed strings by content.
     *
     * This is the whole "which location is the cursor over" test: hoverText
     * and GameLocation._displayName are the same type, so no ASCII conversion
     * and no localisation table is needed -- which matters because the names
     * are Chinese here and NameToAscii() would flatten them all to "?????".
     *
     * Empty is never equal to empty: a failed read also produces length 0, and
     * letting those match is exactly the bug that put quest badges on every
     * villager in v108. */
    bool ManagedStrEquals(const void* a, const void* b) {
        if (a == nullptr || b == nullptr) {
            return false;
        }
        if (a == b) {
            return true;
        }
        const int la = ManagedStrLen(a);
        const int lb = ManagedStrLen(b);
        if (la <= 0 || la != lb) {
            return false;
        }
        if (!RangeReadable(
                reinterpret_cast<const unsigned char*>(a) + 0x14,
                static_cast<size_t>(la) * 2u) ||
            !RangeReadable(
                reinterpret_cast<const unsigned char*>(b) + 0x14,
                static_cast<size_t>(la) * 2u)) {
            return false;
        }
        const char16_t* sa = reinterpret_cast<const char16_t*>(
            reinterpret_cast<const unsigned char*>(a) + 0x14);
        const char16_t* sb = reinterpret_cast<const char16_t*>(
            reinterpret_cast<const unsigned char*>(b) + 0x14);
        for (int i = 0; i < la; i++) {
            if (sa[i] != sb[i]) {
                return false;
            }
        }
        return true;
    }

    /* Managed UTF-16 string -> ASCII buffer (length at +0x10, data at +0x14).
     * Returns false when the string is missing or absurdly long. */    bool NameToAscii(const void* managed, char* out, int cap) {
        if (managed == nullptr || out == nullptr || cap < 2) {
            return false;
        }
        /* Both the length word and the characters are read blind, so both get
         * a range check.  This used to be unguarded: it was only ever handed
         * pointers that had already been range-checked by the caller, and the
         * quest path broke that assumption -- a wrong field offset landed on a
         * pointer whose page was mapped but whose +0x10 was not, and the game
         * crashed on the read. */
        if (!RangeReadable(managed, 0x14u)) {
            return false;
        }
        const int len = *reinterpret_cast<const int*>(
            reinterpret_cast<const unsigned char*>(managed) + 0x10);
        if (len <= 0 || len >= cap) {
            return false;
        }
        if (!RangeReadable(
                reinterpret_cast<const unsigned char*>(managed) + 0x14,
                static_cast<size_t>(len) * 2u)) {
            return false;
        }
        const char16_t* src = reinterpret_cast<const char16_t*>(
            reinterpret_cast<const unsigned char*>(managed) + 0x14);
        for (int i = 0; i < len; i++) {
            if (src[i] > 0x7F) {
                out[i] = '?';
            }
            else {
                out[i] = static_cast<char>(src[i]);
            }
        }
        out[len] = '\0';
        return true;
    }

    /* ASCII name compare, case-insensitive and ignoring leading spaces.
     *
     * Quest targets come out of a NetString field that may hold either the
     * internal key ("Abigail") or something already cased for display; the
     * NPC side is always the internal key.  Comparing case-insensitively
     * costs nothing and stops a one-letter case difference from silently
     * hiding the badge. */
    inline char LowerAscii(char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
    }
    inline bool NameMatchCi(const char* a, const char* b) {
        if (a == nullptr || b == nullptr) {
            return false;
        }
        while (*a == ' ') {
            ++a;
        }
        /* Two empty strings must never compare equal here.
         *
         * They did, and that is what put quest badges on the wrong villagers:
         * a quest field that held nothing but a space reduced to "" on one
         * side, an NPC whose name read failed also produced "", and the loop
         * below returned true on the first iteration.  Every such NPC then
         * got a badge.  A real name is never empty, so "" means "no answer"
         * on both sides. */
        if (*a == '\0' || *b == '\0') {
            return false;
        }
        for (;;) {
            const char ca = LowerAscii(*a);
            const char cb = LowerAscii(*b);
            if (ca != cb) {
                return false;
            }
            if (ca == '\0') {
                return true;
            }
            ++a;
            ++b;
        }
    }

    /* Managed string compare: length at +0x10, UTF-16 data at +0x14. */
    [[maybe_unused]] bool StrEq(const void* a, const void* b) {
        if (a == b) {
            return true;
        }
        if (a == nullptr || b == nullptr) {
            return false;
        }
        const int la = *reinterpret_cast<const int*>(
            reinterpret_cast<const unsigned char*>(a) + 0x10);
        const int lb = *reinterpret_cast<const int*>(
            reinterpret_cast<const unsigned char*>(b) + 0x10);
        if (la != lb || la <= 0 || la > 64) {
            return false;
        }
        const char16_t* ca = reinterpret_cast<const char16_t*>(
            reinterpret_cast<const unsigned char*>(a) + 0x14);
        const char16_t* cb = reinterpret_cast<const char16_t*>(
            reinterpret_cast<const unsigned char*>(b) + 0x14);
        for (int i = 0; i < la; i++) {
            if (ca[i] != cb[i]) {
                return false;
            }
        }
        return true;
    }

    /* Sanity test for a pointer read out of a managed object.  Retail
     * hardware faults on a wild dereference where the emulator quietly returns
     * garbage, so a plain range check is not enough -- `locCount` can describe
     * more entries than the array really holds, and those extra slots are
     * garbage that still looks like an aligned pointer.  Ask the kernel
     * whether the page is actually mapped.
     *
     * svcQueryMemory is far too slow to run per character per frame (the map
     * hook froze the game at ~40k calls/frame), so results are memoised per
     * 4 KiB page in a small direct-mapped cache.  The game's heap layout is
     * stable for a session, so a hit is always still valid. */
    bool MappedSlow(u64 v) {
        MemoryInfo mi{};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&mi, &page, v))) {
            return false;
        }
        if (mi.type == MemType_Unmapped || (mi.perm & Perm_R) == 0) {
            return false;
        }
        return v >= mi.addr && v < mi.addr + mi.size;
    }

    bool Plausible(const void* p) {
        const u64 v = reinterpret_cast<u64>(p);
        if (v < 0x10000ull || (v & 7u) != 0) {
            return false;
        }
        constexpr unsigned kSlots = 256;
        static u64 sPage[kSlots];
        static unsigned char sOk[kSlots];
        const u64 page = v >> 12;
        const unsigned slot = static_cast<unsigned>((page * 2654435761ull) >> 8) & (kSlots - 1u);
        if (sPage[slot] == page) {
            return sOk[slot] != 0;
        }
        const bool ok = MappedSlow(v);
        sPage[slot] = page;
        sOk[slot] = ok ? 1u : 0u;
        return ok;
    }

    /* Plausible() insists on 8-byte alignment because it validates pointers
     * read out of managed fields.  Field scans that step by 4 (looking for the
     * two adjacent ints a size is stored as) hit misaligned addresses on every
     * other step, and the old code fed those straight into Plausible() -- which
     * returned false and made the loop break at its very first iteration.  That
     * silently turned every size scan into a no-op, so textures appeared to
     * carry no width/height at all.  This applies the same page test without
     * the alignment rule (the page is the same either way). */
    bool Readable(const void* p) {
        const u64 v = reinterpret_cast<u64>(p);
        return Plausible(reinterpret_cast<const void*>(v & ~7ull));
    }

    /* Is [p, p+bytes) readable, page by page?
     *
     * Plausible() answers for the single page holding the pointer, so a
     * pointer sitting in the last 16 bytes of a mapping passes while the
     * field just past it does not -- and reading that field faults the game.
     * Every multi-byte read of memory the game owns has to go through this
     * instead.  (The render function has its own copy of this lambda; this one
     * exists so helpers outside it can use the same rule.) */
    bool RangeReadable(const void* p, size_t bytes) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(p);
        if (a < 0x10000u || bytes == 0 || a + bytes < a) {
            return false;
        }
        if (!Readable(p) ||
            !Readable(reinterpret_cast<const void*>(a + bytes - 1))) {
            return false;
        }
        for (uintptr_t page = (a & ~uintptr_t(0xFFFu)) + 0x1000u;
             page < a + bytes; page += 0x1000u) {
            if (!Readable(reinterpret_cast<const void*>(page))) {
                return false;
            }
        }
        return true;
    }

    /* ---- walk-sheet discovery -------------------------------------------
     * A Texture2D is recognised only by its MethodTable, which is what makes
     * scanning memory safe: a random pointer's first word is essentially
     * never the Texture2D vtable, so a match cannot be a false positive.
     *
         * Historical fallback, retained for legacy stages only.  The old
         * inference that c+0x50 was not the sprite field was made on unverified
         * objects and a base-game disassembly.  The matching update getter
         * proves c+0x50 -> NetRef+0x68, then sprite+0x10 is spriteTexture.
         * v93 bypasses this graph search entirely.
     *
     * Field layout is per class, not per instance, so one path found on one
     * villager holds for every villager. */

    bool IsTexture2D(void* p, void* mtTex) {
        return mtTex != nullptr && Plausible(p) &&
               *reinterpret_cast<void**>(p) == mtTex;
    }

    /* Size fields of whichever textures the walk rejected.  0x4F/0x288 style
     * counters said the graph was walked and textures were reached, but every
     * one of them failed the 16-wide test -- so the test itself is what has to
     * be looked at, not the traversal.  These numbers are printed so the real
     * layout can be read off the screen instead of guessed again. */
    static int s_dW = 0;   /* width of the first rejected texture */
    static int s_dH = 0;   /* its height */
    static unsigned s_dOff = 0;  /* field the pair sat at */
    static unsigned s_dHop = 0;  /* 0 = own fields, 1 = platform texture */
    static unsigned s_lw[4];     /* widths of the first four rejected ones */
    static unsigned s_lwN = 0;
    /* The first rejected texture itself.  Its size cannot be read, so the only
     * way to find out what it is, is to draw it: a 16x15 crop of a villager
     * sheet shows a head, a crop of anything else does not. */
    static void* s_dTex = nullptr;

    /* Best plausible (width, height) pair among one object's own fields.
     * Returns 2 when an exact 16 turns up (villager walk sheets are 16 px
     * wide), 1 for the narrowest other pair, 0 when nothing looks like a size.
     * Narrowest rather than first: a 64-wide portrait and a 16-wide walk sheet
     * can both be reachable, and the sheet is the narrow one.  The window is
     * deliberately wide (0x200) because the runtime is free to order instance
     * fields any way it likes -- the previous 0x80 window reported nothing at
     * all, which says more about the window than about the textures. */
    int ScanDims(void* o, int* w, int* h, unsigned* off) {
        *w = 0;
        *h = 0;
        *off = 0;
        if (!Plausible(o)) {
            return 0;
        }
        const auto* ob = reinterpret_cast<const unsigned char*>(o);
        int best = 0;
        for (int f = 0x08; f <= 0x200; f += 4) {
            if (!Readable(ob + f + 12)) {
                break;
            }
            const int a = *reinterpret_cast<const int*>(ob + f);
            const int b = *reinterpret_cast<const int*>(ob + f + 4);
            /* Height may legitimately be smaller than width (a 128x64 sheet),
             * so no ordering is assumed.  8 is the smallest believable width:
             * 1/2/4 are far more likely to be counts or flags. */
            if (a < 8 || a > 4096 || b < 8 || b > 16384) {
                continue;
            }
            if (a == 16) {
                *w = a;
                *h = b;
                *off = static_cast<unsigned>(f);
                return 2;
            }
            if (best == 0 || a < *w) {
                *w = a;
                *h = b;
                *off = static_cast<unsigned>(f);
                best = 1;
            }
        }
        return best;
    }

    /* Where does this Texture2D keep its size?  MonoGame does not guarantee
     * width/height are instance fields of the managed wrapper -- with the NVN
     * backend they live on the platform texture one hop in -- so both the
     * object and its pointer fields are checked.  Returns 2 when a 16-wide
     * pair was found (hop says where), 1 when only some other pair, 0 when
     * neither level exposed a size. */
    int TextureDims(void* t, int* w, int* h, unsigned* off, unsigned* hop) {
        *w = 0;
        *h = 0;
        *off = 0;
        *hop = 0;
        if (!Plausible(t)) {
            return 0;
        }
        int bw = 0;
        int bh = 0;
        unsigned bo = 0;
        const int own = ScanDims(t, &bw, &bh, &bo);
        if (own == 2) {
            *w = bw;
            *h = bh;
            *off = bo;
            return 2;
        }
        if (own == 1) {
            *w = bw;
            *h = bh;
            *off = bo;
        }
        const auto* tb = reinterpret_cast<const unsigned char*>(t);
        for (int f = 0x08; f <= 0x200; f += 8) {
            if (!Plausible(tb + f + 8)) {
                break;
            }
            void* p = *reinterpret_cast<void* const*>(tb + f);
            if (!Plausible(p)) {
                continue;
            }
            int pw = 0;
            int ph = 0;
            unsigned po = 0;
            const int r = ScanDims(p, &pw, &ph, &po);
            if (r == 2) {
                *w = pw;
                *h = ph;
                *off = po;
                *hop = 1;
                return 2;
            }
            if (r == 1 && (*w == 0 || pw < *w)) {
                *w = pw;
                *h = ph;
                *off = po;
                *hop = 1;
            }
        }
        return (*w != 0) ? 1 : 0;
    }

    /* Villager walk sheets are 16 px wide and a whole number of 16x32 frames
     * tall.  Portraits (64 wide) and the world map are rejected by this, so a
     * texture merely reachable through the location cannot impersonate one. */
    bool LooksLikeWalkSheet(void* t) {
        int w = 0;
        int h = 0;
        unsigned o = 0;
        unsigned hop = 0;
        if (TextureDims(t, &w, &h, &o, &hop) != 2) {
            return false;
        }
        return h >= 32 && h <= 16384;
    }

    /* Is this pointer a managed string holding a short ASCII name?
     * Mono-style strings keep the length as an int at +0x10 and UTF-16 code
     * units at +0x14; NPC names are 2..32 printable ASCII characters, which
     * no other object in a character's neighbourhood satisfies. */
    bool AsciiNameAt(void* s, unsigned* len) {
        if (!Plausible(s) ||
            !Plausible(reinterpret_cast<unsigned char*>(s) + 0x60)) {
            return false;
        }
        const int n = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(s) + 0x10);
        if (n < 2 || n > 32) {
            return false;
        }
        const auto* ch = reinterpret_cast<const unsigned char*>(s) + 0x14;
        for (int k = 0; k < n; k++) {
            const unsigned u =
                ch[k * 2] | (static_cast<unsigned>(ch[k * 2 + 1]) << 8);
            if (u < 0x20u || u > 0x7Eu) {
                return false;
            }
        }
        *len = static_cast<unsigned>(n);
        return true;
    }

    /* Where does this object keep its own name?  Tries each 8-byte field both
     * as a direct string and as a +0x68 NetRef payload.  Bit 15 of the result
     * flags the NetRef hop.  Returns 0xFFFF when nothing looks like a name --
     * which would mean the object is not a Character at all, so this doubles
     * as a proof of identity.  Also counts how many fields hold a plausible
     * pointer: a real Character is pointer-dense, a stub is not. */
    [[maybe_unused]] unsigned FindNameField(void* obj, unsigned* lenOut,
                                            unsigned* c0, unsigned* c1,
                                            unsigned* ptrFields) {
        *lenOut = 0;
        *c0 = 0;
        *c1 = 0;
        *ptrFields = 0;
        if (!Plausible(obj)) {
            return 0xFFFFu;
        }
        const auto* ob = reinterpret_cast<const unsigned char*>(obj);
        unsigned found = 0xFFFFu;
        for (int o = 0x08; o <= 0x400; o += 8) {
            if (!Plausible(ob + o + 8)) {
                break;
            }
            void* p = *reinterpret_cast<void* const*>(ob + o);
            if (!Plausible(p)) {
                continue;
            }
            if (*ptrFields < 0xFFu) {
                (*ptrFields)++;
            }
            if (found != 0xFFFFu) {
                continue; /* keep counting pointer fields */
            }
            unsigned nl = 0;
            if (AsciiNameAt(p, &nl)) {
                found = static_cast<unsigned>(o);
            }
            else if (Plausible(reinterpret_cast<unsigned char*>(p) + 0x70) &&
                     AsciiNameAt(*reinterpret_cast<void**>(
                                     reinterpret_cast<unsigned char*>(p) + 0x68),
                                 &nl)) {
                found = static_cast<unsigned>(o) | 0x8000u;
            }
            if (found != 0xFFFFu) {
                void* s = p;
                if ((found & 0x8000u) != 0u) {
                    s = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(p) + 0x68);
                }
                const auto* ch =
                    reinterpret_cast<const unsigned char*>(s) + 0x14;
                *lenOut = nl;
                *c0 = ch[0];
                *c1 = ch[2];
            }
        }
        return found;
    }

    /* Depth-limited depth-first walk over 8-byte managed fields.  `left` is
     * how many hops remain, `limit` the highest field offset to read.
     * Returns the hops taken from `obj`'s fields (0 = nothing found) and
     * writes them to out[0..n-1].
     *
     * DFS rather than BFS: a breadth-first frontier needs a node cap to stay
     * cheap, and the cap cuts off exactly the branch that holds the sprite
     * once a few wide objects get expanded.  Following one branch to the
     * limit and moving on keeps every field of the character reachable.
     * A texture that is not 16 wide is counted in *loose but not followed:
     * seeing one means the graph was walked correctly and the width test is
     * what rejected it. */
    int FindSheetFrom(void* obj, void* mtTex, int left, int limit, unsigned* out,
                      int* loose) {
        if (left <= 0 || !Plausible(obj)) {
            return 0;
        }
        const auto* ob = reinterpret_cast<const unsigned char*>(obj);
        /* Start at 0x08: field 0 is the MethodTable, not a field. */
        for (int f = 0x08; f <= limit; f += 8) {
            if (!Plausible(ob + f + 8)) {
                break;
            }
            void* v = *reinterpret_cast<void* const*>(ob + f);
            if (!Plausible(v)) {
                continue;
            }
            if (IsTexture2D(v, mtTex)) {
                if (LooksLikeWalkSheet(v)) {
                    out[0] = static_cast<unsigned>(f);
                    return 1;
                }
                /* Record what these rejected textures actually are: their
                 * widths settle whether the 16 test is wrong or whether the
                 * walk simply never reached the character's sheet.  The
                 * narrowest one wins rather than the first one seen -- depth
                 * first order is arbitrary, and the walk sheet is by far the
                 * narrowest texture a character owns (a portrait is 64 wide). */
                {
                    int tw = 0;
                    int th = 0;
                    unsigned to = 0;
                    unsigned thp = 0;
                    TextureDims(v, &tw, &th, &to, &thp);
                    if (s_lwN < 4) {
                        /* Eight bits per width: sizes above 255 are not
                         * villager sheets anyway, so saturating loses nothing
                         * useful. */
                        s_lw[s_lwN] =
                            (tw > 0xFF) ? 0xFFu : static_cast<unsigned>(tw);
                        s_lwN++;
                    }
                    if (tw > 0 && (s_dW == 0 || tw < s_dW)) {
                        s_dW = tw;
                        s_dH = th;
                        s_dOff = to;
                        s_dHop = thp;
                        s_dTex = v;
                    }
                }
                (*loose)++;
                continue; /* never walk into a texture */
            }
            unsigned sub[3] = {0, 0, 0};
            const int d = FindSheetFrom(v, mtTex, left - 1, 0x100, sub, loose);
            if (d > 0) {
                out[0] = static_cast<unsigned>(f);
                for (int k = 0; k < d; k++) {
                    out[k + 1] = sub[k];
                }
                return d + 1;
            }
        }
        return 0;
    }

    /* Path to a 16-wide walk sheet reachable from `root` (0 = none).
     * The first level gets a wide window: a Character carries far more state
     * than 0x100 bytes, and instance fields are reordered by the runtime, so
     * the sprite pointer can sit anywhere in it. */
    int FindSheet(void* root, void* mtTex, unsigned* out, int* loose) {
        if (!Plausible(root) || mtTex == nullptr) {
            return 0;
        }
        return FindSheetFrom(root, mtTex, 3, 0x300, out, loose);
    }

    void HookDrawMiniPortraits(void* self, void* batch, float alpha) {
        s_mapDraws++;
        s_frame++;
        s_mapPage = self;
        /* Stage 126 readout, filled in early but drawn at the very end of the
         * function.  Drawing it here put it *under* the avatars -- on the first
         * hardware run the desert villagers covered row 0 completely. */
        static unsigned s_hoverRow0 = 0;
        static unsigned s_hoverRow1 = 0;
        static unsigned s_hoverRow2 = 0;
        static unsigned s_hoverRow3 = 0;
        /* v199: the write-path probe.  Row 4 was free because the stage tag
         * used to sit at y = 236, which is exactly where a fifth row goes. */
        static unsigned s_hoverRow4 = 0;
        /* v202: the fingerprint autopsy.  Labelled `E`, not `5`, because the
         * legacy 9-row column on the left already uses 0..8 and a sixth `5`
         * on screen would be the same trap that made "row 4" ambiguous. */
        static unsigned s_hoverRow5 = 0;
        /* v206: the event order, labelled `F` for the same reason `E` is not
         * `5` -- the legacy column owns 0..8. */
        static unsigned s_hoverRow6 = 0;
        /* Last string we wrote into hoverText.  Guards against appending on
         * every frame when the game does NOT rebuild hoverText per frame:
         * if the field still holds our merged string, skip; if the game put
         * the original back, we append once to that fresh original. */
        static void* s_lastAppendSrc = nullptr;
        /* The GameLocation the cursor is over, resolved by the stage-132
         * matching block and consumed by the stage-138 list.  Reset at the top
         * of every matching attempt so a failed match cannot leave the previous
         * location's NPC list on screen. */
        static void* s_hoverLoc = nullptr;
        static unsigned s_hoverLocIdx = 0xFFu;
        /* The tooltip's id string ("{areaId}/{tooltipId}"), used to look its
         * PixelArea up in tooltip_rect_table.hpp.  Kept separately from the
         * object pointer because that is what the table is keyed by. */
        static void* s_hoverTipId = nullptr;
        /* Stage 151: the villager whose birthday it is today, captured by the
         * avatar pass (it already evaluates the birthday table per marker).
         * The hover block turns it into one HUD message per in-game day. */
        static int s_bdayDay = -1;

        /* What the avatar pass resolved this frame: for every character it
         * accepted, the location it came from and the character itself.  The
         * stage-139 list reads these when the hovered location's own character
         * list cannot be read -- and they are, by construction, the same
         * characters whose avatars the player can see, which is exactly what
         * the list exists to disambiguate.  Rebuilt from scratch every frame. */
        constexpr int kMaxAvatarRecords = 256;
        static void* s_avLocPtr[kMaxAvatarRecords];
        static void* s_avNpcPtr[kMaxAvatarRecords];
        static float s_avX[kMaxAvatarRecords];
        static float s_avY[kMaxAvatarRecords];
        static int s_avCount = 0;
        s_origDrawMini(self, batch, alpha);
        if (kDiagNoDraw) {
            return;
        }
        /* v312: 地图增强 -- one switch for everything this hook adds (avatars,
         * badges, hover list, birthday toast).  Off means the vanilla map, which
         * is exactly what the original call above already drew.
         *
         * The readout goes with it: it is drawn inside the avatar pass, and
         * threading a "draw only the numbers" path through 7000 lines of marker
         * code to keep a diagnostic visible is not worth the risk.  Turn the row
         * back on when you need to read the numbers. */
        if (g_mapOn == 0) {
            return;
        }

        /* Hardware bisect stage 1: the hook is installed and the original
         * runs, but nothing of ours does.  If the map still crashes here the
         * hook target itself is wrong for the retail build; if it opens fine
         * the fault is inside the marker code below. */
        if (kMapStage == 1) {
            return;
        }

        if (batch == nullptr) {
            return;
        }

        auto draw = reinterpret_cast<DrawRectFn>(g_base + kDrawRectOff);
        auto drawSprite = reinterpret_cast<DrawSpriteFn>(g_base + kDrawSpriteOff);
        /* Legacy paths used guessed raw coordinates.  The 2026-10-23 audit
         * found their static disassembly input was the base game, not the
         * update NSO.  The update getters are real functions; v93 below uses
         * verified wrapper/field reads and bypasses those legacy paths. */
        auto getPlayer = reinterpret_cast<GetPlayerFn>(g_base + kGetPlayerOff);
        void* player = getPlayer(nullptr);
        void* texture = *reinterpret_cast<void**>(g_base + kStaminaRectOff);
        if (texture == nullptr) {
            return;
        }
        /* staminaRect is a Texture2D, so its MethodTable is Texture2D's.
         * "first word equals this" cannot false-positive, which is what makes
         * finding the sprite sheet by scanning a safe operation. */
        void* mtTex = *reinterpret_cast<void**>(texture);
        const auto* mb = reinterpret_cast<const unsigned char*>(self);
        const int* bounds = reinterpret_cast<const int*>(mb + kMapBoundOff);

        /* Indicator lamps along the top edge:
         *   1 red   = map hook runs
         *   2 green = portrait hook fired (scale known)
         *   3 blue  = characters enumerated
         *   4 cyan  = at least one marker drawn
         */
        auto lamp = [&](int idx, u32 color) {
            if (!kShowDiagnostics) {
                return; /* public build: no indicator lamps on the map */
            }
            if (kMapStage == 3) {
                return; /* stage 3 measures the reading path only */
            }
            const u64 xy = (static_cast<u64>(bounds[1] + 12) << 32) |
                           static_cast<u32>(bounds[0] + 12 + idx * 22);
            draw(batch, texture, xy, (18ull << 32) | 18u, color);
        };

        /* 3x5 dot-matrix hex digits.  Reading a bit grid off a photo of a
         * tilted screen is unreliable, so numbers get printed instead. */
        auto glyph = [&](int x, int y, unsigned nibble, int cell) {
            static const unsigned short kFont[16] = {
                0x7B6Fu, 0x749Au, 0x73E7u, 0x79E7u, 0x49EDu, 0x79CFu,
                0x7BCFu, 0x4927u, 0x7BEFu, 0x79EFu, 0x5BEFu, 0x3AEBu,
                0x724Fu, 0x3B6Bu, 0x73CFu, 0x13CFu};
            const unsigned short bits = kFont[nibble & 0xFu];
            for (int row = 0; row < 5; row++) {
                for (int col = 0; col < 3; col++) {
                    if ((bits & (1u << (row * 3 + col))) == 0) {
                        continue;
                    }
                    const u64 xy = (static_cast<u64>(y + row * cell) << 32) |
                                   static_cast<u32>(x + col * cell);
                    draw(batch, texture, xy,
                         (static_cast<u64>(cell) << 32) | static_cast<u32>(cell),
                         0xFFFFFFFFu);
                }
            }
        };
        auto hexnum = [&](int x, int y, unsigned value, int digits, int cell) {
            for (int i = 0; i < digits; i++) {
                const int shift = (digits - 1 - i) * 4;
                glyph(x + i * (cell * 4), y, (value >> shift) & 0xFu, cell);
            }
        };
        lamp(0, 0xFF0000FFu);
        /* The scale probe only exists once the pixel hook is installed, which
         * the bisect stages deliberately skip. */
        if (kMapStage == 0 && s_kx <= 0.0f) {
            return;
        }
        lamp(1, 0xFF00FF00u);

        /* White lamp = svcQueryMemory answered for a pointer we know is valid,
         * so the guard below can actually protect the reads. */
        if (Plausible(self)) {
            lamp(2, 0xFFFFFFFFu);
        }

        /* Stage 132 -- P1 of the hover-name feature: the id is namespaced, so
         * compare its first segment.
         *
         * v131 hit it: 0BE70A0E / 0F0123FF / 6E776F54
         *   area count 0x0B = 11, flags 0xE7, matched area 10, matched tooltip 14
         *   flags 0xE7 = 0x01 container + 0x02 text matched + 0x04 id readable
         *              + 0x20 area 0 tooltips + 0x40 hoverText + 0x80 id is
         *              ASCII.  Only 0x08 (id == a location name) is clear.
         *   idLen 0x0F = 15, 35 tooltips scanned, area 0 has 1
         *   row 2 = 6E776F54 = "Town"
         *
         * So the tooltip was found and its id reads, but the id is 15 chars
         * while GameLocation.name for the town is "Town" (4).  A 15-char ASCII
         * id starting with "Town" and a "/" style separator is
         * "Town/TownSquare" -- exactly 15 characters, and the town square is
         * literally what "小镇广场" means.  So the id is "{areaId}/{tooltipId}"
         * and the first segment is the location key.
         *
         * This stage compares GameLocation.name against the id up to the first
         * '/', and still prints the id's own characters so the format is
         * confirmed rather than assumed:
         *
         * Rows:
         *   0  id characters 4..7   -- for "Town/TownSquare" this is
         *                              "/Town", i.e. 776F542F, which proves the
         *                              separator and the second segment
         *   1  id characters 8..11  -- "nSqu" for the same string
         *   2  id length / first-segment length / flags / matched location
         *      index (FF = none)
         *        flags 0x01 mapAreas container readable
         *              0x02 some tooltip's +0x38 text equals hoverText
         *              0x04 that tooltip's +0x40 id is readable
         *              0x08 the first segment equals a GameLocation.name <- goal
         *              0x10 the whole id equals a GameLocation.name
         *              0x20 area 0 has a readable tooltip list
         *              0x40 hoverText readable
         *              0x80 the id is pure ASCII
         *   3  version tag
         *
         * Still no managed call anywhere in here. */
        if (kMapStage >= 132) {
            unsigned idLen = 0;
            unsigned segLen = 0;
            unsigned flags = 0;
            unsigned matchedLoc = 0xFFu;
            unsigned idChars4 = 0;
            unsigned idChars8 = 0;

            s_hoverLoc = nullptr;
            s_hoverLocIdx = 0xFFu;
            s_hoverTipId = nullptr;

            void* hover = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(self) + kMapHoverTextOff);
            const int hl = ManagedStrLen(hover);
            if (hl >= 0) {
                flags |= 0x40u;
            }

            void** locs = nullptr;
            int locCount = 0;
            auto getLocations132 = reinterpret_cast<GetLocationsFn>(
                g_base + kGetLocationsOff);
            void* locList = getLocations132();
            if (locList != nullptr && Plausible(locList)) {
                void** it = *reinterpret_cast<void***>(
                    reinterpret_cast<unsigned char*>(locList) + 0x10);
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(locList) + 0x18);
                if (it != nullptr && n > 0 && n <= 256) {
                    locs = it;
                    locCount = n;
                }
            }

            /* Same container shape for both: data at +0x10, pointer to the
             * length word at +0x20. */
            auto containerItems = [](void* c) -> void** {
                if (c == nullptr || !Plausible(c)) {
                    return nullptr;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(c) + 0x10);
                if (items == nullptr || !Plausible(items)) {
                    return nullptr;
                }
                return reinterpret_cast<void**>(items);
            };
            auto containerCount = [](void* c) -> int {
                if (c == nullptr || !Plausible(c)) {
                    return 0;
                }
                void* lenp = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(c) + 0x20);
                if (lenp == nullptr ||
                    !RangeReadable(
                        reinterpret_cast<const unsigned char*>(lenp), 4u)) {
                    return 0;
                }
                const int n = *reinterpret_cast<const int*>(lenp);
                return (n > 0 && n <= 4096) ? n : 0;
            };

            void* areas = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(self) + kMapAreasOff);
            void** areaItems = containerItems(areas);
            const int ac = containerCount(areas);
            bool found = false;
            if (areaItems != nullptr && ac > 0 && hl > 0) {
                flags |= 0x01u;
                for (int ai = 0; ai < ac && !found; ai++) {
                    void* a = areaItems[ai];
                    if (a == nullptr || !Plausible(a)) {
                        continue;
                    }
                    void* tipList = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(a) +
                        kMapAreaTooltipsOff);
                    void** tipItems = containerItems(tipList);
                    const int tc = containerCount(tipList);
                    if (tipItems == nullptr || tc <= 0) {
                        continue;
                    }
                    if (ai == 0) {
                        flags |= 0x20u;
                    }
                    for (int ti = 0; ti < tc && !found; ti++) {
                        void* t = tipItems[ti];
                        if (t == nullptr || !Plausible(t)) {
                            continue;
                        }
                        void* text = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(t) +
                            kMapTooltipTextOff);
                        if (!ManagedStrEquals(text, hover)) {
                            continue;
                        }
                        flags |= 0x02u;
                        found = true;

                        /* The tooltip is identified the moment its text
                         * matches -- that is all stage 145 needs.  v145
                         * wrongly stored this inside the *location* match
                         * branch, so a failed name match left s_hoverTip null
                         * and the rectangle unreadable (rectOk = 0 on
                         * hardware).  Store it here instead. */
                        void* id = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(t) +
                            kMapTooltipIdOff);
                        s_hoverTipId = id;
                        const int idl = ManagedStrLen(id);
                        if (idl < 0) {
                            break;
                        }
                        flags |= 0x04u;
                        idLen = static_cast<unsigned>(idl);
                        if (idl == 0 || !RangeReadable(
                                reinterpret_cast<const unsigned char*>(id) +
                                0x14,
                                static_cast<size_t>(idl) * 2u)) {
                            break;
                        }
                        const char16_t* s = reinterpret_cast<const char16_t*>(
                            reinterpret_cast<const unsigned char*>(id) + 0x14);
                        bool ascii = true;
                        /* The tooltip id is "{region}/{location}", e.g.
                         * "Mountain/ScienceHouse".  v143 hardware readout
                         * showed hovering the carpenter's shop resolving to
                         * "Moun" -- because the old code took the text up to
                         * the FIRST slash, i.e. the region.  The location is
                         * the LAST segment, so scan for the final slash. */
                        int segStart = 0;
                        for (int k = 0; k < idl; k++) {
                            if (s[k] >= 0x80u) {
                                ascii = false;
                            }
                            if (s[k] == u'/') {
                                segStart = k + 1;
                            }
                        }
                        if (ascii) {
                            flags |= 0x80u;
                        }
                        segLen = static_cast<unsigned>(idl - segStart);
                        for (int k = 0; k < 4 && (k + 4) < idl; k++) {
                            idChars4 |= static_cast<unsigned>(
                                s[k + 4] & 0xFFu) << (k * 8);
                        }
                        for (int k = 0; k < 4 && (k + 8) < idl; k++) {
                            idChars8 |= static_cast<unsigned>(
                                s[k + 8] & 0xFFu) << (k * 8);
                        }

                        /* Exact match first, then the first segment. */
                        for (int li = 0; li < locCount; li++) {
                            void* l = locs[li];
                            if (l == nullptr || !Plausible(l)) {
                                continue;
                            }
                            void* nw = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(l) +
                                kLocNameOff);
                            if (nw == nullptr || !Plausible(nw)) {
                                continue;
                            }
                            void* ns = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(nw) + 0x68);
                            if (ManagedStrEquals(id, ns)) {
                                flags |= 0x10u;
                            }
                            const int nl = ManagedStrLen(ns);
                            if (nl <= 0 ||
                                static_cast<unsigned>(nl) != segLen) {
                                continue;
                            }
                            if (!RangeReadable(
                                    reinterpret_cast<const unsigned char*>(ns) +
                                    0x14,
                                    static_cast<size_t>(nl) * 2u)) {
                                continue;
                            }
                            const char16_t* nsChar =
                                reinterpret_cast<const char16_t*>(
                                    reinterpret_cast<const unsigned char*>(ns) +
                                    0x14);
                            bool same = true;
                            for (int k = 0; k < nl; k++) {
                                if (nsChar[k] != s[segStart + k]) {
                                    same = false;
                                    break;
                                }
                            }
                            if (same && matchedLoc == 0xFFu) {
                                flags |= 0x08u;
                                matchedLoc = static_cast<unsigned>(li);
                                s_hoverLoc = l;
                                s_hoverLocIdx = static_cast<unsigned>(li);
                            }
                        }
                    }
                }
            }

            s_hoverRow0 = idChars4;
            s_hoverRow1 = idChars8;
            s_hoverRow2 = ((idLen & 0xFFu) << 24) |
                ((segLen & 0xFFu) << 16) |
                ((flags & 0xFFu) << 8) |
                (matchedLoc & 0xFFu);
        }

        /* v93: use the matching update NSO (E7F845093E8CBC68...).
         * The old E-drive main.text.bin is byte-identical to the base NSO,
         * not this update.  Validate unhooked getter fingerprints, then use
         * the actual List indexer layout without calling managed methods.
         * This block returns before every legacy scan/render path. */
        /* Stage 94: draw the avatars.
         *
         * Stage 93 proved the update-build chain on hardware: 83 locations all
         * read, 27 of them non-empty, 40 characters worldwide, 39 names and 40
         * sprites readable, and all 40 spriteTexture fields carrying the
         * Texture2D method table.  So the objects are real villagers and the
         * sheets are already loaded -- nothing has to be loaded lazily.
         *
         * What is drawn is exactly what NPCMapLocations draws: a 16x15 window
         * taken from the top-left of the walk sheet, shifted down by the
         * per-NPC offset in npc_crop_table.hpp.
         *
         * Tile comes from Character.cachedTile (two floats at +0x18/+0x1C),
         * which is what get_Tile returns; the pixel cache at +0x30/+0x34 is the
         * fallback.  The old +0x14/+0x18 ints were a guess made against the
         * base-game binary and are not used.
         *
         * Rows (8 hex digits each), only drawn when kShowDiagnostics is true.
         * This list was stale for several stages; the authoritative per-row
         * description is next to the assignments at the end of the block.
         * Current layout, stage 116.  The quest rows come first on purpose:
         * several rounds of feedback pasted only the top of the readout, and
         * every row is now numbered on the left so a dropped line is obvious.
         *   0: quest count / elements / targets / badges drawn
         *   1: quest flags / quest bools / (gatePass : source) /
         *      (probes refused as non-NetString : shape ok)
         *   2: quest id lengths, one nibble per quest 0..7
         *   3: first four characters of target 0
         *   4: first four characters of target 1
         *   5: daysLeft per quest, one nibble each, 0xF = unreadable
         *   6: per-quest bitmaps 0..7: has a target / completed /
         *      dailyQuest; byte 0 is (offset of target 1 : offset of target 0)
         *   7: avatars drawn / seen / with a sheet / without one
         *   8: locations matched / Farm fallback / bad tiles / birthdays
         *      (the build tag is drawn to the right of this row)
         *
         * Stage 100: avatars 40x37, diagnostics off, overlapping heads fanned out.
         * Stage 101: birthday badge on heads whose owner's birthday is today.
         * Stage 109: quest badge matched only against accepted, daily,
         *            unfinished quests -- see the block below for what v108
         *            got wrong.
         * Stage 110: stop vetoing a quest list just because some of its slots
         *            are empty.  v109 did, and the badge disappeared entirely:
         *            the log's own list is 10 slots holding 5 quests, so
         *            "every slot must be a Quest" rejected the real log.
         * Stage 111: ask the game for the elements (NetList.get_Item through
         *            the vtable, the way Farmer.hasQuest does) instead of
         *            decoding the list by hand, and gate on !completed only --
         *            v110's readout had accepted and dailyQuest reading false
         *            on a quest that was demonstrably in the log.
         * Stage 112: readout reordered (quest rows first) and the target
         *            offsets reported, after v111's target 0 came out as
         *            "Coop" -- a place name from GoSomewhereQuest.whereToGo,
         *            which lives at 0xA8 next to the real `target` fields.
         * Stage 113: the PC mod's rule is applied in full, decided for the
         *            whole log at once instead of per quest (v110's "all three
         *            false" was one story quest, not a broken read), and a
         *            probed field must prove it is a NetString before it may
         *            contribute a name -- v112's "Codi" target was a read past
         *            the end of a 0xA8-byte Quest.
         * Stage 114: move the id / MethodTable / NetBool-layout checks in front
         *            of the gate, and run the probes for rejected quests too.
         *            v113's first hardware readout rejected every quest, and
         *            because those checks sat after the gate it reported
         *            nothing about whether the elements were Quests at all --
         *            a whole round spent to learn "no quest is accepted".
         * Stage 115: drop accepted/dailyQuest from the gate.  Two readouts, one
         *            of them with a quest actually accepted, show every flag
         *            false, so a gate on those two can never open on this
         *            build.  What is left is `!completed` plus the NetString
         *            MethodTable compare, which is what actually rejects the
         *            garbage -- and row 6 now shows the flags per quest so a
         *            future build that does set them can tighten it again.
         * Stage 116: gate on `daysLeft > 0` instead.  v115 with no daily/story
         *            filter drew a badge from a story quest -- target 0 was
         *            "Coop", i.e. GoSomewhereQuest.whereToGo, and target 1
         *            "Hodi" came from past the end of a 0xB0-byte object.
         *            daysLeft is a NetInt at +0x78; it drives quest expiry, so
         *            unlike the two dead NetBools the game has to maintain it.
         *            Row 2 now reports id lengths, row 5 the daysLeft values.
         * Stage 117: dispatch the target offset on `questType` (+0x70, NetInt)
         *            instead of probing 0xA8/0xB0/0xB8 on every quest.  v116's
         *            daysLeft read was correct -- Quest.GetDaysLeft is exactly
         *            `ldr x8,[x0,#0x78]; ldr w0,[x8,#0x68]` -- but the field is
         *            0 for every quest in the log, so the gate is now
         *            `!completed && type in {ItemDelivery, Monster, Fishing,
         *            Resource}`, which is the reference mod's subclass switch.
         *            Row 5 now reports questType per quest.
         * Stage 118: fix the questType numbers.  v117 used the declaration
         *            order of Quest's `type_*` statics (ItemDelivery 2,
         *            Monster 3, Fishing 6, Resource 9) and all four were one
         *            low, so an ItemDelivery quest had its `ItemId` read and a
         *            Monster quest its `targetMessage` -- both rejected by the
         *            name-shape check, so the readout showed types but zero
         *            targets.  The real values are stored by each subclass
         *            constructor (3 / 4 / 7 / 10).  Row 6 byte 1 now carries
         *            questPtrCount, so "type 0" can be told apart from
         *            "element never reached pass 1".
         * Stage 119: the type dispatch works -- v118 drew one badge from an
         *            ItemDelivery quest's target at 0xB0 -- but the quest the
         *            player had just accepted was dropped before pass 1
         *            (ptrCount 4 vs count 5).  unwrapQuest gains a second,
         *            id-independent Quest test: three NetBool wrappers at
         *            +0x30/+0x38/+0x40 sharing one MethodTable plus a small
         *            questType at +0x70.  Row 1's `probe` nibble becomes
         *            `dropCode` so a vanished element says why.
         * Stage 120: v119's fingerprint used three NetBools, and when the list
         *            element is a bare Quest (the path v114 measured) the
         *            first candidate tested is `element+0x68` =
         *            Quest.moneyReward, a NetInt wrapper.  The metadata gives
         *            Quest six NetBools at 0x30..0x58 (type 0x476df0) and three
         *            NetInts at 0x68..0x78 (type 0x4a7770), so the fingerprint
         *            now uses all nine and two distinct MethodTables; the
         *            branch order stops mattering.  `dropCode` widened to 1..5
         *            so a drop says whether the slot is empty, not a Quest, or
         *            a different managed object.
         * Stage 121: Farmer.hasQuest (0x13dfee0) settles what get_Item
         *            returns -- right after the call it reads [x0,#0x60], the
         *            Quest.id NetString, so the element is a Quest and +0x68
         *            is its moneyReward.  The old comment claimed the opposite
         *            and tested the payload first.  More importantly, that
         *            function walks count-1..0 dereferencing every element
         *            with no null check, which proves every index below
         *            NetList.count holds a Quest -- so the shape test was
         *            rejecting a real quest, not garbage.  Elements from the
         *            accessor are now accepted on the strength of the
         *            accessor's own return type, with only questType required
         *            to read.
         * Stage 122: gate on `dailyQuest` again.  v114/v116 concluded the
         *            field was dead because two readouts (one with a quest
         *            accepted) showed it false for every quest.  It is not
         *            dead: 0x1722b14 sets dailyQuest=1, then dayQuestAccepted,
         *            accepted=1, canBeCancelled=1 and daysLeft=2, all on the
         *            quest it just built -- the daily-quest setup, and the only
         *            site in the text segment writing daysLeft=2.  Story
         *            quests never reach it.  Without this gate v121 badged
         *            "Jodi's Request", a fixed story quest, which the PC mod
         *            never does.  Row 6 byte 1 is questDailyMask now. */
        if (kMapStage >= kStageAvatar) {
            s_avCount = 0;
            auto readableRange = [](const void* p, size_t bytes) -> bool {
                const uintptr_t a = reinterpret_cast<uintptr_t>(p);
                if (a < 0x10000u || bytes == 0 || a + bytes < a) {
                    return false;
                }
                if (!Readable(p) ||
                    !Readable(reinterpret_cast<const void*>(a + bytes - 1))) {
                    return false;
                }
                for (uintptr_t page = (a & ~uintptr_t(0xFFFu)) + 0x1000u;
                     page < a + bytes; page += 0x1000u) {
                    if (!Readable(reinterpret_cast<const void*>(page))) {
                        return false;
                    }
                }
                return true;
            };
            auto wordMatches = [&](uintptr_t off, u32 expected) -> bool {
                const void* p = reinterpret_cast<const void*>(g_base + off);
                return readableRange(p, sizeof(u32)) &&
                    *reinterpret_cast<const u32*>(p) == expected;
            };
            unsigned fingerprint = 0;
            if (wordMatches(0x1440790, 0x9005A8E8u) &&
                wordMatches(0x144079C, 0xF9401500u)) {
                fingerprint |= 1u;
            }
            if (wordMatches(0x1220560, 0xF9405408u) &&
                wordMatches(0x1220564, 0xF9403500u)) {
                fingerprint |= 2u;
            }
            if (wordMatches(0x1220590, 0xF9402808u) &&
                wordMatches(0x1220594, 0xF9403500u)) {
                fingerprint |= 4u;
            }
            if (wordMatches(0x153312C, 0xF9405A88u) &&
                wordMatches(0x1533130, 0xF9402D09u)) {
                fingerprint |= 8u;
            }

            auto readVerifiedList = [&](void* list, void*** out, int* count)
                -> bool {
                if (!Plausible(list) || !readableRange(list, 0x1Cu)) {
                    return false;
                }
                auto* lb = reinterpret_cast<unsigned char*>(list);
                const int n = *reinterpret_cast<int*>(lb + 0x18);
                void* wrapper = *reinterpret_cast<void**>(lb + 0x10);
                if (n < 0 || n > 4096 || !Plausible(wrapper) ||
                    !readableRange(wrapper, 0x28u)) {
                    return false;
                }
                auto* wb = reinterpret_cast<unsigned char*>(wrapper);
                void* lengthPtr = *reinterpret_cast<void**>(wb + 0x20);
                if (!readableRange(lengthPtr, sizeof(int))) {
                    return false;
                }
                const int capacity = *reinterpret_cast<int*>(lengthPtr);
                if (capacity < n || capacity < 0 || capacity > 65536) {
                    return false;
                }
                void* buffer = *reinterpret_cast<void**>(wb + 0x10);
                if (n > 0 && (!Plausible(buffer) ||
                    !readableRange(buffer,
                                   static_cast<size_t>(n) * sizeof(void*)))) {
                    return false;
                }
                *out = reinterpret_cast<void**>(buffer);
                *count = n;
                return true;
            };

            /* NPC name as ASCII, for the crop table. */
            auto nameAscii = [&](void* npc, char* out, int cap) -> int {
                out[0] = '\0';
                if (!readableRange(npc, 0xB0u)) {
                    return 0;
                }
                void* netName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(npc) + 0xA8);
                if (!Plausible(netName) || !readableRange(netName, 0x70u)) {
                    return 0;
                }
                void* name = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netName) + 0x68);
                if (!Plausible(name) || !readableRange(name, 0x18u)) {
                    return 0;
                }
                auto* sb = reinterpret_cast<unsigned char*>(name);
                const int len = *reinterpret_cast<int*>(sb + 0x10);
                if (len < 1 || len > 64 ||
                    !readableRange(sb + 0x14, static_cast<size_t>(len) * 2u)) {
                    return 0;
                }
                const auto* chars =
                    reinterpret_cast<const unsigned short*>(sb + 0x14);
                int k = 0;
                for (; k < len && k < cap - 1; k++) {
                    const unsigned c = chars[k];
                    if (c == 0 || c > 0x7Eu) {
                        break;
                    }
                    out[k] = static_cast<char>(c);
                }
                out[k] = '\0';
                return k;
            };

            /* Which character layout this object uses: 0 = ordinary NPC,
             * 1 = child, 2 = horse.
             *
             * The horse and the children are not villagers, so they are not in
             * the crop table and fell back to offset 0 -- a 16x15 window at the
             * top of a sheet whose frames are NOT 16x32.  For a child that is
             * an empty box (a 16x22 frame with the child drawn at the bottom of
             * it); for the horse it is a quarter of a 32x32 frame.
             *
             * Neither can be recognised by name: a child's Name is whatever the
             * player typed and the horse's is whatever it was named.  So they
             * are recognised by a method-table fingerprint -- an MT holds the
             * bodies of the methods a class overrides, so a class-specific body
             * address identifies the class without knowing one field offset.
             * Several bodies per class are accepted, so a single wrong guess
             * cannot disable the feature: a miss just leaves the marker on the
             * old path. */
            auto mtHasBody = [&](void* mt, unsigned off) -> bool {
                const uintptr_t want = g_base + off;
                auto* p = reinterpret_cast<uintptr_t*>(mt);
                for (int i = 0; i < 0x120; i++) {
                    if (!readableRange(p + i, 8u)) {
                        return false;
                    }
                    if (p[i] == want) {
                        return true;
                    }
                }
                return false;
            };
            /* Eight entries is plenty: a save has a few dozen classes and the
             * same handful shows up every frame.  Without it the scan would run
             * 288 readableRange checks per NPC per frame. */
            static void* s_kindMt[8] = {nullptr, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr, nullptr};
            static signed char s_kindVal[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            static int s_kindNext = 0;
            auto characterKind = [&](void* npc) -> int {
                if (!Plausible(npc) || !readableRange(npc, 8u)) {
                    return 0;
                }
                void* mt = *reinterpret_cast<void**>(npc);
                if (!Plausible(mt)) {
                    return 0;
                }
                for (int i = 0; i < 8; i++) {
                    if (s_kindMt[i] == mt) {
                        return s_kindVal[i];
                    }
                }
                int kind = 0;
                if (mtHasBody(mt, 0x1235230u) || mtHasBody(mt, 0x1231C90u)) {
                    kind = 2;   /* Horse.draw / Horse.reloadSprite */
                }
                else if (mtHasBody(mt, 0x122E4E0u) ||
                         mtHasBody(mt, 0x122E6E0u) ||
                         mtHasBody(mt, 0x122A6C0u)) {
                    kind = 1;   /* Child.draw / getMugShotSourceRect */
                }
                s_kindMt[s_kindNext] = mt;
                s_kindVal[s_kindNext] = static_cast<signed char>(kind);
                s_kindNext = (s_kindNext + 1) & 7;
                return kind;
            };

            /* Nine rows since v113.  The extra two are the quest id and the
             * per-quest bool nibbles, which are what the gate decision is
             * made from; the row order still puts the quest rows first so a
             * partial readout is still informative. */
            /* Rows 10-11 (v289) are the animal-page icon diagnostic; see the
             * s_icon* block near the top of the file for what each nibble is.
             * Row 12 (v290) is the auto-attack counter. */
            /* Row 14 (v336) is the carry-chest gate trace; see the s_carry*
             * block near the top of the file. */
            unsigned rows[22] = {};
            rows[0] = 0xF0000000u | fingerprint;
            if (fingerprint == 0xFu) {
                auto getLocations94 = reinterpret_cast<GetLocationsFn>(
                    g_base + kGetLocationsOff);
                auto getTile = reinterpret_cast<Vec2 (*)(void*)>(
                    g_base + kGetTileBodyOff);
                void* list = getLocations94();
                void** locations = nullptr;
                int locationCount = 0;
                unsigned seen = 0, drawn = 0, withSheet = 0, noSheet = 0;
                unsigned matched = 0, fallback = 0, badTile = 0;

                /* One entry per avatar to draw.  Collected in the walk below,
                 * spread apart, then drawn -- see the second pass after the
                 * loops.  128 is well above the ~40 characters the game keeps
                 * instantiated; anything past it is dropped rather than
                 * overflowing. */
                constexpr int kMaxMarkers = 128;
                struct Marker {
                    float ax, ay;   /* where the tile really is */
                    float x, y;     /* where it will be drawn */
                    void* sheet;
                    int sx, sw, sh;  /* source rect; crop is the y */
                    int crop;
                    unsigned char bday;   /* 1 = birthday badge on this head */
                    unsigned char quest;  /* 1 = outstanding quest badge */
                };
                static Marker markers[kMaxMarkers];
                int markerCount = 0;
                int bdayCount = 0;
                int badgeCount = 0;   /* markers that got the quest icon */

                /* Today's date, and the atlas the badge comes from.
                 *
                 * Read once per frame rather than per marker.  `season` stays
                 * -1 when the field does not hold a real date, which switches
                 * the whole badge off -- if these offsets ever move with a
                 * game update the feature disappears instead of painting
                 * garbage on every head.  The atlas is validated the same way
                 * the character sheets are: a Texture2D's MethodTable is the
                 * one staminaRect has, and that cannot false-positive. */
                int season = -1;
                int dayOfMonth = 0;
                {
                    const int s = *reinterpret_cast<const int*>(
                        g_base + kSeasonOff);
                    const int d = *reinterpret_cast<const int*>(
                        g_base + kDayOfMonthOff);
                    if (s >= 0 && s <= 3 && d >= 1 && d <= 28) {
                        season = s;
                        dayOfMonth = d;
                    }
                }
                void* cursors = *reinterpret_cast<void**>(
                    g_base + kMouseCursorsOff);
                if (!Plausible(cursors) ||
                    *reinterpret_cast<void**>(cursors) != mtTex) {
                    cursors = nullptr;
                }

                /* Quests in the log, by the NPC they point at.
                 *
                 * Farmer.questLog sits at +0x1B0 (AOT factory: 319 fields,
                 * type 0x4cbed0 = NetList`2) and the Farmer comes from
                 * Game1._player (+0xC95BB50) -- the backing field the getter
                 * returns -- so no managed call is needed.
                 *
                 * The chain below is read off the game's *own* accessors
                 * rather than guessed:
                 *
                 *   Farmer.hasQuest              0x13dfee0
                 *   Farmer.hasQuest(<type>)      0x13dff90
                 *   Farmer.get_hasVisibleQuests  0x13b7030
                 *
                 * All three do exactly this (0x13dff10, 0x13b7030):
                 *
                 *   ldr x8, [x20, #0x1b0]   ; questLog
                 *   ldr x8, [x8,  #0x50]    ; count wrapper
                 *   ldr w8, [x8,  #0x68]    ; -> int count
                 *   ldr x8, [questLog, #0x58]
                 *   ldr x8, [x8,  #0x68]
                 *   ldr x0, [x8,  #0x58]    ; -> List<T>, handed to get_Item
                 *
                 * So NetList's +0x58 holds a NetArray *wrapper* and the List
                 * is two hops further in -- NOT at +0x58 directly, which is
                 * what the first two attempts assumed (hence questLog parsed
                 * as "farmer ok, list empty").  All three spellings are still
                 * tried, and the one that wins is reported in the low bits of
                 * questFlags, because the characters list reaches its List in
                 * one hop and I do not want a silent divergence if a later
                 * build lays this out differently.
                 *
                 * Quest.accepted / dailyQuest / completed are NetBools at
                 * +0x30 / +0x40 / +0x38, value at +0x68 inside the wrapper.
                 * v108 skipped them on the theory that questLog only holds
                 * live quests; it does not -- it also holds story quests, and
                 * a log with nothing accepted still has four of them.  v113
                 * applies the PC mod's full rule to the whole log at once;
                 * see the gate below.
                 *
                 * `target` is not on the base class.  From the AOT factories:
                 * ItemDeliveryQuest +0xB0, FishingQuest +0xA8,
                 * SlayMonsterQuest +0xB8, ResourceCollectionQuest +0xA8.
                 * Telling those apart would need each subclass's MethodTable,
                 * so all three offsets are probed -- but only on a field that
                 * proves it is a NetString, because a plain Quest is 0xA8
                 * bytes and +0xA8 is already outside it. */
                /* Three offsets per quest, so the cap has to be well above
                 * the number of quests: a mid-game log holds five, and the
                 * one we care about can be any of them.  Static because this
                 * is 1 KiB and the render thread's stack is not ours to
                 * spend. */
                constexpr int kMaxQuestTargets = 64;
                static char questTargets[kMaxQuestTargets][16];
                /* questType of the quest each target came from.  2 ItemDelivery
                 * and 3 Monster are the two the readout is most likely to
                 * show; 6 Fishing and 9 Resource are the other two that can
                 * name a villager. */
                static unsigned char questTargetOff[kMaxQuestTargets];
                int questTargetCount = 0;
                int questCount = 0;
                int netCount = -1;          /* NetList.count, -1 = unreadable */
                int questShapeOk = 0;       /* elements that really were Quests */
                int questGatePass = 0;      /* quests that survived the gate */
                [[maybe_unused]] int questProbe = -1; /* which spelling found the list */
                /* v119: why an element was dropped by unwrapQuest, reported in
                 * row 1 in place of `questProbe` (which has read 6 for many
                 * versions and no longer says anything).
                 *
                 *   0  nothing dropped
                 *   1  the element itself was not a readable pointer
                 *   2  element+0x68 was null / unreadable
                 *   3  it is a Quest by neither the id test nor the
                 *      NetBool/questType test
                 *
                 * v118 could only report questPtrCount < questCount, which says
                 * an element vanished but not why. */
                int questDropCode = 0;
                unsigned questFlags = 0;
                /* v113: these bits are now aggregated over the whole log, not
                 * taken from one quest.
                 *
                 *   0x01  some quest read accepted   = true
                 *   0x02  some quest read dailyQuest = true
                 *   0x04  some quest read completed  = true
                 *   0x08  some quest had all three readable  (gate is trusted)
                 *   0x10  some quest passed the gate
                 *   0x20  some quest dropped: accepted false
                 *   0x40  some quest dropped: dailyQuest false
                 *   0x80  some quest dropped: completed true
                 *
                 * v110 reported the three bools of "the first readable quest"
                 * and got all false, which says nothing: that quest may simply
                 * not have been the daily one.  Aggregating over the log is
                 * the only way one screenshot can answer "is this quest
                 * active" for every quest at once. */
                unsigned questBoolBits = 0;
                /* Quest.accepted / dailyQuest / completed for the first four
                 * quests, one nibble each, high nibble = quest 0:
                 *   bit0 accepted, bit1 dailyQuest, bit2 completed,
                 *   bit3 all three readable.
                 * A nibble of 0xB is therefore "active daily quest", 0xE is
                 * "readable story quest", 0x0 is "nothing readable". */
                /* v115: one bit per quest (0..7) instead of a nibble, because
                 * the nibble form only covered four quests and v114's
                 * before/after readout had the newly accepted quest land at
                 * index 4 -- exactly the one slot the nibbles could not show.
                 *
                 *   accepted / dailyQuest / completed as read, plus
                 *   hasTarget: did this quest contribute at least one target.
                 * hasTarget is what makes the readout say *which* quest is
                 * responsible for a badge. */
                [[maybe_unused]] unsigned questAccMask = 0;
                [[maybe_unused]] unsigned questDailyMask = 0;
                unsigned questDoneMask = 0;
                unsigned questHitMask = 0;
                /* v116: one nibble per quest (0..7), saturating.
                 *   questIdLenNib  length of the quest id
                 *   questTypeNib   questType (v117), 0xF = unreadable
                 * A Help Wanted quest shows daysLeft 1 or 2 and a three-digit
                 * id; a story quest shows 0 and a one- or two-character id. */
                unsigned questIdLenNib = 0;
                /* v117: questType per quest, 0..10 or 0xF.  This is the field
                 * that picks the target offset, so the readout has to say what
                 * it holds for the first eight quests -- one screenshot then
                 * answers both "did the type read" and "is the newly accepted
                 * quest a type that even has a villager target". */
                unsigned questTypeNib = 0;
                /* Per-quest questType, parallel to questPtr[], so pass 2 can
                 * dispatch without re-reading the object. */
                static unsigned char questTypes[64];
                /* First four ASCII characters of the first quest `id` (+0x60)
                 * we could read.  Daily quests carry a numeric id ("128");
                 * Data/Quests story entries carry their own key, so this says
                 * which kind of quest the log actually holds. */
                [[maybe_unused]] unsigned questIdChars = 0;
                /* Kept for the next round: the id *content* is what tells a
                 * daily quest ("128") from a story entry ("1"), but row 2 is
                 * full of the shape checks that decide whether these objects
                 * are Quests at all, which has to come first. */
                /* v114: diagnostics that have to run *before* the gate.
                 *
                 * v113 read the id and calibrated the NetString MethodTable
                 * inside pass 2, after the gate -- so when the gate rejected
                 * every quest (which is exactly what v113's first hardware
                 * readout showed) row 2 came back 00000000 and we learned
                 * nothing about whether those elements are real Quests at all.
                 *
                 * Now:
                 *   questIdLen0   id length of the first quest (-1 unreadable)
                 *   questIdOk     quests whose id is non-empty
                 *   questUnwrap0  how the first element unwrapped: 1 = via
                 *                 NetRef +0x68, 2 = the element itself
                 *   questLayout   1 = the three NetBool wrappers of quest 0
                 *                 share one MethodTable (which is what a real
                 *                 Quest looks like), 2 = they differ,
                 *                 3 = unreadable, 0 = no quest
                 *   questProbeOk  MT-verified name-shaped targets found across
                 *                 *all* quests, gate ignored.  If this is
                 *                 non-zero while targets is 0, the structural
                 *                 filter works and the bool gate is the only
                 *                 thing blocking. */
                [[maybe_unused]] int questIdLen0 = -1;
                [[maybe_unused]] int questIdOk = 0;
                [[maybe_unused]] int questUnwrap0 = 0;
                [[maybe_unused]] int questLayout = 0;
                [[maybe_unused]] int questProbeOk = 0;
                /* MethodTable of a NetString, calibrated from Quest.id.
                 *
                 * This is the structural answer to the garbage targets.  A
                 * plain Quest is 0xA8 bytes, so probing +0xA8 on one reads the
                 * *next heap object* -- and if that object starts with a
                 * MethodTable, `netString` happily treats the word at
                 * MethodTable+0x68 as a string.  v112's target 0 came back as
                 * "Codi", which is exactly what that looks like.
                 *
                 * Every one of `target`, `targetMessage`, `npcName`,
                 * `whereToGo` and `id` is a NetString in the AOT metadata, so
                 * they all carry the same MethodTable.  Calibrating it once
                 * from `id` turns "is this field a string?" into a one-word
                 * compare, and a field that is not a NetString cannot inject a
                 * name. */
                void* netStringMt = nullptr;
                int questMtReject = 0;      /* probes refused by that compare */
                /* Quests that survived unwrapping, with their bool nibble, so
                 * the gate can be decided for the whole log before any target
                 * is extracted.  Static: 64 * 9 bytes, and the render thread's
                 * stack is not ours to spend. */
                static void* questPtr[64];
                static unsigned char questBools[64];
                static unsigned char questIdx[64];
                int questPtrCount = 0;
                {
                    auto netBool = [&](void* owner, uintptr_t off, bool* out)
                        -> bool {
                        if (!Plausible(owner) ||
                            !readableRange(
                                reinterpret_cast<unsigned char*>(owner) + off,
                                8u)) {
                            return false;
                        }
                        void* wrapper = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(owner) + off);
                        if (!Plausible(wrapper) ||
                            !readableRange(
                                reinterpret_cast<unsigned char*>(wrapper) +
                                    0x68, 1u)) {
                            return false;
                        }
                        *out = *reinterpret_cast<unsigned char*>(
                            reinterpret_cast<unsigned char*>(wrapper) + 0x68)
                            != 0;
                        return true;
                    };
                    /* NetInt at `off`: wrapper at the offset, int32 payload at
                     * +0x68.  Same NetField shape as NetBool, just four bytes
                     * wide. */
                    auto netInt = [&](void* owner, uintptr_t off,
                                      int* out) -> bool {
                        if (!Plausible(owner) ||
                            !readableRange(
                                reinterpret_cast<unsigned char*>(owner) + off,
                                8u)) {
                            return false;
                        }
                        void* wrapper = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(owner) + off);
                        if (wrapper == nullptr || !Plausible(wrapper) ||
                            !readableRange(
                                reinterpret_cast<unsigned char*>(wrapper) +
                                    0x68, 4u)) {
                            return false;
                        }
                        *out = *reinterpret_cast<const int*>(
                            reinterpret_cast<const unsigned char*>(wrapper) +
                            0x68);
                        return true;
                    };
                    /* NetString at `off` -> managed string -> ASCII.
                     *
                     * Two shapes are tried: the field holds a NetString
                     * wrapper (payload at +0x68), or it holds the managed
                     * string itself.  The second is only a fallback because a
                     * wrong offset then has to survive NameToAscii's length
                     * and content checks, and the caller still requires an
                     * exact NPC-name match -- so it cannot invent a badge. */
                    auto netString = [&](void* owner, uintptr_t off, char* out,
                                         int cap) -> bool {
                        if (!Plausible(owner) ||
                            !readableRange(
                                reinterpret_cast<unsigned char*>(owner) + off,
                                8u)) {
                            return false;
                        }
                        void* field = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(owner) + off);
                        if (!Plausible(field)) {
                            return false;
                        }
                        if (readableRange(field, 0x70u)) {
                            void* str = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(field) +
                                0x68);
                            if (NameToAscii(str, out, cap)) {
                                return true;
                            }
                        }
                        return NameToAscii(field, out, cap);
                    };
                    /* Kept for reference: the original wrapper-only read. */
                    [[maybe_unused]] auto netStringStrict = [&](void* owner,
                                                               uintptr_t off,
                                                               char* out,
                                                               int cap) -> bool {
                        if (!Plausible(owner) ||
                            !readableRange(
                                reinterpret_cast<unsigned char*>(owner) + off,
                                8u)) {
                            return false;
                        }
                        void* wrapper = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(owner) + off);
                        if (!Plausible(wrapper) ||
                            !readableRange(
                                reinterpret_cast<unsigned char*>(wrapper) +
                                    0x70, 8u)) {
                            return false;
                        }
                        void* str = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(wrapper) + 0x68);
                        return NameToAscii(str, out, cap);
                    };

                    void* farmer = nullptr;
                    if (readableRange(
                            reinterpret_cast<const void*>(
                                g_base + kPlayerSlotOff), 8u)) {
                        farmer = *reinterpret_cast<void**>(
                            g_base + kPlayerSlotOff);
                    }
                    if (!Plausible(farmer)) {
                        farmer = getPlayer(nullptr);
                    }
                    if (Plausible(farmer) &&
                        readableRange(farmer, kQuestLogOff + 8)) {
                        questFlags |= 0x80u;
                    }

                    void** quests = nullptr;
                    if (farmer != nullptr) {
                        void* questLog = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(farmer) +
                            kQuestLogOff);
                        if (Plausible(questLog) &&
                            readableRange(questLog, 0x60u)) {
                            questFlags |= 0x40u;

                            /* NetList.count, used to check whichever List we
                             * land on.  When a quest is accepted this is >= 1
                             * and the mismatch catches a wrong hop. */
                            void* countWrap = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(questLog) +
                                0x50);
                            if (Plausible(countWrap) &&
                                readableRange(
                                    reinterpret_cast<unsigned char*>(countWrap) +
                                        0x68, 4u)) {
                                const int c = *reinterpret_cast<int*>(
                                    reinterpret_cast<unsigned char*>(countWrap) +
                                    0x68);
                                if (c >= 0 && c <= 4096) {
                                    netCount = c;
                                    questFlags |= 0x20u;
                                }
                            }

                            /* Preferred source of elements: the game's own
                             * accessor.
                             *
                             * Every attempt to read the list by hand has hit
                             * the same wall -- the object behind
                             * `questLog+0x58 -> +0x68 -> +0x58` reports 10
                             * elements while NetList.count reports 5, and only
                             * four of those ten answer the Quest probe.  Rather
                             * than keep guessing which of those numbers is the
                             * truth, this asks the game.
                             *
                             * `Farmer.hasQuest` (0x13dfee0) indexes the log
                             * exactly like this:
                             *
                             *   ldr x8, [x20, #0x1b0]   ; questLog
                             *   ldr x9, [x8]            ; MethodTable
                             *   ldr x9, [x9, #8]        ; vtable
                             *   ldr x10,[x9, #0x200]    ; get_Item
                             *   ldrb w9,[x9, #0x208]    ; interface adjustor
                             *   add x0, x8, x9          ; this
                             *   blr x10
                             *   ldr x21,[x0, #0x68]     ; NetRef payload
                             *
                             * The call is safe by construction: the index is
                             * bounded by the same count the game itself bounds
                             * it with, so get_Item's bounds check cannot throw.
                             * (Calling it out of bounds is what HANDOFF warns
                             * about; that is not what this does.)  Managed
                             * calls from this hook already work -- get_Tile,
                             * get_player and get_locations are all called the
                             * same way. */
                            {
                                void* mt = *reinterpret_cast<void**>(questLog);
                                if (netCount > 0 && netCount <= 128 &&
                                    Plausible(mt) &&
                                    readableRange(mt, 0x10u)) {
                                    void* vt = *reinterpret_cast<void**>(
                                        reinterpret_cast<unsigned char*>(mt) +
                                        8);
                                    if (Plausible(vt) &&
                                        readableRange(vt, 0x210u)) {
                                        void* fn =
                                            *reinterpret_cast<void**>(
                                                reinterpret_cast<unsigned char*>(
                                                    vt) +
                                                0x200);
                                        const unsigned char adj =
                                            *reinterpret_cast<unsigned char*>(
                                                reinterpret_cast<unsigned char*>(
                                                    vt) +
                                                0x208);
                                        if (Plausible(fn)) {
                                            auto getItem =
                                                reinterpret_cast<void* (*)(
                                                    void*, int)>(fn);
                                            void* self =
                                                reinterpret_cast<unsigned char*>(
                                                    questLog) + adj;
                                            static void* accQuests[128];
                                            for (int i = 0; i < netCount;
                                                 i++) {
                                                accQuests[i] = getItem(self, i);
                                            }
                                            quests = accQuests;
                                            questCount = netCount;
                                            questProbe = 6;
                                            questFlags |= 0x10u | 0x01u;
                                        }
                                    }
                                }
                            }

                            /* Three candidate spellings of "the List behind
                             * the NetList": +0x58 straight (characters
                             * style), one hop, two hops (what questLog
                             * actually needs).  Only used when the accessor
                             * above could not be resolved. */
                            void* cand[3] = {nullptr, nullptr, nullptr};
                            void* arrWrap = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(questLog) +
                                0x58);
                            if (Plausible(arrWrap) &&
                                readableRange(arrWrap, 0x70u)) {
                                cand[0] = arrWrap;
                                void* arrVal = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(arrWrap) +
                                    0x68);
                                if (Plausible(arrVal) &&
                                    readableRange(arrVal, 0x60u)) {
                                    cand[1] = arrVal;
                                    void* inner = *reinterpret_cast<void**>(
                                        reinterpret_cast<unsigned char*>(
                                            arrVal) +
                                        0x58);
                                    if (Plausible(inner)) {
                                        cand[2] = inner;
                                    }
                                }
                            }

                            /* A List that is empty has no element buffer to
                             * validate -- its _items is the shared empty
                             * array, whose capacity pointer is not readable --
                             * so readVerifiedList rejects it.  For the quest
                             * log "empty" is a meaningful, expected answer
                             * (no quest accepted), so n == 0 is accepted on
                             * the strength of the offset alone. */
                            auto readListLoose = [&](void* lst, void*** out,
                                                     int* count) -> bool {
                                if (!Plausible(lst) ||
                                    !readableRange(lst, 0x20u)) {
                                    return false;
                                }
                                const int n = *reinterpret_cast<int*>(
                                    reinterpret_cast<unsigned char*>(lst) +
                                    0x18);
                                if (n < 0 || n > 4096) {
                                    return false;
                                }
                                if (n == 0) {
                                    *out = nullptr;
                                    *count = 0;
                                    return true;
                                }
                                return readVerifiedList(lst, out, count);
                            };

                            /* Probe ids, reported in row 4 so a failure says
                             * *where* it failed:
                             *   0  known chain (two hops, what the game does)
                             *   1  +0x58 straight  2  one hop
                             *   3  scan, the field itself
                             *   4  scan, one hop   5  scan, two hops */
                            auto tryList = [&](void* cand, unsigned id,
                                               bool strict) -> bool {
                                if (!Plausible(cand)) {
                                    return false;
                                }
                                void** items = nullptr;
                                int n = 0;
                                if (!readListLoose(cand, &items, &n)) {
                                    return false;
                                }
                                /* The element count is the test.
                                 *
                                 * v108 read 5 from the count accessor and 10
                                 * elements from this list, kept the list
                                 * because it was "the chain the game uses",
                                 * and badged five villagers that had nothing
                                 * to do with the player's quests.  The three
                                 * game functions that walk this log all bound
                                 * their loop by `*(int*)(questLog+0x50+0x68)`
                                 * and then index the list, so the two cannot
                                 * legitimately disagree: a list of a different
                                 * size is a different list, and reading
                                 * NetStrings out of its elements yields
                                 * whatever those objects happen to hold.
                                 *
                                 * Pass one is strict: only an exact match is
                                 * accepted.  Pass two (only reached when
                                 * nothing matched) allows a mismatch, because
                                 * NetList.count is a mirrored field and could
                                 * in principle lag behind the list -- but it is
                                 * then required to prove itself: every element
                                 * has to have a Quest shape, checked right
                                 * after this block.  A list of villagers, of
                                 * item ids or of anything else cannot pass
                                 * that, so the lenient pass cannot reintroduce
                                 * v108's failure.
                                 *
                                 * When the count itself cannot be read
                                 * (netCount < 0) there is nothing to compare
                                 * against and the candidate is accepted on the
                                 * element check alone. */
                                if (netCount >= 0 && n != netCount) {
                                    questFlags |= 0x08u;
                                    /* An empty list where the count says
                                     * otherwise is never the right object, so
                                     * even the lenient pass rejects it --
                                     * otherwise it would win the pass and stop
                                     * the scan from finding the real list. */
                                    if (strict || n == 0) {
                                        return false;
                                    }
                                    questFlags |= 0x04u;
                                }
                                quests = items;
                                questCount = n;
                                questProbe = static_cast<int>(id);
                                questFlags |= 0x10u;
                                return true;
                            };

                            /* cand[0] is +0x58 straight, cand[1] one hop,
                             * cand[2] two hops -- the last one is what the
                             * game does, so it is tried first. */
                            static const unsigned kProbeId[3] = {1u, 2u, 0u};
                            bool found = (quests != nullptr);
                            for (int k = 2; k >= 0 && !found; k--) {
                                found = tryList(cand[k], kProbeId[k], true);
                            }
                            for (int k = 2; k >= 0 && !found; k--) {
                                found = tryList(cand[k], kProbeId[k], false);
                            }
                            /* Fallback: walk questLog's own pointer fields.
                             * Costs nothing when the chain above works, and
                             * makes the read independent of how many hops
                             * this build inserts. */
                            if (!found && netCount >= 0) {
                                auto* qb =
                                    reinterpret_cast<unsigned char*>(questLog);
                                for (uintptr_t off = 0; off <= 0x100 &&
                                     !found; off += 8) {
                                    if (!readableRange(qb + off, 8u)) {
                                        continue;
                                    }
                                    void* p = *reinterpret_cast<void**>(
                                        qb + off);
                                    if (!Plausible(p)) {
                                        continue;
                                    }
                                    if (tryList(p, 3u, false)) {
                                        found = true;
                                        break;
                                    }
                                    if (!readableRange(p, 0x70u)) {
                                        continue;
                                    }
                                    void* v = *reinterpret_cast<void**>(
                                        reinterpret_cast<unsigned char*>(p) +
                                        0x68);
                                    if (tryList(v, 4u, false)) {
                                        found = true;
                                        break;
                                    }
                                    if (!Plausible(v) ||
                                        !readableRange(v, 0x60u)) {
                                        continue;
                                    }
                                    void* w = *reinterpret_cast<void**>(
                                        reinterpret_cast<unsigned char*>(v) +
                                        0x58);
                                    if (tryList(w, 5u, false)) {
                                        found = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (quests != nullptr) {
                        /* A Quest is recognisable by shape: `id` is a
                         * NetString at +0x60 (AOT factory), so the wrapper is
                         * a pointer and its +0x68 payload is a managed string
                         * with a sane length.  Elements arrive as
                         * NetRef<Quest>, so the payload at +0x68 is checked
                         * first and the raw element second -- this also keeps
                         * working if the list ever stores Quests directly. */
                        auto questShape = [&](void* p) -> bool {
                            if (!Plausible(p) || !readableRange(p, 0xA8u)) {
                                return false;
                            }
                            void* idWrap = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(p) + 0x60);
                            if (!Plausible(idWrap) ||
                                !readableRange(idWrap, 0x70u)) {
                                return false;
                            }
                            void* idStr = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(idWrap) +
                                0x68);
                            /* The id is a NetString, so its payload is a
                             * managed string -- but an *empty* one is still a
                             * Quest.  Requiring a non-empty id is what made
                             * v109 count only 4 of the 5 quests in the log
                             * (row 4 showed shape ok = 4 against a count of
                             * 5), and the missing one was the only quest that
                             * mattered. */
                            if (!Plausible(idStr) ||
                                !readableRange(idStr, 0x14u)) {
                                return false;
                            }
                            const int len = *reinterpret_cast<const int*>(
                                reinterpret_cast<const unsigned char*>(idStr) +
                                0x10);
                            if (len < 0 || len > 64) {
                                return false;
                            }
                            return len == 0 ||
                                readableRange(
                                    reinterpret_cast<const unsigned char*>(
                                        idStr) + 0x14,
                                    static_cast<size_t>(len) * 2u);
                        };

                        /* Gate for "this string could be a villager's name".
                         *
                         * The badge is decided by exact string compare against
                         * NPC names, so a string that is not a name can only
                         * ever sit in the pool unused -- but v108's readout
                         * showed the pool filling with things that were not
                         * names at all (first characters 'C', ' ', '(' and
                         * 'H' for five quests), which is the signature of a
                         * wrong field offset.  Rather than carry that noise
                         * forward, a candidate has to look like the internal
                         * name the game uses: capitalised, 2..15 characters,
                         * no punctuation or brackets, no trailing space. */
                        auto looksLikeNpcName = [](const char* s) -> bool {
                            if (s == nullptr || s[0] == '\0') {
                                return false;
                            }
                            int n = 0;
                            for (; s[n] != '\0'; n++) {
                                const unsigned char c =
                                    static_cast<unsigned char>(s[n]);
                                if (c < 0x20 || c > 0x7E) {
                                    return false;
                                }
                                switch (c) {
                                case '(': case ')': case '[': case ']':
                                case '{': case '}': case '?': case '!':
                                case ',': case ':': case ';': case '%':
                                case '/': case '\\': case '#': case '"':
                                case '\'': case '+': case '=': case '@':
                                case '&': case '*': case '<': case '>':
                                case '|': case '~': case '^': case '$':
                                    return false;
                                default:
                                    break;
                                }
                                if (n == 0 && !(c >= 'A' && c <= 'Z')) {
                                    return false;
                                }
                            }
                            if (n < 2 || n > 15) {
                                return false;
                            }
                            return s[n - 1] != ' ';
                        };

                        /* Prove the list before trusting a single element.
                         *
                         * This is what makes the lenient pass safe.  A list
                         * whose size disagreed with NetList.count is only
                         * kept if its elements really are Quests -- and since
                         * a Quest is identified by a readable `id` NetString
                         * at +0x60, a list of villagers, of items or of
                         * anything else fails here and contributes no target
                         * at all.  v108 had no such check, which is how five
                         * villagers ended up wearing a quest icon.
                         *
                         * What get_Item returns is settled by Farmer.hasQuest
                         * (0x13dfee0), which walks the log exactly the way
                         * this code does and then reads the *id* straight off
                         * the result:
                         *
                         *   ldr x8, [x20, #0x1b0]   ; questLog
                         *   ldr x8, [x8,  #0x50]    ; count wrapper
                         *   ldr w21,[x8,  #0x68]    ; NetList.count
                         *   ldr x9, [x8]            ; MethodTable
                         *   ldr x9, [x9,  #8]       ; vtable
                         *   ldr x10,[x9,  #0x200]   ; get_Item
                         *   ldrb w9,[x9,  #0x208]   ; interface adjustor
                         *   add x0, x8, x9
                         *   blr x10
                         *   ldr x8, [x0,  #0x60]    ; Quest.id   <- not +0x68
                         *   ldr x0, [x8,  #0x68]    ; NetString payload
                         *   bl  0x3942f0            ; string.Equals
                         *
                         * So the element *is* a Quest, not a NetRef<Quest>:
                         * the +0x68 payload this code used to test first is
                         * Quest.moneyReward, a NetInt wrapper.  The element is
                         * therefore tested before its payload now, and when
                         * the elements came from that accessor no shape test is
                         * needed at all.
                         *
                         * The same function also settles the loop bound: it
                         * walks count-1 .. 0 and dereferences every element
                         * without a null check, so every index below
                         * NetList.count really does hold a Quest.  A shortfall
                         * in ptrCount is this code's fault, never the data's. */
                        /* Length of the managed string reachable from
                         * `owner[off]`, whether that field holds a NetString
                         * wrapper (+0x68 payload) or a plain string.
                         * -1 = unreadable. */
                        auto strLenAt = [&](void* owner, uintptr_t off) -> int {
                            if (!Plausible(owner) ||
                                !readableRange(
                                    reinterpret_cast<unsigned char*>(owner) +
                                        off,
                                    8u)) {
                                return -1;
                            }
                            void* w = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(owner) + off);
                            if (w == nullptr || !Plausible(w)) {
                                return -1;
                            }
                            const unsigned char* s = nullptr;
                            if (readableRange(w, 0x70u)) {
                                void* payload = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(w) + 0x68);
                                if (payload != nullptr && Plausible(payload) &&
                                    readableRange(payload, 0x14u)) {
                                    s = reinterpret_cast<const unsigned char*>(
                                        payload);
                                }
                            }
                            if (s == nullptr && readableRange(w, 0x14u)) {
                                s = reinterpret_cast<const unsigned char*>(w);
                            }
                            if (s == nullptr) {
                                return -1;
                            }
                            const int len = *reinterpret_cast<const int*>(
                                s + 0x10);
                            return (len < 0 || len > 4096) ? -1 : len;
                        };

                        /* A Quest, identified without touching `id`.
                         *
                         * The AOT metadata gives Quest a fingerprint that does
                         * not depend on any field being *filled in*, only on
                         * the object having been constructed:
                         *
                         *   +0x30 +0x38 +0x40 +0x48 +0x50 +0x58   six NetBool
                         *       (type 0x476df0), all sharing one MethodTable
                         *   +0x68 +0x70 +0x78                     three NetInt
                         *       (type 0x4a7770), sharing a *different* MT
                         *
                         * Nine pointers, in two same-type groups, is not
                         * something heap garbage or a NetField wrapper can
                         * imitate.  v119 used only the first three bools, which
                         * left one hole: when the list element is a bare Quest
                         * (the path v114 measured), `element+0x68` is
                         * `Quest.moneyReward` -- a NetInt wrapper -- and the
                         * three-bool test was run on *that* before the real
                         * Quest was tried.  Six bools plus the int group make
                         * the two cases impossible to confuse, so the branch
                         * order no longer carries any risk.
                         *
                         * This exists because the id-based check rejects a
                         * freshly accepted quest: v118's readout had
                         * questPtrCount 4 against questCount 5, and the missing
                         * element was exactly the quest the player had just
                         * accepted.  Whatever its `id` looks like at that
                         * moment, its `questType` and `target` are readable,
                         * and those are the only two fields this feature
                         * needs. */
                        auto positiveShape = [&](void* p) -> bool {
                            if (!Plausible(p) || !readableRange(p, 0xA8u)) {
                                return false;
                            }
                            auto mtOf = [&](uintptr_t off, void** out) -> bool {
                                if (!readableRange(
                                        reinterpret_cast<unsigned char*>(p) +
                                            off, 8u)) {
                                    return false;
                                }
                                void* w = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(p) + off);
                                if (w == nullptr || !Plausible(w) ||
                                    !readableRange(w, 8u)) {
                                    return false;
                                }
                                void* mt = *reinterpret_cast<void**>(w);
                                if (mt == nullptr) {
                                    return false;
                                }
                                *out = mt;
                                return true;
                            };
                            static const uintptr_t kBoolOffs[6] = {
                                0x30u, 0x38u, 0x40u, 0x48u, 0x50u, 0x58u,
                            };
                            static const uintptr_t kIntOffs[3] = {
                                0x68u, 0x70u, 0x78u,
                            };
                            void* boolMt = nullptr;
                            for (int k = 0; k < 6; k++) {
                                void* mt = nullptr;
                                if (!mtOf(kBoolOffs[k], &mt)) {
                                    return false;
                                }
                                if (k == 0) {
                                    boolMt = mt;
                                } else if (mt != boolMt) {
                                    return false;
                                }
                            }
                            void* intMt = nullptr;
                            for (int k = 0; k < 3; k++) {
                                void* mt = nullptr;
                                if (!mtOf(kIntOffs[k], &mt)) {
                                    return false;
                                }
                                if (k == 0) {
                                    intMt = mt;
                                } else if (mt != intMt) {
                                    return false;
                                }
                            }
                            /* NetBool and NetInt are different types; if these
                             * two groups share a MethodTable the layout is not
                             * the one the metadata describes. */
                            if (intMt == boolMt) {
                                return false;
                            }
                            /* The int group is verified, so the questType
                             * payload can be read without another guard. */
                            void* tw = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(p) + 0x70u);
                            const int t = *reinterpret_cast<const int*>(
                                reinterpret_cast<const unsigned char*>(tw) +
                                0x68);
                            return t >= 0 && t < 16;
                        };
                        auto unwrapQuest = [&](void* e, int* how) -> void* {
                            if (how != nullptr) {
                                *how = 0;
                            }
                            if (!Plausible(e)) {
                                questDropCode = 1;
                                return nullptr;
                            }
                            /* Authoritative path first.
                             *
                             * When the elements came from the game's own
                             * get_Item the runtime has already guaranteed the
                             * type: Farmer.hasQuest dereferences every index
                             * below NetList.count without a null check, so an
                             * element that is not a Quest cannot exist there.
                             * Requiring a shape on top of that is what kept
                             * rejecting the freshly accepted quest -- its `id`
                             * NetString is still null at that moment, so both
                             * id tests fail, and v118 measured exactly that
                             * (ptrCount 4 against count 5, with the missing
                             * element being the one the player had just
                             * accepted).
                             *
                             * The only check kept is on the field pass 2
                             * actually needs: questType (+0x70) must read as a
                             * NetInt holding 0..15.  Everything else pass 2
                             * touches is bounds-checked where it is used, so
                             * dropping the shape test here costs no safety. */
                            if (questProbe == 6) {
                                int t = -1;
                                if (readableRange(e, 0x80u) &&
                                    netInt(e, kQuestTypeOff, &t) && t >= 0 &&
                                    t < 16) {
                                    if (how != nullptr) {
                                        *how = 6;
                                    }
                                    return e;
                                }
                                /* Not a Quest after all -- fall through in case
                                 * this accessor returned a wrapper. */
                            }
                            void* inner = nullptr;
                            if (readableRange(e, 0x70u)) {
                                inner = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(e) + 0x68);
                            }
                            /* The element before its +0x68 payload: with this
                             * list the element is the Quest and +0x68 is its
                             * moneyReward. */
                            if (questShape(e)) {
                                if (how != nullptr) {
                                    *how = 2;
                                }
                                return e;
                            }
                            if (questShape(inner)) {
                                if (how != nullptr) {
                                    *how = 1;
                                }
                                return inner;
                            }
                            if (positiveShape(e)) {
                                if (how != nullptr) {
                                    *how = 4;
                                }
                                return e;
                            }
                            if (positiveShape(inner)) {
                                if (how != nullptr) {
                                    *how = 3;
                                }
                                return inner;
                            }
                            /* Nothing accepted it.  Record *why*, because
                             * "an element vanished" and "why it vanished" are
                             * different questions and only the second one says
                             * what to do next.  Reported in row 1, low nibble:
                             *
                             *   1  the element itself is not a readable pointer
                             *   2  element+0x68 is null (an empty list slot)
                             *   3  neither candidate has the Quest fingerprint
                             *      -- the slot holds something that is not a
                             *      Quest at all
                             *   4  ...and `inner` does look like a NetField
                             *      wrapper with a readable string at +0x60, so
                             *      the slot holds a *different* managed object
                             *   5  ...and `e` does, likewise
                             *   6  came from get_Item, but questType did not
                             *      read as a NetInt in 0..15 -- the accessor
                             *      returned something that is not a Quest */
                            if (inner == nullptr) {
                                questDropCode = 2;
                                return nullptr;
                            }
                            questDropCode = questProbe == 6 ? 6 : 3;
                            if (strLenAt(inner, 0x60) >= 0) {
                                questDropCode = 4;
                            } else if (strLenAt(e, 0x60) >= 0) {
                                questDropCode = 5;
                            }
                            return nullptr;
                        };
                        for (int i = 0; i < questCount; i++) {
                            if (unwrapQuest(quests[i], nullptr) != nullptr) {
                                questShapeOk++;
                            }
                        }
                        /* Only the elements that really are Quests are used, and
                         * the rest are skipped -- but a list is no longer
                         * thrown away just because some of its slots are not
                         * Quests.
                         *
                         * v109 rejected the whole list in that case and the
                         * feature went dark: the readout showed the game's own
                         * chain handing back 10 slots of which 4 were Quests,
                         * against a NetList.count of 5.  Ten slots with five
                         * quests in them is the list the log actually uses; the
                         * empty slots are just empty, and vetoing on them
                         * proved nothing.  What must not happen is reading
                         * NetStrings out of a slot that is not a Quest, and
                         * skipping those slots handles it directly.
                         *
                         * A list with no Quest in it at all is still rejected --
                         * that is the case v108's wrong-villager badges came
                         * from, and it must not be drawn from. */
                        if (questShapeOk == 0) {
                            quests = nullptr;
                            questCount = 0;
                            questFlags &= ~0x10u;
                            questFlags |= 0x02u;
                        }

                        /* ---- pass 1: what does each quest say about itself?
                         *
                         * The PC mod's whole rule, from NPCMapLocations
                         * `ModEntry.HasQuest`:
                         *
                         *     quest.accepted && quest.dailyQuest
                         *     && !quest.completed
                         *
                         * Same three NetBools, same offsets: accepted +0x30,
                         * completed +0x38, dailyQuest +0x40, each a wrapper
                         * whose payload sits at +0x68.  `completed` is
                         * confirmed twice over -- the game's own
                         * hasVisibleQuest loop does
                         * `ldr x8,[quest,#0x38]; ldrb w8,[x8,#0x68]`, and
                         * Quest.OnBuildingExists does the same thing -- and
                         * all three fields are the same type (AOT 0x476df0),
                         * so the payload offset is shared.
                         *
                         * v112 enforced only `!completed`, because v110 read
                         * all three as false on one quest and that was taken
                         * as "the bools are broken".  It was not: v110 printed
                         * the *first readable* quest, and in a log with no
                         * quest accepted that is a story quest -- accepted,
                         * not daily, not finished.  Four such quests, each
                         * contributing whatever sits at +0xA8/+0xB0/+0xB8,
                         * are why a villager wore an icon with nothing
                         * outstanding.
                         *
                         * v117 reads all three anyway, and adds questType. */
                        /* v117: the three-offset probe list is gone.  The
                         * offset now comes from `questType` via
                         * kQuestTargetByType, which is the reference mod's
                         * subclass switch and the only thing that makes the
                         * read safe for subclasses smaller than the offset
                         * being probed. */

                        /* MethodTable of whatever `owner[off]` points at.
                         * For a NetField wrapper that is the wrapper's own
                         * type, which is what both the NetString check and the
                         * NetBool layout check compare. */
                        auto wrapperMtAt = [&](void* owner,
                                               uintptr_t off) -> void* {
                            if (!Plausible(owner) ||
                                !readableRange(
                                    reinterpret_cast<unsigned char*>(owner) +
                                        off,
                                    8u)) {
                                return nullptr;
                            }
                            void* w = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(owner) + off);
                            if (w == nullptr || !Plausible(w) ||
                                !readableRange(w, 8u)) {
                                return nullptr;
                            }
                            void* mt = *reinterpret_cast<void**>(w);
                            return Plausible(mt) ? mt : nullptr;
                        };

                        for (int i = 0; i < questCount && i < 64; i++) {
                            int how = 0;
                            void* q = unwrapQuest(quests[i], &how);
                            if (q == nullptr) {
                                continue;
                            }
                            if (questUnwrap0 == 0) {
                                questUnwrap0 = how;
                            }
                            bool accepted = false;
                            bool completed = false;
                            bool daily = false;
                            const bool okAccepted =
                                netBool(q, kQuestAcceptedOff, &accepted);
                            const bool okCompleted =
                                netBool(q, kQuestCompletedOff, &completed);
                            const bool okDaily =
                                netBool(q, kQuestDailyOff, &daily);
                            unsigned bits = 0;
                            if (okAccepted && accepted) {
                                bits |= 0x1u;
                                questBoolBits |= 0x01u;
                            }
                            if (okDaily && daily) {
                                bits |= 0x2u;
                                questBoolBits |= 0x02u;
                            }
                            if (okCompleted && completed) {
                                bits |= 0x4u;
                                questBoolBits |= 0x04u;
                            }
                            if (okAccepted && okCompleted && okDaily) {
                                bits |= 0x8u;
                                questBoolBits |= 0x08u;
                            }
                            /* One bit per quest, quests 0..7.  v114 used a
                             * nibble and only covered four quests; the newly
                             * accepted quest landed on index 4, so the
                             * before/after readouts were identical in exactly
                             * the slot that mattered. */
                            if (i < 8) {
                                const unsigned bit = 1u << static_cast<unsigned>(i);
                                if (bits & 0x1u) {
                                    questAccMask |= bit;
                                }
                                if (bits & 0x2u) {
                                    questDailyMask |= bit;
                                }
                                if (bits & 0x4u) {
                                    questDoneMask |= bit;
                                }
                            }

                            /* The id and the NetString MethodTable, read
                             * before the gate so a rejected log still reports
                             * them.  A real Quest always has an id; if every
                             * element comes back with length 0 or -1 then
                             * these are not Quests and no gate is going to
                             * save us. */
                            const int idLen = strLenAt(q, 0x60);
                            if (questIdLen0 < 0) {
                                questIdLen0 = idLen;
                            }
                            if (idLen > 0) {
                                questIdOk++;
                            }
                            if (i < 8) {
                                const unsigned shift =
                                    28u - static_cast<unsigned>(i) * 4u;
                                questIdLenNib |=
                                    static_cast<unsigned>(
                                        idLen < 0 ? 15 : (idLen > 15 ? 15 : idLen))
                                    << shift;
                            }
                            /* v117: questType.  Unlike `daysLeft` this one is
                             * known to be alive -- the game's own virtual
                             * dispatch reads it -- and it decides where
                             * `target` sits, so pass 2 needs it per quest.
                             * bits 0x10 / 0x20 now mean "type readable" and
                             * "type has a villager target". */
                            int questType = -1;
                            const bool okType = netInt(q, kQuestTypeOff,
                                                       &questType);
                            const bool hasTarget =
                                okType && questType >= 0 &&
                                questType < kQuestTypeCount &&
                                kQuestTargetByType[questType] != kNoQuestTarget;
                            if (okType) {
                                bits |= 0x10u;
                                if (hasTarget) {
                                    bits |= 0x20u;
                                }
                            }
                            if (i < 8) {
                                const unsigned shift =
                                    28u - static_cast<unsigned>(i) * 4u;
                                const unsigned nib = okType
                                    ? static_cast<unsigned>(
                                          questType < 0
                                              ? 15
                                              : (questType > 15 ? 15
                                                                : questType))
                                    : 15u;
                                questTypeNib |= nib << shift;
                            }
                            if (netStringMt == nullptr) {
                                void* mt = wrapperMtAt(q, 0x60);
                                if (mt != nullptr) {
                                    netStringMt = mt;
                                }
                            }
                            /* A Quest puts three NetBool wrappers at +0x30,
                             * +0x38 and +0x40.  All three are the same type,
                             * so all three wrappers must start with the same
                             * MethodTable.  Three different MethodTables mean
                             * this object is not laid out like a Quest, which
                             * is the one thing the lenient `questShape` check
                             * cannot tell us. */
                            if (i == 0 && questLayout == 0) {
                                void* b1 = wrapperMtAt(q, kQuestAcceptedOff);
                                void* b2 = wrapperMtAt(q, kQuestCompletedOff);
                                void* b3 = wrapperMtAt(q, kQuestDailyOff);
                                if (b1 == nullptr || b2 == nullptr ||
                                    b3 == nullptr) {
                                    questLayout = 3;
                                } else if (b1 == b2 && b2 == b3) {
                                    questLayout = 1;
                                } else {
                                    questLayout = 2;
                                }
                            }

                            if (questPtrCount < 64) {
                                questPtr[questPtrCount] = q;
                                questBools[questPtrCount] =
                                    static_cast<unsigned char>(bits);
                                questIdx[questPtrCount] =
                                    static_cast<unsigned char>(i);
                                questTypes[questPtrCount] =
                                    static_cast<unsigned char>(
                                        okType && questType >= 0
                                            ? (questType > 255 ? 255 : questType)
                                            : 255);
                                questPtrCount++;
                            }
                        }

                        /* ---- pass 2: extract targets.
                         *
                         * The gate is `dailyQuest && !completed` plus
                         * "this questType has a villager target", and the
                         * target offset comes from `questType` instead of
                         * being probed.
                         *
                         * v113/v114 required accepted && dailyQuest, which is
                         * what the reference mod does, and two hardware
                         * readouts agree that `accepted` never reads true on
                         * this build:
                         *
                         *   - before accepting a quest: 4 quests, all
                         *     accepted=false, daily=false, completed=false
                         *   - after accepting one: 5 quests, still all false
                         *
                         * The three fields are the same NetBool type -- row 2
                         * byte 0 reported that their wrappers share one
                         * MethodTable -- and `completed` is proven correct by
                         * the game's own hasVisibleQuests loop, so the payload
                         * offset is right for all three.  Gating on `accepted`
                         * only silences the feature.
                         *
                         * v116 then tried `daysLeft > 0` as the daily/story
                         * test and it read 0 for every quest both before and
                         * after accepting one.  The read itself was correct
                         * (Quest.GetDaysLeft at 0x1a7a8e0 is exactly
                         * `ldr x8,[x0,#0x78]; ldr w0,[x8,#0x68]`), so that
                         * idea is dead too.
                         *
                         * What v115 actually got wrong was not the gate, it was
                         * the *offset*: target 0 "Coop" is
                         * GoSomewhereQuest.whereToGo -- a place -- and target 1
                         * "Hodi" came out of +0xB0 on a 0xB0-byte
                         * GoSomewhereQuest, i.e. the object after it on the
                         * heap.  Probing all three offsets on every quest is
                         * what let both through.
                         *
                         * `questType` (+0x70, NetInt) removes the guesswork:
                         * the reference mod switches on the subclass to decide
                         * where `target` lives, and kQuestTargetByType is that
                         * switch as a table.  A quest whose type has no
                         * villager target contributes nothing, so no place name
                         * and no out-of-bounds read can become a badge.
                         *
                         * v122 restores `dailyQuest` as the gate.  The
                         * "dead field" conclusion below was wrong: one function
                         * at 0x1722b14..0x1722bac writes dailyQuest, accepted,
                         * canBeCancelled and daysLeft = 2 in a row, on the
                         * daily quest it has just created.  Story quests never
                         * pass through it, so they read false -- which is what
                         * "always false" actually meant. */
                        for (int i = 0; i < questPtrCount; i++) {
                            void* q = questPtr[i];
                            const unsigned bits = questBools[i];
                            const int qtype = questTypes[i] == 255
                                ? -1
                                : static_cast<int>(questTypes[i]);
                            const bool typeHasTarget = (bits & 0x20u) != 0;
                            /* The gate is the reference mod's gate:
                             *
                             *     accepted && dailyQuest && !completed
                             *
                             * with `accepted` dropped because the game writes
                             * it in the same block as `dailyQuest` (see below),
                             * so requiring both would only add a way to lose a
                             * quest, never a way to gain one.
                             *
                             * `dailyQuest` is what separates a Help Wanted
                             * board quest from a story quest, and that
                             * distinction is the whole point: v121 drew a
                             * badge for "Jodi's Request" -- a fixed story
                             * quest with no deadline -- which the PC mod never
                             * does.
                             *
                             * The three fields are NOT dead, which is what this
                             * code wrongly concluded in v114/v116.  One
                             * function writes all of them, in this order, on
                             * the quest object it just built:
                             *
                             *   0x1722b14  +0x40 dailyQuest      = 1
                             *   0x1722b38  +0x80 dayQuestAccepted = today
                             *   0x1722b64  +0x30 accepted         = 1
                             *   0x1722b88  +0x50 canBeCancelled   = 1
                             *   0x1722bac  +0x78 daysLeft         = 2
                             *
                             * That is the daily-quest setup, and it is the only
                             * place in the whole 130 MB text segment that writes
                             * daysLeft = 2.  Story quests never go through it,
                             * so they read false/0 for all of these -- which is
                             * exactly what the readouts showed.  "Never true"
                             * was really "no daily quest in this save". */
                            const bool use = (bits & 0x2u) != 0 &&
                                (bits & 0x4u) == 0 && typeHasTarget;
                            if (!use) {
                                questBoolBits |= 0x80u;
                            } else {
                                questBoolBits |= 0x10u;
                                if (questGatePass < 15) {
                                    questGatePass++;
                                }
                            }

                            /* First four characters of the first id we can
                             * read, for the readout only. */
                            if (questIdChars == 0) {
                                char idbuf[16];
                                if (netString(q, 0x60, idbuf, 16)) {
                                    unsigned v = 0;
                                    for (int c = 0;
                                         c < 4 && idbuf[c] != '\0'; c++) {
                                        v |= static_cast<unsigned>(
                                                 static_cast<unsigned char>(
                                                     idbuf[c]))
                                            << (24 - c * 8);
                                    }
                                    questIdChars = v;
                                }
                            }

                            /* One offset, chosen by type.  The three
                             * target-bearing subclasses do not agree on where
                             * `target` sits (0xA8 / 0xB0 / 0xB8) and every one
                             * of those offsets means something else in the
                             * other subclasses, so reading more than one of
                             * them is reading the wrong field by construction.
                             *
                             * The MethodTable compare stays: every field that
                             * can legitimately hold a name is a NetString
                             * (0x4e0e50, the same type as `id`), while
                             * `targetMessage` -- which sits at 0xA8 in
                             * ItemDeliveryQuest -- is a plain managed string
                             * (0x9c8590).  Calibrating that one MethodTable
                             * from `id` makes "is this field a string?" a
                             * single word compare, and it also rejects the
                             * heap-neighbour read if the object size ever
                             * disagrees with the table.
                             *
                             * One exception, checked against the metadata and
                             * harmless here: ResourceCollectionQuest.targetMessage
                             * at 0xB0 is itself a NetString (0x4e0e50), so the
                             * MethodTable compare would *not* reject it.  That
                             * offset is never read for type 10 -- the table
                             * sends it to 0xA8 -- and the type dispatch is what
                             * keeps it out, which is the reason the dispatch has
                             * to be right rather than merely plausible. */
                            const int k = (qtype >= 0 && qtype < kQuestTypeCount)
                                ? kQuestTargetByType[qtype]
                                : kNoQuestTarget;
                            if (k == kNoQuestTarget) {
                                continue;
                            }
                            const uintptr_t off = static_cast<uintptr_t>(k);
                            if (netStringMt != nullptr) {
                                void* mt = wrapperMtAt(q, off);
                                if (mt == nullptr || mt != netStringMt) {
                                    questMtReject++;
                                    continue;
                                }
                            }
                            char scratch[16];
                            if (use && questTargetCount >= kMaxQuestTargets) {
                                continue;
                            }
                            char* dst = use
                                ? questTargets[questTargetCount]
                                : scratch;
                            if (!netString(q, off, dst, 16)) {
                                continue;
                            }
                            if (!looksLikeNpcName(dst)) {
                                continue;
                            }
                            if (!use) {
                                questProbeOk++;
                                continue;
                            }
                            /* Two quests can name the same villager; keeping
                             * one copy leaves room for the others. */
                            bool dup = false;
                            for (int d = 0; d < questTargetCount; d++) {
                                if (NameMatchCi(dst, questTargets[d])) {
                                    dup = true;
                                    break;
                                }
                            }
                            if (!dup) {
                                questTargetOff[questTargetCount] =
                                    static_cast<unsigned char>(qtype);
                                questTargetCount++;
                                const unsigned qi = questIdx[i];
                                if (qi < 8) {
                                    questHitMask |=
                                        1u << qi;
                                }
                            }
                        }
                    }
                }

                if (readVerifiedList(list, &locations, &locationCount)) {
                    for (int i = 0; i < locationCount; i++) {
                        void* location = locations[i];
                        if (!Plausible(location) ||
                            !readableRange(location, 0xB8u)) {
                            continue;
                        }
                        void* collection = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(location) + 0xB0);
                        if (!Plausible(collection) ||
                            !readableRange(collection, 0x60u)) {
                            continue;
                        }
                        void* innerList = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(collection) + 0x58);
                        void** npcs = nullptr;
                        int n = 0;
                        if (!readVerifiedList(innerList, &npcs, &n) || n <= 0) {
                            continue;
                        }

                        void* netLocName = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(location) + 0x1A8);
                        void* locName = nullptr;
                        if (Plausible(netLocName) &&
                            readableRange(netLocName, 0x70u)) {
                            void* inner = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(netLocName) +
                                0x68);
                            if (Plausible(inner)) {
                                locName = inner;
                            }
                        }
                        if (!Plausible(locName)) {
                            continue;
                        }

                        const WorldMapEntry* entry = nullptr;
                        for (int k = 0; k < kWorldMapCount; k++) {
                            if (NameEquals(locName, kWorldMap[k].name)) {
                                entry = &kWorldMap[k];
                                break;
                            }
                        }
                        bool useCenter = false;
                        if (entry == nullptr) {
                            static const WorldMapEntry* farmEntry = nullptr;
                            if (farmEntry == nullptr) {
                                for (int k = 0; k < kWorldMapCount; k++) {
                                    const char* a = kWorldMap[k].name;
                                    const char* b = "Farm";
                                    while (*a != '\0' && *a == *b) {
                                        ++a;
                                        ++b;
                                    }
                                    if (*a == '\0' && *b == '\0') {
                                        farmEntry = &kWorldMap[k];
                                        break;
                                    }
                                }
                            }
                            if (farmEntry == nullptr) {
                                continue;
                            }
                            entry = farmEntry;
                            useCenter = true;
                            fallback++;
                        }
                        else {
                            matched++;
                        }

                        const float rx = static_cast<float>(entry->rectX) * 4.0f;
                        const float ry = static_cast<float>(entry->rectY) * 4.0f;
                        const float rw = static_cast<float>(entry->rectW) * 4.0f;
                        const float rh = static_cast<float>(entry->rectH) * 4.0f;
                        const float mw = static_cast<float>(entry->mapW);
                        const float mh = static_cast<float>(entry->mapH);
                        if (rw < 1.0f || rh < 1.0f || mw < 1.0f || mh < 1.0f) {
                            continue;
                        }

                        for (int j = 0; j < n && drawn < 256; j++) {
                            void* npc = npcs[j];
                            if (!Plausible(npc) || !readableRange(npc, 0xB0u)) {
                                continue;
                            }
                            seen++;

                            /* Character.get_Tile returns the tile as two floats
                             * and refreshes the cache.  v94 read cachedTile
                             * (+0x18) directly, which is only populated for the
                             * NPCs the game queried this frame -- hence exactly
                             * one villager drawn. */
                            int tx = 0;
                            int ty = 0;
                            const Vec2 t = getTile(npc);
                            if (t.x > 0.0f && t.y > 0.0f && t.x < 512.0f &&
                                t.y < 512.0f) {
                                tx = static_cast<int>(t.x);
                                ty = static_cast<int>(t.y);
                            }
                            else {
                                const float px = *reinterpret_cast<float*>(
                                    reinterpret_cast<unsigned char*>(npc) + 0x30);
                                const float py = *reinterpret_cast<float*>(
                                    reinterpret_cast<unsigned char*>(npc) + 0x34);
                                if (px > 0.0f && py > 0.0f) {
                                    tx = static_cast<int>(px / 64.0f);
                                    ty = static_cast<int>(py / 64.0f);
                                }
                            }
                            if (tx <= 0 || ty <= 0 || tx >= 200 || ty >= 200) {
                                badTile++;
                                continue;
                            }

                            void* netSprite = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(npc) + 0x50);
                            void* sprite = nullptr;
                            if (Plausible(netSprite) &&
                                readableRange(netSprite, 0x70u)) {
                                void* inner = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(netSprite) +
                                    0x68);
                                if (Plausible(inner)) {
                                    sprite = inner;
                                }
                            }
                            void* sheet = nullptr;
                            if (sprite != nullptr && readableRange(sprite, 0x18u)) {
                                void* t = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(sprite) +
                                    0x10);
                                if (Plausible(t) &&
                                    *reinterpret_cast<void**>(t) == mtTex) {
                                    sheet = t;
                                }
                            }
                            if (sheet == nullptr) {
                                noSheet++;
                                continue;
                            }
                            withSheet++;

                            char nm[16];
                            nameAscii(npc, nm, 16);
                            int crop = npccrop::Lookup(nm);
                            if (crop < 0) {
                                crop = 0;
                            }
                            if (crop > 40) {
                                crop = 40;
                            }

                            /* Horse and children: the 16x15 top-left window is
                             * wrong for both, so take the rect that actually
                             * contains them.  See characterKind(). */
                            int sx = 0;
                            int sw = 16;
                            int sh = 15;
                            const int kind = characterKind(npc);
                            if (kind == 2) {
                                /* Horse.reloadSprite builds the sprite as
                                 * AnimatedSprite(..., 0x20, 0x20), so one
                                 * frame is 32x32 and IS the whole horse. */
                                sx = 0;
                                crop = 0;
                                sw = 32;
                                sh = 32;
                            }
                            else if (kind == 1) {
                                /* The game already knows where a child's head
                                 * is: Child.getMugShotSourceRect switches on
                                 * Age and returns (0,0,16,22) / (0,42,24,22) /
                                 * (0,112,16,22) / (0,4,16,24).  Call it rather
                                 * than re-deriving Age from daysOld.  It takes
                                 * `this` in x0 and returns the 16-byte struct
                                 * in x0/x1 -- verified at 0x122A6C0, where the
                                 * body does `mov x19,x0` and ends with
                                 * `ldr x0,[x0]; ldr x1,[x8,#8]`. */
                                struct MugRect {
                                    int x, y, w, h;
                                };
                                auto mug = reinterpret_cast<MugRect (*)(void*)>(
                                    g_base + 0x122A6C0u);
                                const MugRect r = mug(npc);
                                if (r.x >= 0 && r.y >= 0 && r.w > 0 &&
                                    r.h > 0 && r.w <= 64 && r.h <= 64) {
                                    sx = r.x;
                                    crop = r.y;
                                    sw = r.w;
                                    sh = r.h;
                                }
                            }

                            const float px = useCenter
                                ? rx + rw * 0.5f
                                : rx + rw * (static_cast<float>(tx) / mw);
                            const float py = useCenter
                                ? ry + rh * 0.5f
                                : ry + rh * (static_cast<float>(ty) / mh);
                            const int mx = static_cast<int>(
                                static_cast<float>(bounds[0]) + px + kMarkerOffX);
                            const int my = static_cast<int>(
                                static_cast<float>(bounds[1]) + py + kMarkerOffY);

                            /* Collected instead of drawn: in town several
                             * villagers stand within a tile or two of each
                             * other and their heads land on top of one
                             * another.  Drawing is deferred to a second pass
                             * that spreads them first. */
                            if (markerCount < kMaxMarkers) {
                                Marker& m = markers[markerCount++];

                                /* Stage 139: remember which location this
                                 * character came from, so the hover list can
                                 * fall back to exactly the set the player can
                                 * already see on the map.  Recorded only for
                                 * characters that actually get a marker, so
                                 * the list and the avatars never disagree. */
                                if (s_avCount < kMaxAvatarRecords) {
                                    s_avLocPtr[s_avCount] = location;
                                    s_avNpcPtr[s_avCount] = npc;
                                    s_avX[s_avCount] = static_cast<float>(mx);
                                    s_avY[s_avCount] = static_cast<float>(my);
                                    s_avCount++;
                                }

                                m.ax = static_cast<float>(mx);
                                m.ay = static_cast<float>(my);
                                m.x = m.ax;
                                m.y = m.ay;
                                m.sheet = sheet;
                                m.crop = crop;
                                m.sx = sx;
                                m.sw = sw;
                                m.sh = sh;
                                m.bday = 0;
                                int bs = 0;
                                int bd = 0;
                                if (season >= 0 && cursors != nullptr &&
                                    npcbday::Lookup(nm, &bs, &bd) &&
                                    bs == season && bd == dayOfMonth) {
                                    m.bday = 1;
                                    s_bdayDay = dayOfMonth;
                                    bdayCount++;
                                }
                                m.quest = 0;
                                /* A badge needs a name on both sides.  `nm` is
                                 * empty whenever nameAscii could not read this
                                 * NPC, and an empty `nm` used to compare equal
                                 * to an empty target -- see NameMatchCi. */
                                if (nm[0] != '\0') {
                                    for (int q = 0; q < questTargetCount; q++) {
                                        if (NameMatchCi(questTargets[q], nm)) {
                                            m.quest = 1;
                                            badgeCount++;
                                            break;
                                        }
                                    }
                                }
                            }
                            drawn++;
                        }
                    }
                }

                /* Second pass: spread overlapping heads, then draw.
                 *
                 * Villagers standing on the same tile land on the exact same
                 * pixel, and a town block easily puts three or four heads
                 * inside one avatar's width.  Rather than a list of names --
                 * which would need a text renderer we do not have -- the
                 * heads are pushed apart.
                 *
                 * Method: global pairwise relaxation.  Every pair closer than
                 * kAvatarSpread is moved half the shortfall each, along the
                 * line between them, and the whole set is re-examined for
                 * kSpreadIters rounds.  The earlier version bucketed markers
                 * into a half-avatar grid and gave each one a slot on an
                 * eight-way ring; measured offline it moved 76 overlapping
                 * pairs to 75, because a pair in *adjacent* buckets was never
                 * compared and never separated.  Relaxation has no such blind
                 * spot.
                 *
                 * Positions stay relative to the anchor (ax, ay) and are
                 * clamped to kAvatarMaxDrift, so a head can never wander off
                 * its owner's tile by more than half an avatar.  The loop is
                 * pure arithmetic on values already computed -- nothing is
                 * read from the game, and the same positions give the same
                 * result every frame, so nothing shimmers while walking. */
                {
                    if (markerCount > 1) {
                        const float spread2 = kAvatarSpread * kAvatarSpread;
                        for (int it = 0; it < kSpreadIters; it++) {
                            bool moved = false;
                            for (int i = 0; i < markerCount; i++) {
                                for (int j = i + 1; j < markerCount; j++) {
                                    float dx = markers[j].x - markers[i].x;
                                    float dy = markers[j].y - markers[i].y;
                                    const float d2 = dx * dx + dy * dy;
                                    if (d2 >= spread2) {
                                        continue;
                                    }
                                    float d;
                                    if (d2 < 1e-6f) {
                                        /* Exactly on top of each other: no
                                         * direction to derive, so take one
                                         * from the table.  Indexed off both
                                         * markers so a stack of three still
                                         * splits three ways. */
                                        const int slot =
                                            (i * 7 + j) % 12;
                                        dx = kDirX[slot];
                                        dy = kDirY[slot];
                                        d = 0.0f;
                                    } else {
                                        d = SqrtF(d2);
                                        dx /= d;
                                        dy /= d;
                                    }
                                    const float push =
                                        (kAvatarSpread - d) * 0.5f;
                                    markers[i].x -= dx * push;
                                    markers[i].y -= dy * push;
                                    markers[j].x += dx * push;
                                    markers[j].y += dy * push;
                                    moved = true;
                                }
                            }
                            if (!moved) {
                                break;
                            }
                        }
                        const float drift2 =
                            kAvatarMaxDrift * kAvatarMaxDrift;
                        for (int i = 0; i < markerCount; i++) {
                            float dx = markers[i].x - markers[i].ax;
                            float dy = markers[i].y - markers[i].ay;
                            const float d2 = dx * dx + dy * dy;
                            if (d2 > drift2) {
                                const float s = kAvatarMaxDrift / SqrtF(d2);
                                dx *= s;
                                dy *= s;
                            }
                            markers[i].x = markers[i].ax + dx;
                            markers[i].y = markers[i].ay + dy;
                        }
                    }
                    for (int i = 0; i < markerCount; i++) {
                        NullableRect src;
                        src.x = markers[i].sx;
                        src.y = markers[i].crop;
                        src.w = markers[i].sw;
                        src.h = markers[i].sh;
                        src.present = 1;
                        for (int q = 0; q < 7; q++) {
                            src.pad[q] = 0;
                        }
                        const int dx_i =
                            static_cast<int>(markers[i].x) - kAvatarW / 2;
                        const int dy_i =
                            static_cast<int>(markers[i].y) - kAvatarH / 2;
                        const u64 dxy =
                            (static_cast<u64>(static_cast<u32>(dy_i)) << 32) |
                            static_cast<u32>(dx_i);
                        const u64 dwh =
                            (static_cast<u64>(static_cast<u32>(kAvatarH)) << 32) |
                            static_cast<u32>(kAvatarW);
                        drawSprite(batch, markers[i].sheet, dxy, dwh, &src,
                                   0xFFFFFFFFu, 0.0f, 0.0f, 0.0f, 1.0f, 0);

                        /* Birthday badge, pinned to the head's top-right
                         * corner and allowed to overhang it by 2px -- the same
                         * relationship NPC Map Locations uses between its
                         * portrait rect and the icon.  Drawn after the head so
                         * it is never buried by a neighbour's face. */
                        if (markers[i].bday != 0) {
                            NullableRect bs;
                            bs.x = kBadgeSrcX;
                            bs.y = kBadgeSrcY;
                            bs.w = kBadgeSrcW;
                            bs.h = kBadgeSrcH;
                            bs.present = 1;
                            for (int q = 0; q < 7; q++) {
                                bs.pad[q] = 0;
                            }
                            const int bx = dx_i + kAvatarW - kBadgeW +
                                kBadgeOverhang;
                            const int by = dy_i;
                            const u64 bdxy =
                                (static_cast<u64>(static_cast<u32>(by)) << 32) |
                                static_cast<u32>(bx);
                            const u64 bdwh =
                                (static_cast<u64>(static_cast<u32>(kBadgeH))
                                 << 32) |
                                static_cast<u32>(kBadgeW);
                            /* scale stays 1.0: this helper sizes by the
                             * destination rect, so passing 1.8 as well would
                             * scale the icon twice (see the avatar draw above,
                             * which does the same for the 16x15 source). */
                            drawSprite(batch, cursors, bdxy, bdwh, &bs,
                                       0xFFFFFFFFu, 0.0f, 0.0f, 0.0f, 1.0f, 0);
                        }

                        /* Quest badge, on the left so it can never collide
                         * with the gift icon on the right. */
                        if (markers[i].quest != 0) {
                            NullableRect qs;
                            qs.x = kQuestSrcX;
                            qs.y = kQuestSrcY;
                            qs.w = kQuestSrcW;
                            qs.h = kQuestSrcH;
                            qs.present = 1;
                            for (int q = 0; q < 7; q++) {
                                qs.pad[q] = 0;
                            }
                            const int qx = dx_i + kQuestBadgeInset;
                            const int qy = dy_i;
                            const u64 qdxy =
                                (static_cast<u64>(static_cast<u32>(qy)) << 32) |
                                static_cast<u32>(qx);
                            const u64 qdwh =
                                (static_cast<u64>(
                                     static_cast<u32>(kQuestBadgeH)) << 32) |
                                static_cast<u32>(kQuestBadgeW);
                            drawSprite(batch, cursors, qdxy, qdwh, &qs,
                                       0xFFFFFFFFu, 0.0f, 0.0f, 0.0f, 1.0f, 0);
                        }
                    }
                }

                /* Draw probe: the quest icon at a fixed spot near the top-left
                 * of the map, only while diagnostics are on.
                 *
                 * The badge has never been seen on screen, so "no badge" could
                 * equally mean the target name never matched or that the draw
                 * call itself is wrong.  This separates the two: if the icon
                 * shows up here, the source rect and the draw path are fine
                 * and the fault is upstream in the name matching. */
                if (kShowDiagnostics && cursors != nullptr) {
                    NullableRect probe;
                    probe.x = kQuestSrcX;
                    probe.y = kQuestSrcY;
                    probe.w = kQuestSrcW;
                    probe.h = kQuestSrcH;
                    probe.present = 1;
                    for (int q = 0; q < 7; q++) {
                        probe.pad[q] = 0;
                    }
                    const u64 pxy =
                        (static_cast<u64>(static_cast<u32>(bounds[1] + 40))
                         << 32) |
                        static_cast<u32>(bounds[0] + 40);
                    const u64 pwh =
                        (static_cast<u64>(static_cast<u32>(kQuestBadgeH * 2))
                         << 32) |
                        static_cast<u32>(kQuestBadgeW * 2);
                    drawSprite(batch, cursors, pxy, pwh, &probe, 0xFFFFFFFFu,
                               0.0f, 0.0f, 0.0f, 1.0f, 0);
                }

                /* Row order: the quest rows come first.
                 *
                 * Three rounds in a row the interesting lines were the quest
                 * ones, and a partial copy of the readout left them out.  The
                 * order now puts the thing being debugged at the top, so even
                 * one pasted line is enough to say what happened. */

                /* Row 0: the quest log, end to end.
                 *
                 *   [NetList.count][element count][targets][badges]
                 *
                 * `count` is what the game's own accessors loop over and what
                 * bounds the get_Item calls.  `elements` is how many objects
                 * came back from the log.  `targets` is how many strings
                 * survived the offset probe, the name-shape gate and the
                 * "not completed" filter.  `badges` is how many avatars were
                 * actually given the icon this frame -- the only number that
                 * says the feature works. */
                rows[0] = ((netCount < 0 ? 0xFFu
                                         : static_cast<unsigned>(netCount) &
                                               0xFFu)
                           << 24) |
                    ((static_cast<unsigned>(questCount) & 0xFFu) << 16) |
                    ((static_cast<unsigned>(questTargetCount) & 0xFFu) << 8) |
                    (static_cast<unsigned>(badgeCount) & 0xFFu);
                /* Row 1: [flags][bools][gatePass:dropCode][mtReject:shape ok]
                 *
                 * Flags:
                 *   0x80 farmer pointer resolved
                 *   0x40 questLog object readable
                 *   0x20 NetList count readable
                 *   0x10 elements obtained
                 *   0x08 some candidate disagreed with NetList.count
                 *   0x04 ...and one was accepted anyway (lenient pass)
                 *   0x02 the accepted list was thrown out: no Quest in it
                 *   0x01 elements came from the game's own get_Item
                 *
                 * Bools, aggregated over the whole log (v113):
                 *   0x01 some accepted   0x02 some dailyQuest
                 *   0x04 some completed  0x08 some quest had all three
                 *                        readable, i.e. the gate is trusted
                 *   0x10 some quest passed the gate
                 *   0x20 some dropped: accepted false
                 *   0x40 some dropped: dailyQuest false
                 *   0x80 some quest was rejected by the gate in pass 2 --
                 *        either `completed` was true or its type has no
                 *        villager target.  (Not "completed true" alone: the
                 *        type check drops far more quests than `completed`
                 *        ever does, so this bit being set is normal.)
                 *
                 * High nibble of byte 1: how many quests passed the gate.
                 * Low nibble: why the last rejected list element was rejected
                 * (v120; 0 = nothing was rejected).  See unwrapQuest for the
                 * table -- 1 bad element pointer, 2 null element+0x68,
                 * 3 not a Quest at all, 4/5 the slot holds some other managed
                 * object, 6/7 the fingerprint matched yet it was dropped.
                 * This nibble used to hold the list-probe id (6 = the game's
                 * own get_Item), which stopped carrying information once the
                 * accessor path started winning every time.
                 *
                 * Byte 3: low nibble counts elements that really had a Quest
                 * shape, high nibble counts probes refused because the field
                 * was not a NetString (saturating). */
                rows[1] = ((questFlags & 0xFFu) << 24) |
                    ((questBoolBits & 0xFFu) << 16) |
                    (((static_cast<unsigned>(questGatePass > 15
                                                 ? 15
                                                 : questGatePass)
                       << 4) |
                      (static_cast<unsigned>(questDropCode & 0x0Fu)))
                     << 8) |
                    ((static_cast<unsigned>(questMtReject > 15
                                                ? 15
                                                : questMtReject)
                      << 4) |
                     (static_cast<unsigned>(questShapeOk > 15
                                                ? 15
                                                : questShapeOk)
                      & 0x0Fu));

                auto chars4 = [](const char* s) -> unsigned {
                    unsigned v = 0;
                    for (int c = 0; c < 4; c++) {
                        const unsigned char ch =
                            static_cast<unsigned char>(s[c]);
                        if (ch == 0) {
                            break;
                        }
                        v |= static_cast<unsigned>(ch) << (24 - c * 8);
                    }
                    return v;
                };
                /* Row 2: quest id lengths, one nibble per quest (0..7), high
                 * nibble = quest 0.  0xF = unreadable, saturating.
                 *
                 * This is the shape check that survived: v114 established that
                 * these objects really are Quests (id present, three NetBool
                 * wrappers sharing one MethodTable), so the interesting part is
                 * now the *length*.  A board quest carries a three-digit id, a
                 * Data/Quests story entry carries a one- or two-character key. */
                rows[2] = questIdLenNib;
                /* Rows 3 and 4: the first four characters of target 0 and
                 * target 1, as ASCII hex.
                 *
                 * v111 read "Coop" here -- that is GoSomewhereQuest.whereToGo,
                 * a *place*, not a villager.  v112 read "Codi", which is what
                 * reading past the end of a 0xA8-byte Quest looks like.  v115,
                 * with the gate open, read "Coop" and "Hodi": the story quests
                 * were back. */
                rows[3] = (questTargetCount > 0) ? chars4(questTargets[0]) : 0u;
                rows[4] = (questTargetCount > 1) ? chars4(questTargets[1]) : 0u;
                /* Row 5: questType per quest, one nibble each, 0xF = unreadable.
                 *
                 * This is the field the target offset is chosen from, so it has
                 * to be on screen: it is the one number that says both "the
                 * NetInt read works" and "this quest even has a villager
                 * target".  Measured values so far: 1, 2, 3 (ItemDelivery),
                 * 4 (Monster), 5 (Socialize), 7 (Fishing), 8 (Location),
                 * 9 (Harvest/LostItem), 10 (Resource).  Only 3, 4, 7 and 10 can
                 * produce a badge.
                 *
                 * A nibble of 0 is ambiguous on its own -- it is either
                 * questType 0 or an element that never reached pass 1.  Row 6
                 * byte 1 (questPtrCount) settles which. */
                rows[5] = questTypeNib;
                /* Row 6: one bit per quest, quests 0..7 left to right.
                 *
                 *   byte 3: questHitMask -- this quest contributed a target
                 *   byte 2: questDoneMask -- this quest read completed = true
                 *   byte 1: questDailyMask -- this quest read dailyQuest = true
                 *   byte 0: questType of target 1 : target 0
                 *
                 * Byte 1 is the gate now, so it has to be on screen: 0x00
                 * means the log holds no Help Wanted quest and the absence of
                 * badges is correct, not broken.  It replaced questPtrCount in
                 * v122 -- v121 proved every element reaches pass 1, so the
                 * count stopped carrying information.
                 *
                 * Byte 0 shows the type each of the first two targets came
                 * from; only 3 (ItemDelivery), 4 (Monster), 7 (Fishing) and
                 * 10 (Resource) can appear. */
                rows[6] = ((questHitMask & 0xFFu) << 24) |
                    ((questDoneMask & 0xFFu) << 16) |
                    ((questDailyMask & 0xFFu) << 8) |
                    ((static_cast<unsigned>(questTargetCount > 1
                                                ? questTargetOff[1] & 0x0Fu
                                                : 0u)
                      << 4) |
                     static_cast<unsigned>(questTargetCount > 0
                                               ? questTargetOff[0] & 0x0Fu
                                               : 0u));
                /* Row 7: avatars drawn / characters seen / with a sheet /
                 * without one.  Known good since v93, kept as a canary. */
                rows[7] = ((drawn & 0xFFu) << 24) | ((seen & 0xFFu) << 16) |
                    ((withSheet & 0xFFu) << 8) | (noSheet & 0xFFu);
                /* Row 8: locations matched / Farm fallback / bad tiles /
                 * birthday badges.  The last one is the canary that says the
                 * calendar reads still work. */
                rows[8] = ((matched & 0xFFu) << 24) |
                    ((fallback & 0xFFu) << 16) |
                    ((badTile & 0xFFu) << 8) |
                    (static_cast<unsigned>(bdayCount) & 0xFFu);
            }
            if (kShowDiagnostics) {
                /* Every row is numbered on the left.
                 *
                 * Three rounds in a row the pasted readout was one line short
                 * and it took a while to notice -- with nine rows a dropped
                 * line is invisible unless each one says which row it is.
                 * The version tag sits on the same line as row 8 rather than
                 * on a line of its own, so it cannot be mistaken for a row. */
                /* Row 9: scythe diagnostic -- stage bits, call count, and the
                 * tile the last attempt used.  See s_scytheStage. */
                rows[9] = ((s_scytheStage & 0xFFu) << 24) |
                    ((s_grassCut & 0xFFu) << 16) |
                    ((s_grassFound & 0xFFu) << 8) |
                    (s_scytheCalls & 0xFFu);
                /* Rows 10-11: animal page icon (v289).
                 *   row10 = n / mt / ptr / guid
                 *   row11 = fixed / y0 / texH>>4 / horse
                 * horse=0 means the hook never found a horse pointer at all;
                 * mt=0 means FindAnimals returned no Horse entries, i.e. the
                 * horse icon on the page does not come from this list. */
                /* Rows 10-11 (v312): the switches, packed.
                 *
                 *   row10 = list capacity / bits 1..10 of g_rowOn
                 *   row11 = fileOk / bits 1..10 read back from the file copy
                 *
                 * Every row on reads `40 07FE` (capacity 64, all ten bits set).
                 * Turning 打怪 (row 7) off gives `40 07BE`.  If row11's bits do
                 * not match row10 after a restart, the save side lost them; if
                 * they match but a box came back ticked, the element did not get
                 * its +0x51 written at creation. */
                {
                    unsigned mask = 0;
                    for (int i = 1; i < kRowCount; i++) {
                        if (g_rowOn[i] != 0) {
                            mask |= (1u << i);
                        }
                    }
                    rows[10] = ((static_cast<unsigned>(g_listCap) & 0xFFu) << 16) |
                        (mask & 0xFFFFu);
                    rows[11] = ((s_fileMaskOk & 0xFu) << 28) |
                        (s_fileMask & 0xFFFFu);
                }
                /* Row 12: auto-attack (v290/v291) -- damageMonster calls issued,
                 * how many of them found a monster in range, how many tile
                 * moves were skipped because the tool was the scythe, and
                 * whether Game1.CurrentEvent is non-null (v293: the event path
                 * of getMovementSpeed ignores the patched constant). */
                rows[12] = ((s_combatCalls & 0xFFu) << 24) |
                    ((s_combatHits & 0xFFu) << 16) |
                    ((s_combatScythe & 0xFFu) << 8) |
                    (((g_tractorOn & 0xFu) << 4) | (s_eventActive & 0xFu));
                /* Row 13 (v318): tab/page plumbing.
                 *   tabState / pageReady / pages.Count / index of our page
                 * Expected on a good build: 01 / 01 / 0B / 0A.
                 * pageReady=00 or index=FF means the page never made it into
                 * GameMenu.pages, which is what makes the tab open Options. */
                rows[13] = ((static_cast<unsigned>(g_dbgCur) & 0xFFu) << 24) |
                    ((s_pageReady & 0xFFu) << 16) |
                    ((static_cast<unsigned>(g_lastTabRet) & 0xFFu) << 8) |
                    (g_ourCount & 0xFFu);
                /* Row 14 (v336): Carry Chest gate trace.  A clean pickup is
                 * FF111000 -- see the s_carry* block for the nibble map. */
                rows[14] = ((s_carryCalls & 0xFu) << 28) |
                    ((s_carryState & 0xFu) << 24) |
                    ((s_carryAdd & 0xFu) << 20) |
                    ((s_carryInBag & 0xFu) << 16) |
                    ((s_carryRemoved & 0xFu) << 12) |
                    ((s_autoPasses & 0xFu) << 8) |
                    ((s_autoOutput & 0xFu) << 4) |
                    (s_autoInput & 0xFu);
                /* Row 15 (v340): Automate group shape.
                 *   groups / group chests / group machines / group tiles /
                 *   registry chests / registry machines / output / input
                 * A healthy single chest + 3 machines reads roughly
                 * 1 1 3 N 1 3 F F. */
                /* Row 16/17 (v343): why the file log is (not) open.
                 *   row 16 = first non-zero mount step result, 0 = all fine
                 *            (DiagSm / DiagInit / DiagOpenFs / DiagCreate /
                 *             DiagSaveData, in that order)
                 *   row 17 = channel<<8 | isOpen<<4 | attempts
                 * A healthy session reads 00000000 and 00000111. */
                {
                    /* v346: line 11 = the trace read back from the preferences
                     * file at boot (i.e. the last thing the PREVIOUS session
                     * did before it died), line 12 = this session's live trace.
                     * Both look like A5ssssii: ssss = step, iiii = sprite. */
                    rows[16] = s_tracePrev;
                    rows[17] = s_trace;
                    rows[18] = ((static_cast<unsigned>(sdlog::Channel()) & 0xFu) << 8) |
                        ((sdlog::IsOpen() ? 1u : 0u) << 4) |
                        (s_logTry & 0xFu);
                }
                /* v359: wire tiles found, whether the Flooring EEType is known,
                 * and whether this pass was a rebuild (i.e. it did lookups). */
                /* v360: rebuilds | lookups | hits | wire tiles.  One reading
                 * separates "never scanned" from "scanned, found no Flooring"
                 * from "found it, grouping is the problem". */
                /* v370: the wire works, so row 2 keeps only what a support
                 * question needs -- the probe state and how many wire tiles are
                 * cached.  Lookups / hits / rebuilds are still counted in the
                 * background in case they are needed again. */
                /* v374: temporary -- why the 12th row does or does not show.
                 *   A  = g_appendState (1 all appended / 2 no capacity /
                 *        3 bad list / 5 label string failed / 6 partial)
                 *   N  = g_ourCount (how many rows we actually appended)
                 *   P  = probe state */
                /* v380: read-only diagnostic for the switch mask.
                 *   K    = s_fileMaskOk (1 = the file value had our tag)
                 *   MMM  = s_fileMask & 0xFFF (bit 11 = 0x800 = the new switch)
                 * K=0 -> the file had no tag, so everything defaults to ON. */
                /* v384: K = s_fileMaskOk, MMM = what the FILE gave us at boot,
                 * SSS = g_savedMask, i.e. what we are about to write.  Toggle a
                 * switch and watch SSS: if its bit 11 stays 0, the change was
                 * never recorded (g_savedMask is not rebuilt); if it goes to 1
                 * while the file stays old, the save-side injection is the
                 * problem. */
                rows[19] = ((s_fileMaskOk & 0xFu) << 24) |
                    ((s_fileMask & 0xFFFu) << 12) |
                    (g_savedMask & 0xFFFu);
                rows[15] = ((s_autoGroups & 0xFu) << 28) |
                    ((s_dbgGrpChests & 0xFu) << 24) |
                    ((s_dbgGrpMachines & 0xFu) << 20) |
                    ((s_dbgGrpNodes & 0xFu) << 16) |
                    ((s_dbgRegChests & 0xFu) << 12) |
                    ((s_dbgRegMachines & 0xFu) << 8) |
                    ((s_autoOutput & 0xFu) << 4) |
                    (s_autoInput & 0xFu);
                /* v360: five rows, left column, labelled 0..4.
                 *
                 * Before this there were 21 numbers on screen -- a 14-row
                 * legacy block plus a 7-row right-hand column -- and the only
                 * ones that still answer a question are these five:
                 *   0 version tag
                 *   1 automation: groups / grpChests / grpMachines / grpNodes /
                 *                 regChests / regMachines / output / input
                 *   2 wire: probe state / cached wire tiles
                 *   3 trace from the file (previous session's last action)
                 *   4 trace live (this session) */
                {
                    unsigned shown[5];
                    shown[0] = StageTag(kMapStage);
                    shown[1] = rows[15];
                    shown[2] = rows[19];
                    /* v385: rows 3/4 show the serialisation chain instead of
                     * the old trace -- the trace no longer goes to disk.
                     *   3 = N MMM : N = how many times the prefs-save hook ran,
                     *              MMM = the mask actually sitting in the copy
                     *              we wrote (this is what the file should get)
                     *   4 = the raw 32-bit value of Farmer.disconnectDay.
                     *       FEFFFEFF = untouched default (nothing of ours)
                     *       5A5A0xxx = our magic | switch mask (write stuck)
                     *       FFFFFFFF = player/wrapper not readable */
                    shown[3] = ((s_dbgLoadRuns & 0xFu) << 16) |
                        ((s_dbgHookRuns & 0xFu) << 12) | (s_dbgCoVal & 0xFFFu);
                    shown[4] = s_dbgDisc;
                    for (int i = 0; i < 5; i++) {
                        hexnum(bounds[0] + 96, bounds[1] + 100 + i * 34,
                               static_cast<unsigned>(i), 1, 4);
                        if (i == 2) {
                            continue;   /* row 2 is drawn with gaps below */
                        }
                        hexnum(bounds[0] + 120, bounds[1] + 100 + i * 34,
                               shown[i], 8, 4);
                    }
                    /* Row 2: P | CC
                     *   A = append state, N = rows appended, P = probe */
                    const int rx = bounds[0] + 120;
                    const int ry = bounds[1] + 100 + 2 * 34;
                    hexnum(rx, ry, s_fileMaskOk & 0xFu, 1, 4);
                    hexnum(rx + 24, ry, s_fileMask & 0xFFFu, 3, 4);
                    hexnum(rx + 72, ry, g_savedMask & 0xFFFu, 3, 4);
                }
            }
            /* Stage 126 readout.  Drawn here, after the avatar pass, so no
             * villager can sit on top of it -- which is what happened on the
             * first hardware run. */
            if (kMapStage == 132) {
                hexnum(bounds[0] + 96, bounds[1] + 100, s_hoverRow0, 8, 4);
                hexnum(bounds[0] + 96, bounds[1] + 134, s_hoverRow1, 8, 4);
                hexnum(bounds[0] + 96, bounds[1] + 168, s_hoverRow2, 8, 4);
                hexnum(bounds[0] + 96, bounds[1] + 202, StageTag(kMapStage),
                       3, 4);
            }
            /* Stage 139 -- P3a: the hovered location's NPCs, by name.
             *
             * P2 is done (v137): 0xC1BB0 draws, and Chinese comes out as real
             * glyphs.  P1 is done (v132): the hovered location is resolved.
             *
             * v138 tried the obvious source -- loc+0xB0 -> collection+0x58 ->
             * List<NPC> -- and got flags=0x07: location found, collection
             * pointer readable, but the list read failed, so nothing was drawn.
             * The chain itself is right; the game's own getCharacterFromName
             * (0x14CE270) does exactly ldr x8,[x20,#176] / ldr x8,[x8,#88] and
             * then reads _size at +0x18 and _version at +0x1C, which is the
             * List<T> layout readVerifiedList already expects.
             *
             * So this stage keeps that read as path A *and* adds path B, which
             * needs no list at all: the avatar pass already walks every
             * location and resolves every drawable character.  It now records
             * (location, character) pairs as it goes, and path B simply keeps
             * the records whose location pointer equals the hovered one.
             *
             * Path B is not just a fallback.  It is by construction the same
             * set of characters whose avatars the player can see on that map
             * area, which is precisely what this list exists to disambiguate
             * when avatars overlap.
             *
             * Names, in priority order:
             *   Character+0xC0  _displayName   bare managed string (no +0x68)
             *   Character+0xA8  name           NetString, payload at +0x68
             * The first is the localised name ("罗宾"), the second the internal
             * key ("Robin").  An unreadable or empty name is never a match.
             *
             * Layout: map frame top-left + (24,24), 34px apart, at most 8
             * lines, each drawn twice (black at +2,+2 then white) so no
             * MeasureString and no guessed glyph width is needed.
             *
             * Rows:
             *   0  locIndex<<24 | total<<16 | shown<<8 | flags
             *        flags 0x01 font / 0x02 loc readable / 0x04 collection
             *              readable / 0x08 path A list OK /
             *              0x10 path B produced names / 0x20 avatar records
             *   1  first four UTF-16 units of the hovered location's name
             *   2  avHits<<24 | avCount<<16 | pathAFail<<8 | pathA n
             *        pathAFail 1 list unreadable / 2 n out of range /
             *                  3 wrapper bad / 4 capacity word bad /
             *                  5 capacity < n / 6 buffer bad / 7 n == 0
             *   3  version tag */
            /* This block is NOT diagnostics: it is the live hover feature
             * (floating names plus the append into the game's own tooltip).
             * It used to be gated on `kMapStage == 172`, which means any later
             * stage bump silently deleted it -- caught by the binary shrinking
             * 6 KB when stage 173 was built.  Gate it on `>=` instead, the same
             * form the avatar pass uses, so it can never be dropped again. */
            if (kMapStage >= kStageAvatar) {
                void* loc = s_hoverLoc;
                unsigned flags = 0;
                int total = 0;
                int shown = 0;
                void* names[8] = {nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr};
                unsigned locChars = 0;
                int pathAN = 0;
                int pathAFail = 0;
                int avHits = 0;

                /* A bare managed string we can hand to DrawString: non-empty and
                 * with its UTF-16 payload fully readable. */
                auto usableStr = [](const void* s) -> int {
                    const int l = ManagedStrLen(s);
                    if (l <= 0) {
                        return -1;
                    }
                    if (!RangeReadable(
                            reinterpret_cast<const unsigned char*>(s) + 0x14,
                            static_cast<size_t>(l) * 2u)) {
                        return -1;
                    }
                    return l;
                };
                /* NetString field -> payload string object. */
                auto netPayload = [](void* obj, uintptr_t off) -> void* {
                    if (obj == nullptr || !Plausible(obj) ||
                        !RangeReadable(
                            reinterpret_cast<unsigned char*>(obj) + off, 8u)) {
                        return nullptr;
                    }
                    void* wrap = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(obj) + off);
                    if (wrap == nullptr || !Plausible(wrap) ||
                        !RangeReadable(wrap, 0x70u)) {
                        return nullptr;
                    }
                    return *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(wrap) + 0x68);
                };
                /* First four UTF-16 units, low byte each, packed little-endian.
                 * Enough to recognise "Scie" for ScienceHouse in a readout. */
                auto pack4 = [](const void* s) -> unsigned {
                    const int l = ManagedStrLen(s);
                    if (l <= 0) {
                        return 0;
                    }
                    const char16_t* p = reinterpret_cast<const char16_t*>(
                        reinterpret_cast<const unsigned char*>(s) + 0x14);
                    unsigned v = 0;
                    for (int k = 0; k < 4 && k < l; k++) {
                        v |= static_cast<unsigned>(p[k] & 0xFFu) << (k * 8);
                    }
                    return v;
                };
                auto nameOf = [&](void* npc) -> void* {
                    if (npc == nullptr || !Plausible(npc) ||
                        !RangeReadable(
                            reinterpret_cast<unsigned char*>(npc), 0xC8u)) {
                        return nullptr;
                    }
                    void* n = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(npc) + 0xC0);
                    if (usableStr(n) > 0) {
                        return n;
                    }
                    n = netPayload(npc, 0xA8);
                    if (usableStr(n) > 0) {
                        return n;
                    }
                    return nullptr;
                };
                auto pushName = [&](void* nm) {
                    if (nm == nullptr) {
                        return;
                    }
                    total++;
                    if (shown < 8) {
                        names[shown] = nm;
                        shown++;
                    }
                };

                void* font = nullptr;
                {
                    void* slot = reinterpret_cast<void*>(
                        g_base + kGame1SmallFontOff);
                    if (RangeReadable(slot, 8u)) {
                        void* f = *reinterpret_cast<void**>(slot);
                        if (f != nullptr && Plausible(f) &&
                            RangeReadable(f, 8u)) {
                            font = f;
                            flags |= 0x01u;
                        }
                    }
                }

                if (loc != nullptr && Plausible(loc) &&
                    RangeReadable(reinterpret_cast<unsigned char*>(loc),
                                  0x1B0u)) {
                    flags |= 0x02u;
                    locChars = pack4(netPayload(loc, 0x1A8));

                    /* Path A: the location's own character list. */
                    void* collection = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(loc) + 0xB0);
                    if (collection != nullptr && Plausible(collection) &&
                        RangeReadable(
                            reinterpret_cast<unsigned char*>(collection),
                            0x60u)) {
                        flags |= 0x04u;
                        void* inner = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(collection) +
                            0x58);
                        if (!Plausible(inner) ||
                            !RangeReadable(
                                reinterpret_cast<unsigned char*>(inner),
                                0x1Cu)) {
                            pathAFail = 1;
                        }
                        else {
                            auto* lb = reinterpret_cast<unsigned char*>(inner);
                            pathAN = *reinterpret_cast<int*>(lb + 0x18);
                            void* wrapper =
                                *reinterpret_cast<void**>(lb + 0x10);
                            if (pathAN < 0 || pathAN > 4096) {
                                pathAFail = 2;
                            }
                            else if (!Plausible(wrapper) ||
                                     !RangeReadable(wrapper, 0x28u)) {
                                pathAFail = 3;
                            }
                            else {
                                auto* wb =
                                    reinterpret_cast<unsigned char*>(wrapper);
                                void* lengthPtr =
                                    *reinterpret_cast<void**>(wb + 0x20);
                                if (!RangeReadable(lengthPtr, sizeof(int))) {
                                    pathAFail = 4;
                                }
                                else {
                                    const int cap =
                                        *reinterpret_cast<int*>(lengthPtr);
                                    void* buffer =
                                        *reinterpret_cast<void**>(wb + 0x10);
                                    if (cap < pathAN || cap < 0 ||
                                        cap > 65536) {
                                        pathAFail = 5;
                                    }
                                    else if (pathAN > 0 &&
                                             (!Plausible(buffer) ||
                                              !RangeReadable(
                                                  buffer,
                                                  static_cast<size_t>(pathAN) *
                                                      sizeof(void*)))) {
                                        pathAFail = 6;
                                    }
                                    else if (pathAN == 0) {
                                        pathAFail = 7;
                                    }
                                    else {
                                        flags |= 0x08u;
                                        auto** items =
                                            reinterpret_cast<void**>(buffer);
                                        for (int j = 0; j < pathAN && j < 512;
                                             j++) {
                                            pushName(nameOf(items[j]));
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                /* Path B: whatever the avatar pass resolved for this location
                 * on this very frame.  Only consulted when path A produced
                 * nothing, so a working path A always wins. */
                if (s_avCount > 0) {
                    flags |= 0x20u;
                }
                if (total == 0 && loc != nullptr) {
                    for (int k = 0; k < s_avCount; k++) {
                        if (s_avLocPtr[k] != loc) {
                            continue;
                        }
                        avHits++;
                        pushName(nameOf(s_avNpcPtr[k]));
                    }
                    if (total > 0) {
                        flags |= 0x10u;
                    }
                }

                if (font != nullptr && shown > 0) {
                    auto drawString = reinterpret_cast<DrawStringFn>(
                        g_base + kDrawStringOff);
                    const float lx = static_cast<float>(bounds[0] + 56);
                    const float ly = static_cast<float>(bounds[1] + 52);
                    for (int i = 0; i < shown; i++) {
                        const float y = ly + static_cast<float>(i) * 34.0f;
                        /* Shadow first, then the glyphs on top of it. */
                        drawString(batch, font, names[i], 0xCC000000u,
                                   lx + 2.0f, y + 2.0f);
                        drawString(batch, font, names[i], 0xFFFFFFFFu, lx, y);
                    }
                }

                s_hoverRow0 = ((s_hoverLocIdx & 0xFFu) << 24) |
                    ((static_cast<unsigned>(total) & 0xFFu) << 16) |
                    ((static_cast<unsigned>(shown) & 0xFFu) << 8) |
                    (flags & 0xFFu);
                s_hoverRow1 = locChars;
                s_hoverRow2 =
                    ((static_cast<unsigned>(avHits) & 0xFFu) << 24) |
                    ((static_cast<unsigned>(s_avCount) & 0xFFu) << 16) |
                    ((static_cast<unsigned>(pathAFail) & 0xFFu) << 8) |
                    (static_cast<unsigned>(pathAN) & 0xFFu);
                /* Stage 141/142: hoverText probe, then append a name.
                 *
                 * v141 readout (hardware): length 49, **two U+000A**, first
                 * four units `木匠的商` -- so hoverText is one multi-line
                 * string.  That means a newline can be cut straight out of it
                 * with Substring and the names concatenated on, with no
                 * string allocator involved.
                 *
                 *   Substring 0x395F40  (this, startIndex, length)  -- 270 live callers
                 *   Concat    0x39D7C0  (a, b)                      -- 2019 live callers
                 *
                 * Both signatures were pinned by reading live call sites, not
                 * by guessing.  Only ONE name is appended here: every extra
                 * Concat is another allocation whose intermediate result is
                 * only held in a native local the GC cannot see. */
                void* ht = nullptr;
                if (s_mapPage != nullptr) {
                    ht = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(s_mapPage) + 0xC0);
                }
                int hlen = 0;
                int nls = 0;
                int nlIdx = -1;
                bool readable = false;
                if (ht != nullptr && Plausible(ht) &&
                    RangeReadable(
                        reinterpret_cast<unsigned char*>(ht), 0x18u)) {
                    hlen = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(ht) + 0x10);
                    if (hlen > 0 && hlen < 4096 &&
                        RangeReadable(
                            reinterpret_cast<unsigned char*>(ht) + 0x14,
                            static_cast<size_t>(hlen) * 2u)) {
                        readable = true;
                        const char16_t* p = reinterpret_cast<const char16_t*>(
                            reinterpret_cast<unsigned char*>(ht) + 0x14);
                        for (int i = 0; i < hlen; i++) {
                            if (p[i] == u'\n') {
                                nls++;
                                if (nlIdx < 0) {
                                    nlIdx = i;
                                }
                            }
                        }
                    }
                }

                /* Stage 145: which avatars fall inside the hovered tooltip's
                 * own rectangle?
                 *
                 * The location name is NOT reachable from the tooltip id --
                 * v144 proved the id is "{region}/{displayName}", so neither
                 * segment is a GameLocation name.  But MapAreaTooltip carries
                 * `CachedPixelArea` at +0x10, and static analysis of
                 * MapAreaPosition.GetPixelArea (0x1BBB430) shows that type is
                 * `Rectangle?`:
                 *     +0x20  hasValue (byte)
                 *     +0x10  X, Y   (two int32)
                 *     +0x18  W, H   (two int32)
                 *     returned as an HFA pair in x0/x1
                 * So the rectangle is readable directly, and "which markers
                 * sit inside it" needs no name matching at all. */
                /* Look the tooltip's PixelArea up by id.  v146 read it from
                 * MapAreaTooltip+0x10 and got (164,124,48,36), while the data
                 * says the carpenter's shop is (169,31,12,9) -- so that offset
                 * is not the rectangle.  The id, however, is definitely right
                 * (v143 matched its first segment), and the rectangle is in
                 * Data/WorldMap, so key the lookup on the id instead. */
                int tipX = 0;
                int tipY = 0;
                int tipW = 0;
                int tipH = 0;
                int rectOk = 0;
                if (s_hoverTipId != nullptr && Plausible(s_hoverTipId)) {
                    const int idl2 = ManagedStrLen(s_hoverTipId);
                    if (idl2 > 0 && idl2 < 128 &&
                        RangeReadable(
                            reinterpret_cast<unsigned char*>(s_hoverTipId) +
                            0x14,
                            static_cast<size_t>(idl2) * 2u)) {
                        const char16_t* ip =
                            reinterpret_cast<const char16_t*>(
                                reinterpret_cast<unsigned char*>(s_hoverTipId) +
                                0x14);
                        for (int i = 0; i < kTooltipRectCount; i++) {
                            const char* lit = kTooltipRects[i].id;
                            bool eq = true;
                            for (int k = 0; k < idl2; k++) {
                                if (lit[k] == '\0' ||
                                    ip[k] != static_cast<char16_t>(
                                                 static_cast<unsigned char>(
                                                     lit[k]))) {
                                    eq = false;
                                    break;
                                }
                            }
                            if (eq && lit[idl2] != '\0') {
                                eq = false;
                            }
                            if (eq) {
                                tipX = kTooltipRects[i].x;
                                tipY = kTooltipRects[i].y;
                                tipW = kTooltipRects[i].w;
                                tipH = kTooltipRects[i].h;
                                rectOk = 1;
                                break;
                            }
                        }
                    }
                }

                int inRect = 0;
                int appended = 0;
                if (readable && nlIdx >= 0 && rectOk != 0 &&
                    ht != s_lastAppendSrc) {
                    /* Same space the markers live in: bounds + 4*texture. */
                    const float rx0 = static_cast<float>(bounds[0]) +
                                      static_cast<float>(tipX) * 4.0f +
                                      kMarkerOffX - 4.0f;
                    const float ry0 = static_cast<float>(bounds[1]) +
                                      static_cast<float>(tipY) * 4.0f +
                                      kMarkerOffY - 4.0f;
                    const float rx1 = rx0 + static_cast<float>(tipW) * 4.0f + 8.0f;
                    const float ry1 = ry0 + static_cast<float>(tipH) * 4.0f + 8.0f;
                    auto substring = reinterpret_cast<void* (*)(void*, int, int)>(
                        g_base + 0x395F40);
                    auto concat = reinterpret_cast<void* (*)(void*, void*)>(
                        g_base + 0x39D7C0);
                    void* nl = substring(ht, nlIdx, 1);
                    if (nl != nullptr && Plausible(nl)) {
                        void* merged = ht;
                        for (int k = 0; k < s_avCount && appended < 4; k++) {
                            if (s_avX[k] < rx0 || s_avX[k] > rx1 ||
                                s_avY[k] < ry0 || s_avY[k] > ry1) {
                                continue;
                            }
                            inRect++;
                            void* nm = nameOf(s_avNpcPtr[k]);
                            if (nm == nullptr) {
                                continue;
                            }
                            void* m1 = concat(merged, nl);
                            if (m1 == nullptr || !Plausible(m1)) {
                                break;
                            }
                            void* m2 = concat(m1, nm);
                            if (m2 == nullptr || !Plausible(m2)) {
                                break;
                            }
                            merged = m2;
                            appended++;
                        }
                        if (appended > 0) {
                            *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(s_mapPage) +
                                0xC0) = merged;
                            s_lastAppendSrc = merged;
                        }
                    }
                }

                /* v187 readout.  Four rows, each numbered on screen.
                 *
                 * Row 0 -- the storage story:
                 *   bits 28..31  resolution path (1 via drawHUD's `this`,
                 *                2 via the static Game1 slot, 3 both)
                 *   bit  24      Options+0x24 right now
                 *   bit  20      Options+0x24 AS FOUND AT BOOT
                 *   bits 16..19  appendState (1 ok, 2 no capacity, 3 bad list,
                 *                4 bad element)
                 *   bits  8..15  appendCount
                 *   bits  0.. 7  bit0 resolved, bit1 passed LooksLikeOptions,
                 *                bit2 an element was appended (latched, never
                 *                cleared -- it is NOT a liveness test any more,
                 *                see the dangling-pointer note in
                 *                SyncFastFromOptions), bit3 the box was clicked
                 *
                 * Row 1 -- Options+0x64 as read on the SAME frame the boot
                 * latch was taken.  This exists to distrust row 0's bit 20: the
                 * UI scale is the one option value known to survive a restart
                 * (155% reads 3FC60000), so if row 1 is not 3FC60000 then the
                 * latch fired before the file was applied and bit 20 proves
                 * nothing.
                 *
                 * Row 2 -- the save side.  Byte 3 is a bitfield:
                 *   bit0  hook captured an instance at all
                 *   bit1  it passes Plausible()
                 *   bit2  its +0x50 clientOptions IS Game1.options.  It is NOT
                 *         at runtime -- the preferences constructor makes its
                 *         own copy -- and that mismatch is what kept v195 from
                 *         ever reaching the file
                 *   bit3  the serialised copy (+0x50) was written with the
                 *         value we want in the file
                 *   bit4  +0x59 _isBusy was already non-zero when we asked, so
                 *         the call would return without writing
                 *   bit5  the call was entered
                 *   bit6  the call RETURNED
                 *   bit7  clientOptions (+0x50) was not null
                 * Byte 2 is the serialised copy's own +0x24 in the high nibble
                 * (this is the value that goes into the file), optionsDirty in
                 * bit 1 and _isBusy in bit 0.  Byte 1 is the busy watch in the
                 * high nibble (bit0 armed, bit1 saw _isBusy set, bit2 the task
                 * finished) and the save task's status in the low nibble:
                 * 3 running, 5 RanToCompletion (the file was written),
                 * 6 Canceled, 7 Faulted (the write threw -- v197), F nothing
                 * queued yet.
                 * Byte 0 multiplier pending (2 = fast).
                 *
                 * Row 3 -- the element side: whichOption (+0x28, must be 23 hex
                 * = 35 decimal), isChecked (+0x51), the desired value we settled
                 * on, and the multiplier in force.  If the top byte is not 23 the
                 * ctor argument did not land and nothing else means anything. */
                s_hoverRow0 =
                    ((s_optFlags & 0xF0u) << 24) |
                    ((s_optBit & 0x1u) << 24) |
                    ((s_optBoot & 0x1u) << 20) |
                    ((g_appendState & 0x0Fu) << 16) |
                    ((g_appendCount & 0xFFu) << 8) |
                    (s_optFlags & 0x0Fu);
                /* v212: row 1 now reports the tab attempt instead of the old
                 * UI-scale latch (options persistence is solved, that latch has
                 * done its job).  byte 3 = s_tabState, byte 2 = appendState,
                 * byte 1 = appendCount.  1 = tab installed, >=10 = the stage
                 * that refused. */
                s_hoverRow1 = ((s_tabState & 0xFFu) << 24) |
                              ((g_appendState & 0xFFu) << 16) |
                              ((g_appendCount & 0xFFu) << 8) |
                              (s_bootUiBits & 0xFFu);
                s_hoverRow2 =
                    ((s_saveDbg & 0xFFu) << 24) |
                    (((s_serBit & 0xFu) << 4) | (s_optDirty & 0x1u) << 1 |
                     (s_busyVal & 0x1u)) << 16 |
                    (((s_busySeen & 0x7u) << 4) |
                     ((s_taskState >= 0 ? s_taskState : 0xFu) & 0xFu)) << 8 |
                    (static_cast<unsigned>(g_pendingFast) & 0xFFu);
                /* v203: the high nibble of byte 0 says which instance the save
                 * was issued on -- 1 = Game1+0x150, 2 = the captured pointer,
                 * 3 = neither passed the fingerprint (nothing issued).  The
                 * low nibble is still the multiplier. */
                s_hoverRow3 =
                    ((s_elemWhich & 0xFFu) << 24) |
                    ((s_elemChecked & 0xFFu) << 16) |
                    ((s_desired & 0xFFu) << 8) |
                    ((s_prefsSrc & 0xFu) << 4) |
                    (static_cast<unsigned>(g_animFast) & 0xFu);
                /* Row 4 -- the v199 write-path probe.
                 *
                 * Byte 3 is "how far did the write get", for OUR attempt:
                 *   bit0  _savePreferences was entered
                 *   bit1  writeSettings was entered
                 *   bit2  _savePreferences RETURNED
                 *   bit3  writeSettings RETURNED
                 *
                 * Read it as a ladder.  Each rung localises the throw:
                 *   0x0  nothing entered        -> it threw before the write
                 *   0x1  entered, not back      -> threw in File.Create/writer
                 *   0x3  + writeSettings entered-> threw while serialising
                 *   0x5  _savePreferences back  -> the write finished; the
                 *                                  fault is in the Task wrapper
                 *   0xF  everything back        -> the file WAS written
                 *
                 * Byte 2 high nibble / byte 1 high nibble are the CUMULATIVE
                 * entry counts (clamped at 15).  The game runs this same path
                 * at boot, so both must be non-zero right after a restart --
                 * that is how we tell "hook never installed" from "hook
                 * installed but never reached". */
                s_hoverRow4 =
                    ((((s_wrArmed && s_wrEntered > s_wrEntered0) ? 1u : 0u) |
                      (((s_wrArmed && s_wrWrite > s_wrWrite0) ? 1u : 0u) << 1) |
                      (((s_wrArmed && s_wrBack) ? 1u : 0u) << 2) |
                      (((s_wrArmed && s_wrWriteBack) ? 1u : 0u) << 3) |
                      ((s_wrBackAll ? 1u : 0u) << 4) |
                      ((s_wrWriteBackAll ? 1u : 0u) << 5) |
                      ((s_wrArmed ? 1u : 0u) << 6)) << 24) |
                    ((s_wrEntered & 0xFu) << 20) |
                    ((s_wrWrite & 0xFu) << 12) |
                    ((s_serBit & 0xFu) << 4) |
                    (static_cast<unsigned>(g_savedFlag) & 0xFu);
                /* Row `E` -- the v202 fingerprint autopsy.
                 *
                 * v201 could only say "LooksLikeStartupPrefs said no", and two
                 * very different faults produce that: a dangling wrapper
                 * (nothing retains it after boot -- 0x178ADC4 only lets
                 * prefs+0x50 escape) and a check that is too strict for a live
                 * object.  The first says "allocate our own instance", the
                 * second says "fix the check"; guessing between them is how
                 * ten versions got spent.  So report WHICH check said no.
                 *
                 *   byte 3  failing check index (F = all passed)
                 *   byte 2  low byte of the method table
                 *   byte 1  timesPlayed (+0x18) low byte
                 *   byte 0  windowMode (+0x1c) high nibble,
                 *           displayIndex (+0x20) low nibble
                 *
                 * A dead block still keeps its MethodTable, so byte 2 alone
                 * cannot decide; but a live StartupPreferences has
                 * timesPlayed 0..n and windowMode 0..2, so garbage in bytes
                 * 1/0 is the signature of recycled memory. */
                s_hoverRow5 = 0;
                {
                    /* v204: back to reporting the CAPTURED candidate -- the
                     * Game1+0x150 idea was disproved (check 2 failed there,
                     * and Game1's field table ends at +0xeb, so that class is
                     * not Game1 at all).  Byte 1 now says whether a valid
                     * snapshot was ever taken, which is what decides whether
                     * the instance we build keeps the player's scalars. */
                    /* v209: bit 3 = we had to write the value at install,
                     * low 3 bits = the fingerprint check index (7 = passed,
                     * because 0xF & 7 is 7). */
                    unsigned b3 = (((s_forced != 0) ? 8u : 0u) |
                                   (s_fpFailCap & 0x7u))
                                  << 24;
                    /* v206: bytes 2 and 1 now belong to the READ side.
                     *
                     * v205 proved the write works (row 4 = 7F202011), so the
                     * method table and the windowMode/displayIndex pair that
                     * used to live here have done their job and are replaced.
                     *
                     *   byte 2 high nibble  s_fileBit      -- what the FILE said
                     *   byte 2 low  nibble  s_appliedBit   -- what set_options got
                     *   byte 1 high nibble  s_optBoot      -- LIVE +0x24 at the
                     *                                        first HUD frame
                     *   byte 1 low  nibble  bit7 snapshot valid,
                     *                       low 3 bits frames it stayed valid
                     *
                     * F in any of them means that probe never fired. */
                    unsigned b2 = (((s_fileBit & 0xFu) << 4) |
                                   (s_appliedBit & 0xFu))
                                  << 16;
                    /* v208: the high nibble is the LIVE Options+0x24 as
                     * latched on the first HUD frame (row 0 bit 20 reports the
                     * same value).  It replaces v207's s_gameWriteBit, which
                     * had to be sampled on the save's worker thread and cost a
                     * data race to collect.
                     *
                     * The three nibbles now read as a pipeline in one line:
                     *   file / installed / live-after-boot
                     *   `111` everything works -> the loss is in our own
                     *         checkbox read path
                     *   `110` installed, then lost from the live object
                     *   `000` the file said false -> our write never landed
                     *   `1F1` installed, latch never taken */
                    unsigned b1 = ((((g_bootLatched != 0)
                                        ? (s_optBoot & 0x1u)
                                        : 0xFu) << 4) |
                                   ((s_snapOk ? 8u : 0u) |
                                    (s_snapFrames & 0x7u)))
                                  << 8;
                    /* v209: byte 0 is the install report, replacing
                     * windowMode/displayIndex (v202's autopsy already used
                     * those; the fingerprint now reports 7 = passed anyway).
                     *   high nibble  the value set_options got on the first
                     *               call that happened AFTER readSettings
                     *               (F = that never happened)
                     *   low  nibble  how many times set_options ran */
                    unsigned b0 = ((s_appliedAfterRead & 0xFu) << 4) |
                                  (s_setOptCalls & 0xFu);
                    s_hoverRow5 = b3 | b2 | b1 | b0;
                    /* Row `F` -- the v206 event order.
                     *
                     * byte 3  0xF0 | number of hooked events seen (proves this
                     *         build is the one running)
                     * byte 2  sequence at the first writeSettings
                     * byte 1  sequence at the first set_options
                     * byte 0  sequence at the first readSettings
                     *
                     * 0xFF = that hook never fired.  The ORDER is the point:
                     * it decides whether the game's own boot save runs before
                     * or after the file is applied, and therefore whether our
                     * write is being erased on the very next boot. */
                    s_hoverRow6 = 0xF0000000u |
                                  ((s_evtSeq & 0xFu) << 24) |
                                  ((s_evtWrite & 0xFFu) << 16) |
                                  ((s_evtSetOpt & 0xFFu) << 8) |
                                  (s_evtRead & 0xFFu);
                }
                (void)nls;
                (void)hlen;
                (void)locChars;
                (void)s_hoverTipId;
                (void)rectOk;
                /* Counted for the hover path but no longer reported; the
                 * readout now belongs to the options store. */
                (void)inRect;
                (void)flags;
                (void)appended;
                /* Only ever fed the old birthday readout row. */
                (void)s_bdayDay;
            }

            /* Diagnostic rows.  Gated ONLY on kShowDiagnostics: gating it on
             * `kMapStage == N` broke silently three times, because bumping the
             * stage with sed never matched the `kShowDiagnostics &&` prefix and
             * the rows stopped being drawn without any compile error. */
            if (kShowDiagnostics) {
                /* Numbered like the legacy column on the left, so "row 3" can
                 * never be read as "the third number I can see".  That mix-up
                 * cost two rounds: the left column (rows 0..8, quest and avatar
                 * counters) changes every time the cursor moves, so pasting it
                 * looks exactly like an unstable readout.
                 *
                 * The sixth row is labelled `E`, not `5`, on purpose: the
                 * legacy column already owns 0..8, so a `5` would be a second
                 * collision on the same scanline -- exactly the ambiguity that
                 * made "row 4" unanswerable. */
                const unsigned* optRows[7] = {&s_hoverRow0, &s_hoverRow1,
                                              &s_hoverRow2, &s_hoverRow3,
                                              &s_hoverRow4, &s_hoverRow5,
                                              &s_hoverRow6};
                for (int i = 0; i < 7; i++) {
                    hexnum(bounds[0] + 420, bounds[1] + 100 + i * 34,
                           static_cast<unsigned>(
                               i < 5 ? i : (i == 5 ? 0xE : 0xF)), 1, 4);
                    hexnum(bounds[0] + 444, bounds[1] + 100 + i * 34,
                           *optRows[i], 8, 4);
                }
                /* The stage tag stays on the y = 236 line, which is a line we
                 * know is on screen (it used to hold it at x = 444).  It just
                 * moves LEFT of the row numbers, into space the readout never
                 * used, because row 4 now occupies 444.. on that line.  Moving
                 * it down instead would have risked clipping, and the tag is
                 * the only thing that proves which build is installed. */
                hexnum(bounds[0] + 300, bounds[1] + 236, StageTag(kMapStage),
                       3, 4);
            }

            return;
        }

        /* Every location on the world map, using a table generated offline
         * from Data/WorldMap.xnb and the map tbin files (PC 1.6.15).
         *
         *   pixel  = (rect * 4).xy + (rect * 4).wh * (tile / mapSize)
         *   screen = mapBounds + pixel + 32
         *
         * The rectangles in the asset are in base-map pixels and the game draws
         * them at 4x; that is the same rect the game caches at MapPage+0x68
         * (farm: 68,62,50,34 -> 272,248,200,136, exactly what we measured). */
        auto getLocations = reinterpret_cast<GetLocationsFn>(g_base + kGetLocationsOff);
        void* locList = getLocations();
        if (locList == nullptr) {
            return;
        }
        void** locs = *reinterpret_cast<void***>(
            reinterpret_cast<unsigned char*>(locList) + 0x10);
        const int locCount = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(locList) + 0x18);

        /* Stage 74: the offset scan, redone with the read that is proven.
         *
         * `locs` is obtained as
         *     void** locs = *(void***)((char*)locList + 0x10);
         *     int    n    = *(int*)((char*)locList + 0x18);
         * one hop to the array -- and that is the read which returns all 83 real
         * locations, names matching Data/WorldMap.  Every scan since stage 61
         * instead used a two-hop read, *(*(list+0x10)+0x10), which is not how a
         * List is laid out.  So those scans never inspected elements at all;
         * they inspected the wrapper's own fields.  That single inconsistency
         * explains why every criterion -- name, position, method table, count --
         * failed at once.
         *
         * This build rescans with the proven one-hop read and keeps an offset
         * only if it looks like a villager collection:
         *   - 5..60 elements on at least two locations (NPCs are spread out)
         *   - 20..250 elements worldwide
         *   - element[0] shares one MethodTable across every location
         * It then measures a name offset, a texture offset and a position offset
         * on the fullest location, and draws a marker per element so the
         * positions can be judged at a glance.
         *
         * Rows:
         *   0: offset (4) / locations (2) / total elements (2)
         *   1: name offset (4) / first two chars (4)
         *   2: texture offset (4) / locations agreeing (2)
         *   3: markers drawn (4) / distinct tiles (2)
         *   4: build tag, 74
         */
        if (kMapStage >= 40 && kMapStage < 90) {
            static int s_done = 0;
            static unsigned s_r0 = 0;
            static unsigned s_r1 = 0;
            static unsigned s_r2 = 0;
            static unsigned s_r3 = 0;

            auto arrOf = [](void* list) -> void* {
                if (!Plausible(list)) {
                    return nullptr;
                }
                void* a = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(list) + 0x10);
                return Plausible(a) ? a : nullptr;
            };
            auto cntOf = [](void* list) -> int {
                if (!Plausible(list)) {
                    return -1;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(list) + 0x18);
                return (n < 0 || n > 4096) ? -1 : n;
            };

            if (s_done == 0) {
                s_done = 1;

                void* mtTex = Plausible(texture)
                    ? *reinterpret_cast<void**>(texture)
                    : nullptr;

                if (locs != nullptr && locCount > 0 && locCount <= 256) {
                    int bestOff = -1;
                    int bestMaps = 0;
                    int bestTotal = 0;

                    for (int o = 0x08; o <= 0x3000; o += 8) {
                        int maps = 0;
                        int total = 0;
                        void* mt0 = nullptr;
                        bool uniform = true;
                        for (int i = 0; i < locCount; i++) {
                            void* l = locs[i];
                            if (!Plausible(l)) {
                                continue;
                            }
                            void* p = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(l) + o);
                            if (!Plausible(p)) {
                                continue;
                            }
                            const int n = cntOf(p);
                            if (n <= 0 || n > 60) {
                                continue;
                            }
                            void* arr = arrOf(p);
                            if (arr == nullptr) {
                                continue;
                            }
                            void* e = reinterpret_cast<void**>(arr)[0];
                            if (!Plausible(e)) {
                                continue;
                            }
                            void* mt = *reinterpret_cast<void**>(e);
                            if (mt0 == nullptr) {
                                mt0 = mt;
                            }
                            else if (mt != mt0) {
                                uniform = false;
                                break;
                            }
                            maps++;
                            total += n;
                        }
                        if (!uniform || maps < 2 || total < 20 || total > 250) {
                            continue;
                        }
                        if (total > bestTotal) {
                            bestTotal = total;
                            bestMaps = maps;
                            bestOff = o;
                        }
                    }

                    s_r0 = ((static_cast<unsigned>(bestOff) & 0xFFFFu) << 16) |
                        ((static_cast<unsigned>(bestMaps) & 0xFFu) << 8) |
                        (static_cast<unsigned>(bestTotal) & 0xFFu);

                    if (bestOff >= 0) {
                        /* The location holding the most elements is the best
                         * specimen for measuring fields. */
                        int richLoc = -1;
                        int richN = 0;
                        for (int i = 0; i < locCount; i++) {
                            void* l = locs[i];
                            if (!Plausible(l)) {
                                continue;
                            }
                            void* p = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(l) + bestOff);
                            const int n = cntOf(p);
                            if (n > richN) {
                                richN = n;
                                richLoc = i;
                            }
                        }

                        int nameOff = -1;
                        int texOff = -1;
                        int texAgree = 0;
                        int posOff = -1;
                        int posIsInt = 0;
                        int distinct = 0;

                        if (richLoc >= 0) {
                            void* l = locs[richLoc];
                            void* p = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(l) + bestOff);
                            void* arr = arrOf(p);
                            void** items = reinterpret_cast<void**>(arr);

                            /* Name: letter-only managed string, 3..20 chars. */
                            for (int f = 0x08; f <= 0x400 && nameOff < 0; f += 8) {
                                void* s = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(items[0]) + f);
                                if (!Plausible(s)) {
                                    continue;
                                }
                                const int len = *reinterpret_cast<int*>(
                                    reinterpret_cast<unsigned char*>(s) + 0x10);
                                if (len < 3 || len > 20) {
                                    continue;
                                }
                                const unsigned char* ch =
                                    reinterpret_cast<const unsigned char*>(s) + 0x14;
                                bool ok = true;
                                for (int k = 0; k < len && ok; k++) {
                                    const unsigned u = ch[k * 2] |
                                        (static_cast<unsigned>(ch[k * 2 + 1]) << 8);
                                    const bool letter =
                                        (u >= 'A' && u <= 'Z') ||
                                        (u >= 'a' && u <= 'z');
                                    if (!letter && u != ' ') {
                                        ok = false;
                                    }
                                }
                                if (ok) {
                                    nameOff = f;
                                    s_r1 = (static_cast<unsigned>(f) << 16) |
                                        (static_cast<unsigned>(ch[0]) << 8) |
                                        static_cast<unsigned>(ch[2]);
                                }
                            }

                            /* Texture: a field whose object carries the
                             * Texture2D method table (staminaRect's). */
                            if (mtTex != nullptr) {
                                for (int i = 0; i < locCount; i++) {
                                    void* li = locs[i];
                                    if (!Plausible(li)) {
                                        continue;
                                    }
                                    void* pi = *reinterpret_cast<void**>(
                                        reinterpret_cast<unsigned char*>(li) +
                                        bestOff);
                                    const int n = cntOf(pi);
                                    if (n <= 0 || n > 60) {
                                        continue;
                                    }
                                    void* ai = arrOf(pi);
                                    if (ai == nullptr) {
                                        continue;
                                    }
                                    void* e0 = reinterpret_cast<void**>(ai)[0];
                                    if (!Plausible(e0)) {
                                        continue;
                                    }
                                    for (int f = 0x08; f <= 0x400; f += 8) {
                                        void* v = *reinterpret_cast<void**>(
                                            reinterpret_cast<unsigned char*>(e0) +
                                            f);
                                        if (!Plausible(v)) {
                                            continue;
                                        }
                                        if (*reinterpret_cast<void**>(v) != mtTex) {
                                            continue;
                                        }
                                        if (texOff < 0) {
                                            texOff = f;
                                        }
                                        if (f == texOff) {
                                            texAgree++;
                                        }
                                        break;
                                    }
                                }
                            }

                            /* Position: the field whose values spread out over
                             * distinct tiles -- a constant would collapse. */
                            for (int mode = 0; mode < 2; mode++) {
                                for (int f = 0x08; f <= 0x400; f += 4) {
                                    int seenX[64];
                                    int seenY[64];
                                    int ns = 0;
                                    for (int j = 0; j < richN && ns < 64; j++) {
                                        void* c = items[j];
                                        if (!Plausible(c)) {
                                            continue;
                                        }
                                        int tx = 0;
                                        int ty = 0;
                                        if (mode == 0) {
                                            const float fx = *reinterpret_cast<float*>(
                                                reinterpret_cast<unsigned char*>(c) + f);
                                            const float fy = *reinterpret_cast<float*>(
                                                reinterpret_cast<unsigned char*>(c) + f + 4);
                                            if (fx < 64.0f || fy < 64.0f ||
                                                fx > 12800.0f || fy > 12800.0f) {
                                                continue;
                                            }
                                            tx = static_cast<int>(fx / 64.0f);
                                            ty = static_cast<int>(fy / 64.0f);
                                        }
                                        else {
                                            tx = *reinterpret_cast<int*>(
                                                reinterpret_cast<unsigned char*>(c) + f);
                                            ty = *reinterpret_cast<int*>(
                                                reinterpret_cast<unsigned char*>(c) + f + 4);
                                        }
                                        if (tx <= 0 || ty <= 0 || tx >= 200 ||
                                            ty >= 200) {
                                            continue;
                                        }
                                        bool dup = false;
                                        for (int q = 0; q < ns; q++) {
                                            if (seenX[q] == tx && seenY[q] == ty) {
                                                dup = true;
                                            }
                                        }
                                        if (!dup) {
                                            seenX[ns] = tx;
                                            seenY[ns] = ty;
                                            ns++;
                                        }
                                    }
                                    if (ns > distinct) {
                                        distinct = ns;
                                        posOff = f;
                                        posIsInt = mode;
                                    }
                                }
                            }

                            /* Markers, using the proven five-argument draw. */
                            int drawn = 0;
                            if (posOff >= 0 && distinct >= 3) {
                                for (int i = 0; i < locCount && drawn < 256; i++) {
                                    void* li = locs[i];
                                    if (!Plausible(li)) {
                                        continue;
                                    }
                                    void* pi = *reinterpret_cast<void**>(
                                        reinterpret_cast<unsigned char*>(li) +
                                        bestOff);
                                    const int n = cntOf(pi);
                                    if (n <= 0 || n > 60) {
                                        continue;
                                    }
                                    void* ai = arrOf(pi);
                                    if (ai == nullptr) {
                                        continue;
                                    }
                                    void* nm = *reinterpret_cast<void**>(
                                        reinterpret_cast<unsigned char*>(li) +
                                        0x1A8);
                                    if (!Plausible(nm)) {
                                        continue;
                                    }
                                    const WorldMapEntry* entry = nullptr;
                                    for (int w = 0; w < 2 && entry == nullptr; w++) {
                                        void* cand = nm;
                                        if (w == 1) {
                                            cand = *reinterpret_cast<void**>(
                                                reinterpret_cast<unsigned char*>(nm) +
                                                0x68);
                                        }
                                        if (!Plausible(cand)) {
                                            continue;
                                        }
                                        for (int k = 0; k < kWorldMapCount; k++) {
                                            if (NameEquals(cand, kWorldMap[k].name)) {
                                                entry = &kWorldMap[k];
                                                break;
                                            }
                                        }
                                    }
                                    if (entry == nullptr) {
                                        continue;
                                    }
                                    const float rx =
                                        static_cast<float>(entry->rectX) * 4.0f;
                                    const float ry =
                                        static_cast<float>(entry->rectY) * 4.0f;
                                    const float rw =
                                        static_cast<float>(entry->rectW) * 4.0f;
                                    const float rh =
                                        static_cast<float>(entry->rectH) * 4.0f;
                                    const float mw = static_cast<float>(entry->mapW);
                                    const float mh = static_cast<float>(entry->mapH);
                                    if (rw < 1.0f || rh < 1.0f || mw < 1.0f ||
                                        mh < 1.0f) {
                                        continue;
                                    }
                                    for (int j = 0; j < n && drawn < 256; j++) {
                                        void* c = reinterpret_cast<void**>(ai)[j];
                                        if (!Plausible(c)) {
                                            continue;
                                        }
                                        int tx = 0;
                                        int ty = 0;
                                        if (posIsInt != 0) {
                                            tx = *reinterpret_cast<int*>(
                                                reinterpret_cast<unsigned char*>(c) +
                                                posOff);
                                            ty = *reinterpret_cast<int*>(
                                                reinterpret_cast<unsigned char*>(c) +
                                                posOff + 4);
                                        }
                                        else {
                                            const float fx = *reinterpret_cast<float*>(
                                                reinterpret_cast<unsigned char*>(c) +
                                                posOff);
                                            const float fy = *reinterpret_cast<float*>(
                                                reinterpret_cast<unsigned char*>(c) +
                                                posOff + 4);
                                            tx = static_cast<int>(fx / 64.0f);
                                            ty = static_cast<int>(fy / 64.0f);
                                        }
                                        if (tx <= 0 || ty <= 0) {
                                            continue;
                                        }
                                        const float px =
                                            rx + rw * (static_cast<float>(tx) / mw);
                                        const float py =
                                            ry + rh * (static_cast<float>(ty) / mh);
                                        const u32 mx = static_cast<u32>(
                                            static_cast<float>(bounds[0]) + px +
                                            32.0f);
                                        const u32 my = static_cast<u32>(
                                            static_cast<float>(bounds[1]) + py +
                                            32.0f);
                                        draw(batch, texture,
                                             (static_cast<u64>(my) << 32) | mx,
                                             (12ull << 32) | 12u, 0xFFFF00FFu);
                                        drawn++;
                                    }
                                }
                            }

                            s_r2 = ((static_cast<unsigned>(texOff) & 0xFFFFu) << 16) |
                                (static_cast<unsigned>(texAgree) & 0xFFu);
                            s_r3 = ((static_cast<unsigned>(drawn) & 0xFFFFu) << 16) |
                                (static_cast<unsigned>(distinct) & 0xFFu);
                        }
                    }
                }
            }

            hexnum(bounds[0] + 120, bounds[1] + 110, s_r0, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 160, s_r1, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 210, s_r2, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 260, s_r3, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 310, 0x74, 2, 4);
            return;
        }

        /* Stage 38: identify the Character and its sprite texture by TYPE, not
         * by offset guesswork.
         *
         * The marker square is drawn with Game1.staminaRect, a 1x1 white
         * Texture2D, so its MethodTable IS the Texture2D MethodTable.  A managed
         * object's first word is its MethodTable, therefore "first word equals
         * mtTex" is a check that cannot produce a false positive the way an
         * offset guess can -- and unlike a draw attempt, it cannot crash the
         * console either.
         *
         * Four rows of hex:
         *   0: name field offset, name length          (0800 0500 -> +0x08, 5)
         *   1: first two UTF-16 code units of that name (4162 -> "Ab")
         *   2: sprite offset inside Character, texture offset inside the sprite
         *   3: unwrap path code (0..7), plausible pointer fields on the object
         *
         * Reading row 3 tells us whether items[0] is a rich object (a Character
         * has ~20 pointer fields) or a stub like MapAreaPosition (1..3). */
        if (kMapStage == 38) {
            unsigned nameOff = 0xFFFFu, nameLen = 0, ch0 = 0, ch1 = 0;
            unsigned outerOff = 0xFFFFu, innerOff = 0xFFFFu, path = 0xFFFFu;
            unsigned ptrFields = 0;

            void* mapTex = *reinterpret_cast<void**>(g_base + kStaminaRectOff);
            void* mtTex = Plausible(mapTex)
                ? *reinterpret_cast<void**>(mapTex)
                : nullptr;

            /* Town has the densest character list, and its first slot is the
             * least likely to be empty. */
            void* c = nullptr;
            for (int i = 0; i < locCount && c == nullptr; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                if (n <= 0 || n > 512) {
                    continue;
                }
                for (int j = 0; j < n && c == nullptr; j++) {
                    void* cand = reinterpret_cast<void**>(items)[j];
                    if (Plausible(cand)) {
                        c = cand;
                    }
                }
            }

            if (c != nullptr) {
                unsigned char* ob = reinterpret_cast<unsigned char*>(c);

                /* 1. The name is a plain managed string: +0x10 length, +0x14
                 * UTF-16 chars.  Anything printable and 2..32 chars long is a
                 * name; NPC names are short and ASCII. */
                for (int o = 0x08; o <= 0x200; o += 8) {
                    void* s = *reinterpret_cast<void**>(ob + o);
                    if (!Plausible(s)) {
                        continue;
                    }
                    const int len = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(s) + 0x10);
                    if (len < 2 || len > 32) {
                        continue;
                    }
                    const unsigned char* ch =
                        reinterpret_cast<const unsigned char*>(s) + 0x14;
                    bool ok = true;
                    for (int k = 0; k < len; k++) {
                        const unsigned u = ch[k * 2] |
                            (static_cast<unsigned>(ch[k * 2 + 1]) << 8);
                        if (u < 0x20u || u > 0x7Eu) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) {
                        continue;
                    }
                    nameOff = static_cast<unsigned>(o);
                    nameLen = static_cast<unsigned>(len);
                    ch0 = ch[0];
                    ch1 = ch[2];
                    break;
                }

                /* 2. How rich is this object?  A Character is full of pointers. */
                for (int o = 0x08; o <= 0x300; o += 8) {
                    if (Plausible(*reinterpret_cast<void**>(ob + o))) {
                        ptrFields++;
                    }
                }

                /* 3. Sprite -> texture.  Character.Sprite may be a plain field,
                 * a NetRef (+0x68), or sit behind a wrapper (+0x10), so try the
                 * four shapes and let the MethodTable decide which one is real. */
                for (int o = 0x08; o <= 0x300 && outerOff == 0xFFFFu; o += 8) {
                    void* p1 = *reinterpret_cast<void**>(ob + o);
                    if (!Plausible(p1)) {
                        continue;
                    }
                    void* cont[4];
                    cont[0] = p1;
                    cont[1] = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(p1) + 0x68);
                    cont[2] = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(p1) + 0x10);
                    cont[3] = Plausible(cont[1])
                        ? *reinterpret_cast<void**>(
                              reinterpret_cast<unsigned char*>(cont[1]) + 0x10)
                        : nullptr;
                    for (int k = 0; k < 4 && outerOff == 0xFFFFu; k++) {
                        if (!Plausible(cont[k])) {
                            continue;
                        }
                        unsigned char* cb =
                            reinterpret_cast<unsigned char*>(cont[k]);
                        for (int in = 0x08; in <= 0x200; in += 8) {
                            void* q = *reinterpret_cast<void**>(cb + in);
                            if (!Plausible(q)) {
                                continue;
                            }
                            void* cand[2];
                            cand[0] = q;
                            cand[1] = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(q) + 0x68);
                            for (int w = 0; w < 2; w++) {
                                if (!Plausible(cand[w]) || mtTex == nullptr) {
                                    continue;
                                }
                                if (*reinterpret_cast<void**>(cand[w]) != mtTex) {
                                    continue;
                                }
                                outerOff = static_cast<unsigned>(o);
                                innerOff = static_cast<unsigned>(in);
                                path = static_cast<unsigned>(k * 2 + w);
                                break;
                            }
                            if (outerOff != 0xFFFFu) {
                                break;
                            }
                        }
                    }
                }
            }

            hexnum(bounds[0] + 120, bounds[1] + 100,
                   (nameOff << 16) | (nameLen & 0xFFFFu), 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 140,
                   (ch0 << 8) | ch1, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 180,
                   (outerOff << 16) | (innerOff & 0xFFFFu), 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 220,
                   (path << 8) | (ptrFields & 0xFFu), 4, 4);

            /* If a Texture2D was actually reached, paint it on the map.  A
             * whole sprite sheet squeezed into a 96px box is instantly
             * recognisable, so this answers "did we find the right texture"
             * in the same run as the hex readout instead of costing another
             * build/test cycle.  Safe to draw: the pointer was validated by
             * MethodTable equality, so it really is a Texture2D. */
            if (outerOff != 0xFFFFu) {
                void* p1 = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(c) + outerOff);
                void* cont = p1;
                const unsigned kk = path >> 1;
                if (kk == 1) {
                    cont = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(p1) + 0x68);
                }
                else if (kk == 2) {
                    cont = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(p1) + 0x10);
                }
                else if (kk == 3) {
                    void* t = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(p1) + 0x68);
                    cont = Plausible(t)
                        ? *reinterpret_cast<void**>(
                              reinterpret_cast<unsigned char*>(t) + 0x10)
                        : nullptr;
                }
                void* tex = nullptr;
                if (Plausible(cont)) {
                    tex = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(cont) + innerOff);
                    if ((path & 1u) != 0u && Plausible(tex)) {
                        tex = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(tex) + 0x68);
                    }
                }
                if (Plausible(tex)) {
                    const u64 sxy = (static_cast<u64>(bounds[1] + 120) << 32) |
                                    static_cast<u32>(bounds[0] + 420);
                    draw(batch, tex, sxy, (96ull << 32) | 96u, 0xFFFFFFFFu);
                }
            }
            return;
        }

        /* Stage 37: find the sprite by content instead of by guesswork.  A real
         * Character holds a pointer chain that ends at a managed string starting
         * with "Characters/".  Walk the object's pointer fields, and for each
         * target walk its pointer fields (with and without a +0x68 NetRef
         * unwrap) looking for such a string.
         * Prints: outer offset, inner offset, and how many elements were tried. */
        if (kMapStage == 37) {
            unsigned outerOff = 0xFFFFu;
            unsigned innerOff = 0xFFFFu;
            unsigned unwrapped = 0;
            for (int i = 0; i < locCount && outerOff == 0xFFFFu; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                void* c = reinterpret_cast<void**>(items)[0];
                if (!Plausible(c)) {
                    continue;
                }
                const unsigned char* ob = reinterpret_cast<const unsigned char*>(c);
                for (int o = 0x08; o <= 0x300 && outerOff == 0xFFFFu; o += 8) {
                    void* p1 = *reinterpret_cast<void**>(
                        const_cast<unsigned char*>(ob) + o);
                    if (!Plausible(p1)) {
                        continue;
                    }
                    for (int shape = 0; shape < 2 && outerOff == 0xFFFFu; shape++) {
                        void* t = p1;
                        if (shape == 1) {
                            t = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(p1) + 0x68);
                            if (!Plausible(t)) {
                                continue;
                            }
                        }
                        const unsigned char* tb =
                            reinterpret_cast<const unsigned char*>(t);
                        for (int in2 = 0x08; in2 <= 0x200; in2 += 8) {
                            void* q = *reinterpret_cast<void**>(
                                const_cast<unsigned char*>(tb) + in2);
                            if (!Plausible(q)) {
                                continue;
                            }
                            void* cand[2] = {q, nullptr};
                            if (Plausible(q)) {
                                cand[1] = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(q) + 0x68);
                            }
                            for (int w = 0; w < 2; w++) {
                                void* str = cand[w];
                                if (!Plausible(str)) {
                                    continue;
                                }
                                const int len = *reinterpret_cast<int*>(
                                    reinterpret_cast<unsigned char*>(str) + 0x10);
                                if (len < 8 || len > 64) {
                                    continue;
                                }
                                const unsigned char* ch =
                                    reinterpret_cast<const unsigned char*>(str) + 0x14;
                                const char* pre = "Characters/";
                                bool ok = true;
                                int k2 = 0;
                                for (; pre[k2] != '\0'; k2++) {
                                    if (k2 >= len || ch[k2 * 2] !=
                                            static_cast<unsigned char>(pre[k2])) {
                                        ok = false;
                                        break;
                                    }
                                }
                                if (!ok) {
                                    continue;
                                }
                                outerOff = static_cast<unsigned>(o);
                                innerOff = static_cast<unsigned>(in2);
                                unwrapped = static_cast<unsigned>(shape * 2 + w);
                                break;
                            }
                            if (outerOff != 0xFFFFu) {
                                break;
                            }
                        }
                    }
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 110, outerOff, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 145, innerOff, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 180, unwrapped, 2, 4);
            return;
        }

        /* Stage 36: Game1.locations is a GC static, so its address is derived
         * from an index rather than being a fixed offset.  Scan the statics
         * region (.data, 0x47B8000..0x47C0000) for slots that hold a List with
         * 20..200 elements -- Game1.locations should be one of very few.
         * Prints: number of candidate slots, then the first three slot offsets. */
        if (kMapStage == 36) {
            unsigned found = 0;
            unsigned first[3] = {0, 0, 0};
            for (unsigned off = 0x47B8000u; off < 0x47C0000u; off += 8u) {
                void* p = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(g_base) + off);
                if (!Plausible(p)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(p) + 0x18);
                if (n < 20 || n > 200) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(p) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                if (!Plausible(reinterpret_cast<void**>(items)[0])) {
                    continue;
                }
                if (found < 3) {
                    first[found] = off;
                }
                found++;
            }
            hexnum(bounds[0] + 120, bounds[1] + 110, found, 4, 4);
            for (int k = 0; k < 3; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 145 + k * 30, first[k], 6, 4);
            }
            return;
        }

        /* Stage 35: the game's own code at 0x6386E0 proves the real shape:
         *   ldr x0, [x28, #0x108]   -> the List itself
         *   ldr w19, [x0, #0x18]    -> count
         *   bl 0x5d3c8c             -> List.get_Item
         * No +0x68 unwrap and no extra +0x10 hop.  Print the count, the first
         * element's +0x14/+0x18, and the vtable 0xE0 slot offset. */
        if (kMapStage == 35) {
            unsigned cnt = 0;
            unsigned tx0 = 0;
            unsigned ty0 = 0;
            unsigned slotOff = 0;
            int ok = 0;
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* list = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(list)) {
                    break;
                }
                cnt = static_cast<unsigned>(*reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(list) + 0x18));
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(list) + 0x10);
                if (!Plausible(items)) {
                    break;
                }
                void* c = reinterpret_cast<void**>(items)[0];
                if (!Plausible(c)) {
                    break;
                }
                tx0 = static_cast<unsigned>(*reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(c) + 0x14));
                ty0 = static_cast<unsigned>(*reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(c) + 0x18));
                void* mt = *reinterpret_cast<void**>(c);
                if (Plausible(mt)) {
                    void* fn = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(mt) + 0xE0);
                    if (Plausible(fn)) {
                        const uintptr_t f = reinterpret_cast<uintptr_t>(fn);
                        const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
                        if (f >= base && f - base < 0x2493190ull) {
                            slotOff = static_cast<unsigned>(f - base);
                        }
                    }
                }
                ok = 1;
                break;
            }
            hexnum(bounds[0] + 120, bounds[1] + 100, cnt, 2, 4);
            hexnum(bounds[0] + 120, bounds[1] + 130, tx0, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 160, ty0, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 190, slotOff, 6, 4);
            hexnum(bounds[0] + 120, bounds[1] + 220, static_cast<unsigned>(ok), 2, 4);
            return;
        }

        /* Stage 34: call the virtual slot for Character.get_Sprite (0xE0, the
         * same slot the community Switch port uses) instead of guessing a field
         * offset.  Virtual dispatch is ABI, so it survives version differences.
         * Guarded: only call if the slot actually points into the module's text.
         * Prints the slot offset from base, then the result's first 8 words. */
        if (kMapStage == 34) {
            unsigned slotOff = 0;
            unsigned words[8] = {0};
            int called = 0;
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                void* c = reinterpret_cast<void**>(items)[0];
                if (!Plausible(c)) {
                    continue;
                }
                void* mt = *reinterpret_cast<void**>(c);
                if (!Plausible(mt)) {
                    break;
                }
                void* fn = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(mt) + 0xE0);
                if (!Plausible(fn)) {
                    break;
                }
                const uintptr_t f = reinterpret_cast<uintptr_t>(fn);
                const uintptr_t base = reinterpret_cast<uintptr_t>(g_base);
                if (f < base || f - base > 0x2493190ull) {
                    slotOff = 0xDEADu;
                    break;
                }
                slotOff = static_cast<unsigned>(f - base);
                using VirtFn = void* (*)(void*);
                void* sprite = reinterpret_cast<VirtFn>(fn)(c);
                called = 1;
                if (Plausible(sprite)) {
                    const unsigned char* b =
                        reinterpret_cast<const unsigned char*>(sprite);
                    for (int k = 0; k < 8; k++) {
                        __builtin_memcpy(&words[k], b + k * 4, 4);
                    }
                }
                break;
            }
            hexnum(bounds[0] + 120, bounds[1] + 100, slotOff, 6, 4);
            hexnum(bounds[0] + 120, bounds[1] + 130, static_cast<unsigned>(called), 2, 4);
            for (int k = 0; k < 8; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 160 + k * 28, words[k], 8, 4);
            }
            return;
        }

        /* Stage 33: the element at items[j] is not a Character (its tile lives
         * in two ints at +0x14/+0x18, while Character.Position is floats).  It
         * carries a pointer at +0x20 -- dump that target's first 10 words to see
         * whether the real NPC lives there. */
        if (kMapStage == 33) {
            unsigned words[10] = {0};
            bool ok = false;
            for (int i = 0; i < locCount && !ok; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                void* c = reinterpret_cast<void**>(items)[0];
                if (!Plausible(c)) {
                    continue;
                }
                void* target = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(c) + 0x20);
                if (!Plausible(target)) {
                    continue;
                }
                const unsigned char* b = reinterpret_cast<const unsigned char*>(target);
                for (int k = 0; k < 10; k++) {
                    __builtin_memcpy(&words[k], b + k * 4, 4);
                }
                ok = true;
            }
            for (int k = 0; k < 10; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 100 + k * 28, words[k], 8, 4);
            }
            return;
        }

        /* Stage 32: the raw dump of items[0] showed two small ints at +0x14 and
         * +0x18 (52 and 19) -- exactly the shape of a tile coordinate.  Print
         * them for three elements: if they are in tile range and differ per
         * element, that is the NPC's tile. */
        if (kMapStage == 32) {
            unsigned out[6] = {0, 0, 0, 0, 0, 0};
            int got = 0;
            for (int i = 0; i < locCount && got < 3; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                for (int j = 0; j < n && got < 3; j++) {
                    void* c = reinterpret_cast<void**>(items)[j];
                    if (!Plausible(c)) {
                        continue;
                    }
                    const unsigned char* b = reinterpret_cast<const unsigned char*>(c);
                    unsigned v14 = 0;
                    unsigned v18 = 0;
                    __builtin_memcpy(&v14, b + 0x14, 4);
                    __builtin_memcpy(&v18, b + 0x18, 4);
                    out[got * 2] = v14;
                    out[got * 2 + 1] = v18;
                    got++;
                }
            }
            for (int k = 0; k < 6; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 130 + k * 30, out[k], 4, 4);
            }
            hexnum(bounds[0] + 120, bounds[1] + 130 + 6 * 30,
                   static_cast<unsigned>(got), 2, 4);
            return;
        }

        /* Stage 31: raw dump of items[0], 16 words (64 bytes).  A Character
         * object is full of object references (Sprite, currentLocation, ...),
         * so the words should be mostly 0x0-0x7xxxxxxxx pointers.  If they are
         * not, the list we found is not characters after all. */
        if (kMapStage == 31) {
            unsigned words[16] = {0};
            bool ok = false;
            for (int i = 0; i < locCount && !ok; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                void* c = reinterpret_cast<void**>(items)[0];
                if (!Plausible(c)) {
                    continue;
                }
                const unsigned char* b = reinterpret_cast<const unsigned char*>(c);
                for (int k = 0; k < 16; k++) {
                    __builtin_memcpy(&words[k], b + k * 4, 4);
                }
                ok = true;
            }
            for (int k = 0; k < 16; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 100 + k * 26, words[k], 8, 4);
            }
            return;
        }

        /* Stage 30: Position is neither inline nor behind the element wrapper,
         * so it is probably a pointer to a NetVector2-style object.  Scan each
         * pointer field of the NPC, then look inside its target for a float
         * pair that satisfies the pixel-coordinate test.
         * Prints: field offset, inner offset, X0..2, npc count. */
        if (kMapStage == 30) {
            void* npcs[3] = {nullptr, nullptr, nullptr};
            int got = 0;
            for (int i = 0; i < locCount && got < 3; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                for (int j = 0; j < n && got < 3; j++) {
                    void* c = reinterpret_cast<void**>(items)[j];
                    if (Plausible(c)) {
                        npcs[got++] = c;
                    }
                }
            }
            unsigned fOff = 0xFFFFu;
            unsigned iOff = 0xFFFFu;
            unsigned out[3] = {0, 0, 0};
            if (got == 3) {
                for (int off = 0x08; off <= 0x200 && fOff == 0xFFFFu; off += 8) {
                    void* targets[3] = {nullptr, nullptr, nullptr};
                    bool ok = true;
                    for (int k = 0; k < 3 && ok; k++) {
                        void* tp = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(npcs[k]) + off);
                        if (!Plausible(tp)) {
                            ok = false;
                            break;
                        }
                        targets[k] = tp;
                    }
                    if (!ok) {
                        continue;
                    }
                    for (int in = 0x00; in <= 0x20; in += 4) {
                        float f[3] = {0, 0, 0};
                        float g[3] = {0, 0, 0};
                        bool good = true;
                        for (int k = 0; k < 3 && good; k++) {
                            const unsigned char* b =
                                reinterpret_cast<const unsigned char*>(targets[k]);
                            __builtin_memcpy(&f[k], b + in, 4);
                            __builtin_memcpy(&g[k], b + in + 4, 4);
                            if (!(f[k] > 256.0f && f[k] < 8000.0f)) {
                                good = false;
                            }
                            if (!(g[k] > 256.0f && g[k] < 8000.0f)) {
                                good = false;
                            }
                        }
                        if (!good) {
                            continue;
                        }
                        bool distinct = true;
                        for (int a = 0; a < 3 && distinct; a++) {
                            for (int b2 = a + 1; b2 < 3; b2++) {
                                const float dx = f[a] - f[b2];
                                const float dy = g[a] - g[b2];
                                const float ax = dx < 0 ? -dx : dx;
                                const float ay = dy < 0 ? -dy : dy;
                                if (ax < 2.0f && ay < 2.0f) {
                                    distinct = false;
                                }
                            }
                        }
                        if (!distinct) {
                            continue;
                        }
                        fOff = static_cast<unsigned>(off);
                        iOff = static_cast<unsigned>(in);
                        for (int k = 0; k < 3; k++) {
                            out[k] = static_cast<unsigned>(f[k]);
                        }
                        break;
                    }
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 120, fOff, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 150, iOff, 4, 4);
            for (int k = 0; k < 3; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 180 + k * 30, out[k], 4, 4);
            }
            hexnum(bounds[0] + 120, bounds[1] + 300, static_cast<unsigned>(got), 2, 4);
            return;
        }

        /* Stage 29: no inline float pair anywhere in 0x08..0x400, yet all four
         * element MethodTables were identical -- consistent with items[j] being
         * a NetRef wrapper whose payload is at +0x68.  Unwrap one more level and
         * repeat the strict scan.
         * Prints: offset, X0..2, Y0..2, npc count. */
        if (kMapStage == 29) {
            void* npcs[3] = {nullptr, nullptr, nullptr};
            int got = 0;
            for (int i = 0; i < locCount && got < 3; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                for (int j = 0; j < n && got < 3; j++) {
                    void* ref = reinterpret_cast<void**>(items)[j];
                    if (!Plausible(ref)) {
                        continue;
                    }
                    void* npc = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(ref) + 0x68);
                    if (Plausible(npc)) {
                        npcs[got++] = npc;
                    }
                }
            }
            unsigned foundOff = 0xFFFFu;
            unsigned out[6] = {0, 0, 0, 0, 0, 0};
            if (got == 3) {
                for (int off = 0x08; off <= 0x600; off += 4) {
                    float f[3] = {0, 0, 0};
                    float g[3] = {0, 0, 0};
                    bool ok = true;
                    for (int k = 0; k < 3 && ok; k++) {
                        const unsigned char* b =
                            reinterpret_cast<const unsigned char*>(npcs[k]);
                        __builtin_memcpy(&f[k], b + off, 4);
                        __builtin_memcpy(&g[k], b + off + 4, 4);
                        if (!(f[k] > 256.0f && f[k] < 8000.0f)) {
                            ok = false;
                        }
                        if (!(g[k] > 256.0f && g[k] < 8000.0f)) {
                            ok = false;
                        }
                    }
                    if (!ok) {
                        continue;
                    }
                    bool distinct = true;
                    for (int a = 0; a < 3 && distinct; a++) {
                        for (int b2 = a + 1; b2 < 3; b2++) {
                            const float dx = f[a] - f[b2];
                            const float dy = g[a] - g[b2];
                            const float ax = dx < 0 ? -dx : dx;
                            const float ay = dy < 0 ? -dy : dy;
                            if (ax < 2.0f && ay < 2.0f) {
                                distinct = false;
                            }
                        }
                    }
                    if (!distinct) {
                        continue;
                    }
                    foundOff = static_cast<unsigned>(off);
                    for (int k = 0; k < 3; k++) {
                        out[k] = static_cast<unsigned>(f[k]);
                        out[3 + k] = static_cast<unsigned>(g[k]);
                    }
                    break;
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 120, foundOff, 4, 4);
            for (int k = 0; k < 6; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 150 + k * 30, out[k], 4, 4);
            }
            hexnum(bounds[0] + 120, bounds[1] + 330, static_cast<unsigned>(got), 2, 4);
            return;
        }

        /* Stage 28: same idea as stage 27 but with a filter that only pixel
         * coordinates can pass.  Stage 27 matched offset 0x104 with values
         * 0/32/22 -- velocities or timers, not pixels.  Town NPCs sit at pixel
         * 640..5760, so require both X and Y of all three NPCs inside
         * 256..8000 AND all six values mutually different.
         * Prints: offset, X0..2, Y0..2, npc count. */
        if (kMapStage == 28) {
            void* npcs[3] = {nullptr, nullptr, nullptr};
            int got = 0;
            for (int i = 0; i < locCount && got < 3; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                for (int j = 0; j < n && got < 3; j++) {
                    void* c = reinterpret_cast<void**>(items)[j];
                    if (Plausible(c)) {
                        npcs[got++] = c;
                    }
                }
            }
            unsigned foundOff = 0xFFFFu;
            unsigned out[6] = {0, 0, 0, 0, 0, 0};
            if (got == 3) {
                for (int off = 0x08; off <= 0x400; off += 4) {
                    float f[3] = {0, 0, 0};
                    float g[3] = {0, 0, 0};
                    bool ok = true;
                    for (int k = 0; k < 3 && ok; k++) {
                        const unsigned char* b =
                            reinterpret_cast<const unsigned char*>(npcs[k]);
                        __builtin_memcpy(&f[k], b + off, 4);
                        __builtin_memcpy(&g[k], b + off + 4, 4);
                        if (!(f[k] > 256.0f && f[k] < 8000.0f)) {
                            ok = false;
                        }
                        if (!(g[k] > 256.0f && g[k] < 8000.0f)) {
                            ok = false;
                        }
                    }
                    if (!ok) {
                        continue;
                    }
                    bool distinct = true;
                    for (int a = 0; a < 3 && distinct; a++) {
                        for (int b2 = a + 1; b2 < 3; b2++) {
                            const float dx = f[a] - f[b2];
                            const float dy = g[a] - g[b2];
                            const float ax = dx < 0 ? -dx : dx;
                            const float ay = dy < 0 ? -dy : dy;
                            if (ax < 2.0f && ay < 2.0f) {
                                distinct = false;
                            }
                        }
                    }
                    if (!distinct) {
                        continue;
                    }
                    foundOff = static_cast<unsigned>(off);
                    for (int k = 0; k < 3; k++) {
                        out[k] = static_cast<unsigned>(f[k]);
                        out[3 + k] = static_cast<unsigned>(g[k]);
                    }
                    break;
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 120, foundOff, 4, 4);
            for (int k = 0; k < 6; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 150 + k * 30, out[k], 4, 4);
            }
            hexnum(bounds[0] + 120, bounds[1] + 330, static_cast<unsigned>(got), 2, 4);
            return;
        }

        /* Stage 27: chain is now confirmed (four equal MethodTables), so redo
         * the position search against real NPCs:
         *   offset, X of npc0/1/2, Y of npc0
         * The X/Y pair sits 4 bytes apart; X/64 and Y/64 should land in the
         * town tile grid and differ per NPC. */
        if (kMapStage == 27) {
            void* npcs[3] = {nullptr, nullptr, nullptr};
            int got = 0;
            for (int i = 0; i < locCount && got < 3; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                for (int j = 0; j < n && got < 3; j++) {
                    void* c = reinterpret_cast<void**>(items)[j];
                    if (Plausible(c)) {
                        npcs[got++] = c;
                    }
                }
            }
            unsigned foundOff = 0xFFFFu;
            unsigned xv[3] = {0, 0, 0};
            unsigned yv = 0;
            if (got == 3) {
                for (int off = 0x08; off <= 0x300; off += 4) {
                    float f[3] = {0, 0, 0};
                    float fy = 0;
                    bool ok = true;
                    for (int k = 0; k < 3 && ok; k++) {
                        const unsigned char* b =
                            reinterpret_cast<const unsigned char*>(npcs[k]);
                        __builtin_memcpy(&f[k], b + off, 4);
                        if (!(f[k] > 0.0f && f[k] < 12800.0f)) {
                            ok = false;
                        }
                    }
                    if (!ok) {
                        continue;
                    }
                    __builtin_memcpy(&fy,
                                     reinterpret_cast<const unsigned char*>(npcs[0]) + off + 4, 4);
                    if (!(fy > 0.0f && fy < 12800.0f)) {
                        continue;
                    }
                    const float d01 = f[0] > f[1] ? f[0] - f[1] : f[1] - f[0];
                    const float d02 = f[0] > f[2] ? f[0] - f[2] : f[2] - f[0];
                    if (d01 < 1.0f || d02 < 1.0f) {
                        continue;
                    }
                    foundOff = static_cast<unsigned>(off);
                    for (int k = 0; k < 3; k++) {
                        xv[k] = static_cast<unsigned>(f[k]);
                    }
                    yv = static_cast<unsigned>(fy);
                    break;
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 130, foundOff, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 160, xv[0], 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 190, xv[1], 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 220, xv[2], 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 250, yv, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 280, static_cast<unsigned>(got), 2, 4);
            return;
        }

        /* Stage 26: the previous dump showed *(list+0x10) is the inner List
         * object, not the backing array (its word 0 was a MethodTable and word
         * 2 a pointer).  So the chain has one more hop:
         *   netRef -> +0x68 wrapper -> +0x10 inner List -> +0x10 array
         * Prints inner size, then the MethodTable of items[0..3]; four equal
         * values means these really are same-class NPCs. */
        if (kMapStage == 26) {
            unsigned mts[4] = {0, 0, 0, 0};
            unsigned size = 0;
            bool ok = false;
            for (int i = 0; i < locCount && !ok; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* wrapper = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(wrapper)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                if (!Plausible(inner)) {
                    continue;
                }
                size = static_cast<unsigned>(*reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18));
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                for (int k = 0; k < 4 && k < static_cast<int>(size); k++) {
                    void* c = reinterpret_cast<void**>(items)[k];
                    if (!Plausible(c)) {
                        mts[k] = 0xDEADBEEFu;
                        continue;
                    }
                    unsigned v = 0;
                    __builtin_memcpy(&v, c, 4);
                    mts[k] = v;
                }
                ok = true;
            }
            for (int k = 0; k < 4; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 140 + k * 30, mts[k], 8, 4);
            }
            hexnum(bounds[0] + 120, bounds[1] + 140 + 4 * 30, size, 2, 4);
            return;
        }

        /* Stage 25: are the array elements real objects of one type?  The first
         * 8 bytes of any managed object is its MethodTable pointer, and objects
         * of the same class share it.  Prints the low 32 bits of arr[0..3]'s MT
         * plus the element count -- identical values mean genuine NPCs, wildly
         * different values mean we are reading garbage. */
        if (kMapStage == 25) {
            unsigned mt[4] = {0, 0, 0, 0};
            unsigned cnt = 0;
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* list = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(list)) {
                    continue;
                }
                void* arr = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(list) + 0x10);
                if (!Plausible(arr)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(list) + 0x18);
                cnt = static_cast<unsigned>(n);
                for (int k = 0; k < 4 && k < n; k++) {
                    void* c = reinterpret_cast<void**>(arr)[k];
                    if (!Plausible(c)) {
                        mt[k] = 0xDEADBEEFu;
                        continue;
                    }
                    unsigned v = 0;
                    __builtin_memcpy(&v, c, 4);
                    mt[k] = v;
                }
                break;
            }
            for (int k = 0; k < 4; k++) {
                hexnum(bounds[0] + 120, bounds[1] + 140 + k * 30, mt[k], 8, 4);
            }
            hexnum(bounds[0] + 120, bounds[1] + 140 + 4 * 30, cnt, 2, 4);
            return;
        }

        /* Stage 24: map every float-looking field in the first Town NPC.
         * Prints up to 8 rows of "offset rawword"; rawword is the little-endian
         * 32-bit value, filtered to those whose exponent makes it a float in
         * roughly 1..25000 (0x3F800000..0x46C35000) -- i.e. pixel coordinates.
         * Last row prints the NPC count as a sanity check. */
        if (kMapStage == 24) {
            void* npc = nullptr;
            int got = 0;
            for (int i = 0; i < locCount && npc == nullptr; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* list = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(list)) {
                    continue;
                }
                void* arr = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(list) + 0x10);
                if (!Plausible(arr)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(list) + 0x18);
                got = n;
                for (int j = 0; j < n; j++) {
                    void* c = reinterpret_cast<void**>(arr)[j];
                    if (Plausible(c)) {
                        npc = c;
                        break;
                    }
                }
            }
            int row = 0;
            if (npc != nullptr) {
                const unsigned char* b = reinterpret_cast<const unsigned char*>(npc);
                for (int off = 0; off <= 0x300 && row < 8; off += 4) {
                    unsigned w = 0;
                    __builtin_memcpy(&w, b + off, 4);
                    if (w < 0x3F800000u || w > 0x46C35000u) {
                        continue;
                    }
                    hexnum(bounds[0] + 120, bounds[1] + 140 + row * 30,
                           static_cast<unsigned>(off), 2, 4);
                    hexnum(bounds[0] + 190, bounds[1] + 140 + row * 30, w, 8, 4);
                    row++;
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 140 + row * 30,
                   static_cast<unsigned>(got), 2, 4);
            return;
        }

        /* Stage 23: find the pixel-position field by comparing three different
         * NPCs.  Position differs per NPC; speed/facing/etc. usually match, so
         * the first offset where three separate floats are all in tile-pixel
         * range AND mutually different is the X component.
         * Prints: offset, (int)X for npc0/1/2. */
        if (kMapStage == 23) {
            void* npcs[3] = {nullptr, nullptr, nullptr};
            int got = 0;
            for (int i = 0; i < locCount && got < 3; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* list = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(list)) {
                    continue;
                }
                void* arr = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(list) + 0x10);
                if (!Plausible(arr)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(list) + 0x18);
                for (int j = 0; j < n && got < 3; j++) {
                    void* c = reinterpret_cast<void**>(arr)[j];
                    if (Plausible(c)) {
                        npcs[got++] = c;
                    }
                }
            }
            unsigned foundOff = 0xFFFFu;
            unsigned vals[3] = {0, 0, 0};
            if (got == 3) {
                for (int off = 0x10; off <= 0x200; off += 4) {
                    float f[3] = {0, 0, 0};
                    bool ok = true;
                    for (int k = 0; k < 3 && ok; k++) {
                        __builtin_memcpy(&f[k],
                                         reinterpret_cast<const unsigned char*>(npcs[k]) + off, 4);
                        if (!(f[k] > 0.0f && f[k] < 12800.0f)) {
                            ok = false;
                        }
                    }
                    if (!ok) {
                        continue;
                    }
                    const float d01 = f[0] > f[1] ? f[0] - f[1] : f[1] - f[0];
                    const float d02 = f[0] > f[2] ? f[0] - f[2] : f[2] - f[0];
                    const float d12 = f[1] > f[2] ? f[1] - f[2] : f[2] - f[1];
                    if (d01 < 1.0f || d02 < 1.0f || d12 < 1.0f) {
                        continue;
                    }
                    foundOff = static_cast<unsigned>(off);
                    for (int k = 0; k < 3; k++) {
                        vals[k] = static_cast<unsigned>(f[k]);
                    }
                    break;
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 140, foundOff, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 170, vals[0], 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 200, vals[1], 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 230, vals[2], 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 260, static_cast<unsigned>(got), 2, 4);
            return;
        }

        /* Stage 22: dump the first 32 bytes of the characters list object as
         * eight 32-bit words, so its real layout can be read off the screen
         * instead of guessed.  Word at +0x10 tells us whether that slot is the
         * backing array (looks like 0x2xxxxxxxx) or a small int (0x00000010). */
        if (kMapStage == 22) {
            unsigned words[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    break;
                }
                void* list = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(list)) {
                    break;
                }
                const unsigned char* b = reinterpret_cast<const unsigned char*>(list);
                for (int w = 0; w < 8; w++) {
                    unsigned v = 0;
                    __builtin_memcpy(&v, b + w * 4, 4);
                    words[w] = v;
                }
                break;
            }
            for (int w = 0; w < 8; w++) {
                hexnum(bounds[0] + 120, bounds[1] + 140 + w * 30, words[w], 8, 4);
            }
            return;
        }

        /* Stage 21: locate Character.Position without calling any game
         * function.  The tile getters we used are mid-function addresses and
         * fault on retail.  Position is a Vector2 of pixel floats, so scan the
         * first Town NPC for two adjacent floats in a sane pixel range that are
         * multiples of 8.  Prints: offset, (int)X, (int)Y -- X/64 and Y/64
         * should land inside the town tile grid. */
        if (kMapStage == 21) {
            unsigned foundOff = 0xFFFFu;
            unsigned fx = 0;
            unsigned fy = 0;
            for (int i = 0; i < locCount && foundOff == 0xFFFFu; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName) || !NameEquals(locName, "Town")) {
                    continue;
                }
                void* netRef = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x108);
                if (!Plausible(netRef)) {
                    continue;
                }
                void* list = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netRef) + 0x68);
                if (!Plausible(list)) {
                    continue;
                }
                void* arr = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(list) + 0x10);
                if (!Plausible(arr)) {
                    continue;
                }
                void* npc = reinterpret_cast<void**>(arr)[0];
                if (!Plausible(npc)) {
                    continue;
                }
                const unsigned char* base =
                    reinterpret_cast<const unsigned char*>(npc);
                for (int off = 0; off <= 0x180; off += 4) {
                    float f0 = 0;
                    float f1 = 0;
                    __builtin_memcpy(&f0, base + off, 4);
                    __builtin_memcpy(&f1, base + off + 4, 4);
                    if (!(f0 > 0.0f && f0 < 20000.0f)) {
                        continue;
                    }
                    if (!(f1 > 0.0f && f1 < 20000.0f)) {
                        continue;
                    }
                    const int i0 = static_cast<int>(f0);
                    const int i1 = static_cast<int>(f1);
                    if ((i0 & 7) != 0 || (i1 & 7) != 0) {
                        continue;
                    }
                    foundOff = static_cast<unsigned>(off);
                    fx = static_cast<unsigned>(i0);
                    fy = static_cast<unsigned>(i1);
                    break;
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 150, foundOff, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 180, fx, 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 210, fy, 4, 4);
            return;
        }

        /* Stage 20: 0xB8 and 0x108 read as empty lists when dereferenced
         * directly, yet stage 18 saw 103/38 elements through them -- the field
         * is a NetRef and needs the +0x68 unwrap.  Prints six counts:
         *   1 Town 0xB8 direct   2 Town 0xB8 unwrapped
         *   3 Town 0x108 direct  4 Town 0x108 unwrapped
         *   5 Farm 0xB8 unwrapped  6 Farm 0x108 unwrapped */
        if (kMapStage == 20) {
            auto countAt = [&](void* l, int off, bool unwrap) -> unsigned {
                void* p = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + off);
                if (!Plausible(p)) {
                    return 0xFFFFu;
                }
                if (unwrap) {
                    p = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(p) + 0x68);
                    if (!Plausible(p)) {
                        return 0xFFFEu;
                    }
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(p) + 0x18);
                if (n < 0 || n > 4096) {
                    return 0xFFFDu;
                }
                return static_cast<unsigned>(n);
            };
            unsigned v[6] = {0xEEEEu, 0xEEEEu, 0xEEEEu,
                             0xEEEEu, 0xEEEEu, 0xEEEEu};
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName)) {
                    continue;
                }
                if (NameEquals(locName, "Town")) {
                    v[0] = countAt(l, 0xB8, false);
                    v[1] = countAt(l, 0xB8, true);
                    v[2] = countAt(l, 0x108, false);
                    v[3] = countAt(l, 0x108, true);
                }
                else if (NameEquals(locName, "Farm")) {
                    v[4] = countAt(l, 0xB8, true);
                    v[5] = countAt(l, 0x108, true);
                }
            }
            for (int r = 0; r < 6; r++) {
                hexnum(bounds[0] + 120, bounds[1] + 140 + r * 30, v[r], 4, 4);
            }
            return;
        }

        /* Stage 19: decisive comparison.  Town holds a dozen villagers, the
         * Farm holds one cat, so whichever offset reproduces that shape is
         * `characters`.  Prints four counts:
         *   row1 Town@0xB8   row2 Town@0x108
         *   row3 Farm@0xB8   row4 Farm@0x108 */
        if (kMapStage == 19) {
            auto countAt = [&](void* l, int off) -> unsigned {
                void* list = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + off);
                if (!Plausible(list)) {
                    return 0xFFFFu;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(list) + 0x18);
                if (n < 0 || n > 4096) {
                    return 0xFFFEu;
                }
                return static_cast<unsigned>(n);
            };
            unsigned v[4] = {0xEEEEu, 0xEEEEu, 0xEEEEu, 0xEEEEu};
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName)) {
                    continue;
                }
                if (NameEquals(locName, "Town")) {
                    v[0] = countAt(l, 0xB8);
                    v[1] = countAt(l, 0x108);
                }
                else if (NameEquals(locName, "Farm")) {
                    v[2] = countAt(l, 0xB8);
                    v[3] = countAt(l, 0x108);
                }
            }
            for (int r = 0; r < 4; r++) {
                hexnum(bounds[0] + 120, bounds[1] + 150 + r * 30, v[r], 4, 4);
            }
            return;
        }

        /* Stage 18: the eight offsets that survived stage 17, compared by
         * total and peak element count.  characters should total roughly the
         * number of NPCs in the world (tens) with a peak in the low teens;
         * animal/object collections run far higher. */
        if (kMapStage == 18) {
            static const int kCand[8] = {0x58, 0x60, 0x68, 0x70,
                                         0xB8, 0x108, 0x138, 0x148};
            for (int c = 0; c < 8; c++) {
                const int off = kCand[c];
                unsigned sum = 0;
                unsigned mx = 0;
                for (int i = 0; i < locCount; i++) {
                    void* l = locs[i];
                    if (!Plausible(l)) {
                        continue;
                    }
                    void* raw = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(l) + off);
                    if (!Plausible(raw)) {
                        continue;
                    }
                    int best = 0;
                    for (int shape = 0; shape < 2; shape++) {
                        void* list = raw;
                        if (shape == 1) {
                            list = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(raw) + 0x68);
                            if (!Plausible(list)) {
                                continue;
                            }
                        }
                        void* arr = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(list) + 0x10);
                        if (!Plausible(arr)) {
                            continue;
                        }
                        const int n = *reinterpret_cast<int*>(
                            reinterpret_cast<unsigned char*>(list) + 0x18);
                        if (n <= 0 || n > 64) {
                            continue;
                        }
                        if (!Plausible(reinterpret_cast<void**>(arr)[0])) {
                            continue;
                        }
                        if (n > best) {
                            best = n;
                        }
                    }
                    sum += static_cast<unsigned>(best);
                    if (static_cast<unsigned>(best) > mx) {
                        mx = static_cast<unsigned>(best);
                    }
                }
                if (sum > 0xFFFFu) {
                    sum = 0xFFFFu;
                }
                if (mx > 0xFFu) {
                    mx = 0xFFu;
                }
                hexnum(bounds[0] + 120, bounds[1] + 150 + c * 30, sum, 4, 4);
                hexnum(bounds[0] + 240, bounds[1] + 150 + c * 30, mx, 2, 4);
            }
            return;
        }

        /* Stage 17: wider scan, both shapes, and a count window that only
         * characters can satisfy.  A location holds a handful of NPCs, never
         * hundreds, so requiring 1..64 per location rejects objects/animals
         * without needing to name them.  Shape A: [loc+off] is the list.
         * Shape B: [loc+off] is a NetRef and [that+0x68] is the list.
         * Line 1 = offsets 0x40+k*8 for k=0..31, line 2 = k=32..47. */
        if (kMapStage == 17) {
            unsigned lo = 0;
            unsigned hi = 0;
            for (int k = 0; k < 48; k++) {
                const int off = 0x40 + k * 8;
                int hits = 0;
                for (int i = 0; i < locCount; i++) {
                    void* l = locs[i];
                    if (!Plausible(l)) {
                        continue;
                    }
                    void* raw = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(l) + off);
                    if (!Plausible(raw)) {
                        continue;
                    }
                    for (int shape = 0; shape < 2 && hits < 10; shape++) {
                        void* list = raw;
                        if (shape == 1) {
                            list = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(raw) + 0x68);
                            if (!Plausible(list)) {
                                continue;
                            }
                        }
                        void* arr = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(list) + 0x10);
                        if (!Plausible(arr)) {
                            continue;
                        }
                        const int n = *reinterpret_cast<int*>(
                            reinterpret_cast<unsigned char*>(list) + 0x18);
                        if (n <= 0 || n > 64) {
                            continue;
                        }
                        if (!Plausible(reinterpret_cast<void**>(arr)[0])) {
                            continue;
                        }
                        hits++;
                    }
                }
                if (hits >= 10) {
                    if (k < 32) {
                        lo |= (1u << k);
                    }
                    else {
                        hi |= (1u << (k - 32));
                    }
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 150, lo, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 180, hi, 8, 4);
            return;
        }

        /* Stage 16: disambiguate the candidate offsets found by stage 15.
         * For each one print "sum of counts" then "max count in one location".
         * characters is a handful per location (max ~10, total ~50-150);
         * objects/animals run into the hundreds and hit the cap. */
        if (kMapStage == 16) {
            static const int kCand[7] = {0xD0, 0xD8, 0xF0, 0xF8,
                                         0x118, 0x138, 0x140};
            for (int c = 0; c < 7; c++) {
                const int off = kCand[c];
                unsigned sum = 0;
                unsigned mx = 0;
                for (int i = 0; i < locCount; i++) {
                    void* l = locs[i];
                    if (!Plausible(l)) {
                        continue;
                    }
                    void* list = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(l) + off);
                    if (!Plausible(list)) {
                        continue;
                    }
                    const int n = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(list) + 0x18);
                    if (n < 0 || n > 4096) {
                        continue;
                    }
                    sum += static_cast<unsigned>(n);
                    if (static_cast<unsigned>(n) > mx) {
                        mx = static_cast<unsigned>(n);
                    }
                }
                if (sum > 0xFFFFu) {
                    sum = 0xFFFFu;
                }
                if (mx > 0xFFu) {
                    mx = 0xFFu;
                }
                hexnum(bounds[0] + 120, bounds[1] + 150 + c * 30, sum, 4, 4);
                hexnum(bounds[0] + 240, bounds[1] + 150 + c * 30, mx, 2, 4);
            }
            return;
        }

        /* Stage 15: scan GameLocation field offsets for one that really holds a
         * List<NPC>.  The old 0xB0 -> 0x58 chain produced "plausible" garbage,
         * so test the direct shape instead: [loc+off] = list, list+0x10 =
         * backing array, list+0x18 = count, and (if count>0) array[0] mapped.
         * Prints a 32-bit mask: bit k set = offset (0x80 + k*8) qualified for
         * at least 10 loaded locations. */
        if (kMapStage == 15) {
            unsigned mask = 0;
            for (int k = 0; k < 32; k++) {
                const int off = 0x80 + k * 8;
                int hits = 0;
                for (int i = 0; i < locCount; i++) {
                    void* l = locs[i];
                    if (!Plausible(l)) {
                        continue;
                    }
                    void* list = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(l) + off);
                    if (!Plausible(list)) {
                        continue;
                    }
                    void* arr = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(list) + 0x10);
                    if (!Plausible(arr)) {
                        continue;
                    }
                    const int n = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(list) + 0x18);
                    if (n < 0 || n > 512) {
                        continue;
                    }
                    if (n > 0 &&
                        !Plausible(reinterpret_cast<void**>(arr)[0])) {
                        continue;
                    }
                    hits++;
                }
                if (hits >= 10) {
                    mask |= (1u << k);
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 150, mask, 8, 4);
            return;
        }

        /* Stage 14: print the three mount Result codes in hex, top to bottom:
         *   fsInitialize / fsOpenSdCardFileSystem / fsFsCreateFile
         * 00000000 means that step succeeded. */
        if (kMapStage == 14) {
            const int cell = 4;
            hexnum(bounds[0] + 120, bounds[1] + 150, sdlog::DiagSm(), 8, cell);
            hexnum(bounds[0] + 120, bounds[1] + 180, sdlog::DiagInit(), 8, cell);
            hexnum(bounds[0] + 120, bounds[1] + 210, sdlog::DiagOpenFs(), 8, cell);
            hexnum(bounds[0] + 120, bounds[1] + 240, sdlog::DiagCreate(), 8, cell);
            return;
        }

        /* Stage 13: the character stage, same bit-grid readout.
         *   row 0 (y+150) = characters returned by List.get_Item
         *   row 1 (y+200) = ... that pass the mapping guard
         *   row 2 (y+250) = ... with tileX>=0 and tileY>=0
         *   row 3 (y+300) = markers actually drawn
         * c1/c2/c3 from stage 12 were 80/29/24, so everything up to the
         * world-map lookup already works on retail. */
        if (kMapStage == 13) {
            auto getTileXFn = reinterpret_cast<GetTileFn>(g_base + kGetTileXOff);
            auto getTileYFn = reinterpret_cast<GetTileFn>(g_base + kGetTileYOff);
            auto getPlayerFn = reinterpret_cast<GetPlayerFn>(g_base + kGetPlayerOff);
            void* player = getPlayerFn(nullptr);
            int d[4] = {0, 0, 0, 0};
            int budget = 64; /* keep the frame time bounded even if a
                                * bogus location reports a huge count */
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* coll = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0xB0);
                if (!Plausible(coll)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(coll) + 0x58);
                if (!Plausible(inner)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                if (n <= 0 || n > 512) {
                    continue;
                }
                /* Same List<T> layout as Game1.locations: backing array at
                 * +0x10, size at +0x18. */
                void* items = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                if (!Plausible(items)) {
                    continue;
                }
                const WorldMapEntry* entry = nullptr;
                {
                    void* nameNet = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(l) + 0x1A8);
                    if (Plausible(nameNet)) {
                        void* locName = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                        if (Plausible(locName)) {
                            for (int k = 0; k < kWorldMapCount; k++) {
                                if (NameEquals(locName, kWorldMap[k].name)) {
                                    entry = &kWorldMap[k];
                                    break;
                                }
                            }
                        }
                    }
                }
                for (int j = 0; j < n; j++) {
                    if (--budget <= 0) {
                        break;
                    }
                    /* Read the backing array instead of calling
                     * List.get_Item.  get_Item bounds-checks and throws a
                     * managed exception when the index is past the real size,
                     * and an unhandled exception inside a native render hook
                     * hangs the console.  A raw array read cannot throw. */
                    void* c = reinterpret_cast<void**>(items)[j];
                    if (c == nullptr || c == player) {
                        continue;
                    }
                    d[0]++;
                    if (!Plausible(c)) {
                        continue;
                    }
                    d[1]++;
                    const int tx = getTileXFn(c);
                    const int ty = getTileYFn(c);
                    if (tx < 0 || ty < 0) {
                        continue;
                    }
                    d[2]++;
                    if (entry == nullptr) {
                        continue;
                    }
                    if (entry->rectW < 1 || entry->rectH < 1 ||
                        entry->mapW < 1 || entry->mapH < 1) {
                        continue;
                    }
                    d[3]++;
                }
            }
            hexnum(bounds[0] + 120, bounds[1] + 150, static_cast<unsigned>(d[0]), 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 180, static_cast<unsigned>(d[1]), 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 210, static_cast<unsigned>(d[2]), 4, 4);
            hexnum(bounds[0] + 120, bounds[1] + 240, static_cast<unsigned>(d[3]), 4, 4);
            return;
        }

        /* Stage 12: three counters encoded as bit grids so the readout can be
         * decoded from a photo instead of counted by eye.
         *   grid row 0 (y+150) = locations with a mapped GameLocation
         *   grid row 1 (y+200) = ... whose character list is mapped, n>0
         *   grid row 2 (y+250) = ... whose name matched the world-map table
         * Column i (x+120+ i*44) is bit i; 30px cells, so nothing overlaps. */
        if (kMapStage == 12) {
            int c[3] = {0, 0, 0};
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                c[0]++;
                void* coll = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0xB0);
                if (!Plausible(coll)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(coll) + 0x58);
                if (!Plausible(inner)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                if (n <= 0 || n > 512) {
                    continue;
                }
                c[1]++;
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName)) {
                    continue;
                }
                for (int k = 0; k < kWorldMapCount; k++) {
                    if (NameEquals(locName, kWorldMap[k].name)) {
                        c[2]++;
                        break;
                    }
                }
            }
            for (int r = 0; r < 3; r++) {
                for (int b = 0; b < 8; b++) {
                    if (((c[r] >> b) & 1) == 0) {
                        continue;
                    }
                    const u64 wxy =
                        (static_cast<u64>(bounds[1] + 150 + r * 50) << 32) |
                        static_cast<u32>(bounds[0] + 120 + b * 44);
                    draw(batch, texture, wxy, (30ull << 32) | 30u, 0xFFFF00FFu);
                }
            }
            return;
        }

        /* Stage 11: how far does the per-location chain actually get?
         *   row 1 (y+150) magenta = locations with a mapped GameLocation
         *   row 2 (y+190) yellow  = ... whose character list is mapped and non-empty
         *   row 3 (y+230) blue    = ... whose name matched the world-map table
         * 10 squares per row, capped at 30. */
        if (kMapStage == 11) {
            int c1 = 0, c2 = 0, c3 = 0;
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                c1++;
                void* coll = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0xB0);
                if (!Plausible(coll)) {
                    continue;
                }
                void* inner = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(coll) + 0x58);
                if (!Plausible(inner)) {
                    continue;
                }
                const int n = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                if (n <= 0 || n > 512) {
                    continue;
                }
                c2++;
                void* nameNet = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0x1A8);
                if (!Plausible(nameNet)) {
                    continue;
                }
                void* locName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                if (!Plausible(locName)) {
                    continue;
                }
                for (int k = 0; k < kWorldMapCount; k++) {
                    if (NameEquals(locName, kWorldMap[k].name)) {
                        c3++;
                        break;
                    }
                }
            }
            const int counts[3] = {c1, c2, c3};
            const u32 colors[3] = {0xFFFF00FFu, 0xFF00FFFFu, 0xFFFF0000u};
            const int ys[3] = {150, 190, 230};
            for (int r = 0; r < 3; r++) {
                int shown = counts[r] > 30 ? 30 : counts[r];
                for (int k = 0; k < shown; k++) {
                    const u64 wxy = (static_cast<u64>(bounds[1] + ys[r] + (k / 10) * 26) << 32) |
                                    static_cast<u32>(bounds[0] + 120 + (k % 10) * 24);
                    draw(batch, texture, wxy, (20ull << 32) | 20u, colors[r]);
                }
            }
            return;
        }

        /* Stage 9: read the array entries only -- no dereference of anything
         * they point at.  If this still faults, `locs`/`locCount` do not
         * describe a real array and the whole enumeration has to change.
         *   row A (y+150) = how many entries look like real pointers
         *   row B (y+250) = locCount in binary, bit 7..0 left to right */
        if (kMapStage == 9) {
            int good = 0;
            for (int i = 0; i < locCount; i++) {
                if (Plausible(locs[i])) {
                    good++;
                }
            }
            int shown = good > 30 ? 30 : good;
            for (int k = 0; k < shown; k++) {
                const u64 wxy = (static_cast<u64>(bounds[1] + 150 + (k / 10) * 30) << 32) |
                                static_cast<u32>(bounds[0] + 120 + (k % 10) * 26);
                draw(batch, texture, wxy, (22ull << 32) | 22u, 0xFFFF00FFu);
            }
            for (int b = 0; b < 8; b++) {
                if (((locCount >> b) & 1) == 0) {
                    continue;
                }
                const u64 wxy = (static_cast<u64>(bounds[1] + 250) << 32) |
                                static_cast<u32>(bounds[0] + 120 + (7 - b) * 26);
                draw(batch, texture, wxy, (22ull << 32) | 22u, 0xFF00FFFFu);
            }
            return;
        }

        /* Stage 8: dereference `location + 0xB0` for EVERY location, nothing
         * deeper.  Stage 5 only tried location 0 before bailing out, so it
         * never proved the rest of the array was safe.
         *   row A (y+150) = how many locations yielded a plausible collection
         *   row B (y+250) = locCount in binary, bit 7..0 left to right */
        if (kMapStage == 8) {
            int good = 0;
            for (int i = 0; i < locCount; i++) {
                void* l = locs[i];
                if (!Plausible(l)) {
                    continue;
                }
                void* coll = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(l) + 0xB0);
                if (Plausible(coll)) {
                    good++;
                }
            }
            int shown = good > 30 ? 30 : good;
            for (int k = 0; k < shown; k++) {
                const u64 wxy = (static_cast<u64>(bounds[1] + 150 + (k / 10) * 30) << 32) |
                                static_cast<u32>(bounds[0] + 120 + (k % 10) * 26);
                draw(batch, texture, wxy, (22ull << 32) | 22u, 0xFFFF00FFu);
            }
            for (int b = 0; b < 8; b++) {
                if (((locCount >> b) & 1) == 0) {
                    continue;
                }
                const u64 wxy = (static_cast<u64>(bounds[1] + 250) << 32) |
                                static_cast<u32>(bounds[0] + 120 + (7 - b) * 26);
                draw(batch, texture, wxy, (22ull << 32) | 22u, 0xFF00FFFFu);
            }
            return;
        }

        /* Stage 7: report `locCount` itself.  One magenta square per entry,
         * capped at 20 (all 20 lit means "20 or more").  A sane save has
         * roughly 12-40 loaded locations; if this lights the whole row the
         * count field is not what we think it is, and the loop below is
         * walking off the end of the array. */
        if (kMapStage == 7) {
            int cnt = locCount;
            if (cnt < 0) {
                cnt = 0;
            }
            if (cnt > 20) {
                cnt = 20;
            }
            for (int k = 0; k < cnt; k++) {
                const u64 wxy = (static_cast<u64>(bounds[1] + 150) << 32) |
                                static_cast<u32>(bounds[0] + 120 + k * 26);
                draw(batch, texture, wxy, (24ull << 32) | 24u, 0xFFFF00FFu);
            }
            return;
        }

        /* Stage 4: only the location list itself.  `locCount` is read straight
         * out of the managed List header, so a bad header would make the loop
         * below walk off the end of the array -- clamp it and stop here. */
        if (kMapStage == 4) {
            if (locCount > 0 && locCount <= 200) {
                const u64 wxy = (static_cast<u64>(bounds[1] + 120) << 32) |
                                static_cast<u32>(bounds[0] + 120);
                draw(batch, texture, wxy, (24ull << 32) | 24u, 0xFFFF00FFu);
            }
            return;
        }

        /* Stage 5: walk the location list with a plausibility check on every
         * dereference and paint one magenta square per check that passed.
         * The square count names the first check that failed:
         *   1 locs array   2 locs[i]      3 +0xB0 characters
         *   4 +0x58 inner  5 char count   6 +0x1A8 name
         *   7 name+0x68    8 every location passed
         */
        if (kMapStage == 5) {
            auto plausible = [](const void* p) -> bool {
                const uintptr_t v = reinterpret_cast<uintptr_t>(p);
                return v >= 0x10000ull && v < 0x8000000000ull && (v & 7u) == 0;
            };
            int code = 0;
            if (!plausible(locs)) {
                code = 1;
            }
            else {
                for (int i = 0; i < locCount; i++) {
                    void* l = locs[i];
                    if (!plausible(l)) { code = 2; break; }
                    void* coll = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(l) + 0xB0);
                    if (!plausible(coll)) { code = 3; break; }
                    void* inner = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(coll) + 0x58);
                    if (!plausible(inner)) { code = 4; break; }
                    const int n = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(inner) + 0x18);
                    if (n < 0 || n > 512) { code = 5; break; }
                    void* nameNet = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(l) + 0x1A8);
                    if (!plausible(nameNet)) { code = 6; break; }
                    void* locName = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(nameNet) + 0x68);
                    if (!plausible(locName)) { code = 7; break; }
                }
                if (code == 0) {
                    code = 8;
                }
            }
            for (int k = 0; k < code; k++) {
                const u64 wxy = (static_cast<u64>(bounds[1] + 120) << 32) |
                                static_cast<u32>(bounds[0] + 120 + k * 30);
                draw(batch, texture, wxy, (24ull << 32) | 24u, 0xFFFF00FFu);
            }
            return;
        }

        int placed = 0;
        /* Stage 91 diagnostics: enough to tell "sheet accepted", "path cached",
         * "heads drawn" apart in one hardware round. */
        int headsDrawn = 0;
        int texOk = 0;  /* characters whose cached path reached a texture */
        /* Sheet paths found by FindSheet, cached for the session: layout is
         * per class, so the probes only need to run on the first few
         * characters (different subclasses -- NPC, FarmAnimal, Monster --
         * have different layouts, hence up to three paths). */
        static unsigned s_path[3][3];
        static int s_pathD[3];
        static int s_pathN = 0;
        /* Texture2Ds reached during probing that failed the 16-wide test. */
        static int s_loose = 0;
    /* Probing starts from the character itself, not from the player.
     * The player object is pointer-dense (157 fields were counted) yet three
     * hops out of it never reached a single Texture2D, while the same walk
     * from a list character did -- so Game1.player is not the object whose
     * neighbourhood holds the sheets.  There is also no need to probe it:
     * field layout is per class, and the characters in the list are exactly
     * the instances that get drawn.
     *
     * The probe waits for the Texture2D MethodTable: FindSheet returns 0
     * immediately on a null mtTex, and the old one-shot latch then froze that
     * empty result for the rest of the session.  One character is probed per
     * frame -- the walk costs about 100k reads and three in a row is a
     * visible hitch -- and roots are de-duplicated by MethodTable so the
     * three probes cover three different classes (NPC, FarmAnimal, Monster)
     * instead of the same class three times. */
    static int s_probeTries = 0;
    static void* s_probeMT[3] = {nullptr, nullptr, nullptr};
    static unsigned s_probeFrame = 0xFFFFFFFFu;

        for (int i = 0; i < locCount && placed < 256; i++) {
            void* l = locs[i];
            if (!Plausible(l)) {
                continue;
            }
            /* characters, confirmed on retail 1.6.15.13:
             *   loc+0x108 -> NetRef, +0x68 -> wrapper, +0x10 -> inner List,
             *   inner+0x10 -> backing array, inner+0x18 -> size.
             * The wrapper's own +0x18 is the *capacity* (16), which is why the
             * count looked right while the array did not. */
            void* netRef = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(l) + 0x108);
            if (!Plausible(netRef)) {
                continue;
            }
            void* wrapper = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(netRef) + 0x68);
            if (!Plausible(wrapper)) {
                continue;
            }
            void* inner = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(wrapper) + 0x10);
            if (!Plausible(inner)) {
                continue;
            }
            void* items = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(inner) + 0x10);
            if (!Plausible(items)) {
                continue;
            }
            const int n = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(inner) + 0x18);
            if (n <= 0 || n > 512) {
                continue;
            }
            /* Location name is a NetString: [loc+0x1A8] -> [+0x68]. */
            void* nameNet = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(l) + 0x1A8);
            void* locName = Plausible(nameNet)
                ? *reinterpret_cast<void**>(
                      reinterpret_cast<unsigned char*>(nameNet) + 0x68)
                : nullptr;
            if (!Plausible(locName)) {
                continue;
            }

            const WorldMapEntry* entry = nullptr;
            for (int k = 0; k < kWorldMapCount; k++) {
                if (NameEquals(locName, kWorldMap[k].name)) {
                    entry = &kWorldMap[k];
                    break;
                }
            }
            /* Unknown location: mirror the game, which treats anything without
             * a world-map entry as part of the Farm.  Without this, villagers
             * in barns/coops/sheds simply vanish from the map. */
            bool useCenter = false;
            if (entry == nullptr) {
                static const WorldMapEntry* farmEntry = nullptr;
                if (farmEntry == nullptr) {
                    for (int k = 0; k < kWorldMapCount; k++) {
                        const char* a = kWorldMap[k].name;
                        const char* b = "Farm";
                        while (*a != '\0' && *a == *b) {
                            ++a;
                            ++b;
                        }
                        if (*a == '\0' && *b == '\0') {
                            farmEntry = &kWorldMap[k];
                            break;
                        }
                    }
                }
                if (farmEntry == nullptr) {
                    continue;
                }
                entry = farmEntry;
                useCenter = true;
            }

            const float rx = static_cast<float>(entry->rectX) * 4.0f;
            const float ry = static_cast<float>(entry->rectY) * 4.0f;
            const float rw = static_cast<float>(entry->rectW) * 4.0f;
            const float rh = static_cast<float>(entry->rectH) * 4.0f;
            const float mw = static_cast<float>(entry->mapW);
            const float mh = static_cast<float>(entry->mapH);
            if (rw < 1.0f || rh < 1.0f || mw < 1.0f || mh < 1.0f) {
                continue;
            }

            for (int j = 0; j < n && placed < 256; j++) {
                /* Raw array read: List.get_Item throws on an out-of-range
                 * index, and an unhandled managed exception inside a native
                 * render hook hangs the console. */
                void* c = reinterpret_cast<void**>(items)[j];
                if (!Plausible(c) || c == player) {
                    continue;
                }
                /* Probe one character per frame, only for a class that has not
                 * been probed yet.  Layout is per class, so three probes cover
                 * NPC, FarmAnimal and Monster; probing three villagers would
                 * only repeat the same answer. */
                if (s_pathN == 0 && s_probeTries < 3 && mtTex != nullptr &&
                    s_probeFrame != s_frame) {
                    void* cmt = *reinterpret_cast<void**>(c);
                    bool seen = false;
                    for (int q = 0; q < s_probeTries; q++) {
                        if (s_probeMT[q] == cmt) {
                            seen = true;
                        }
                    }
                    if (!seen) {
                        s_probeFrame = s_frame;
                        s_probeMT[s_probeTries] = cmt;
                        s_probeTries++;
                        unsigned cand[3] = {0, 0, 0};
                        const int cd = FindSheet(c, mtTex, cand, &s_loose);
                        if (cd > 0 && s_pathN < 3) {
                            s_pathD[s_pathN] = cd;
                            for (int k = 0; k < 3; k++) {
                                s_path[s_pathN][k] = cand[k];
                            }
                            s_pathN++;
                        }
                    }
                }
                /* Tile is stored as two ints, not floats: retail reads back
                 * (52,19) (53,19) (95,50) for three Town villagers. */
                const int tx = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(c) + 0x14);
                const int ty = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(c) + 0x18);
                if (tx <= 0 || ty <= 0) {
                    continue;
                }
                const float px = useCenter
                    ? rx + rw * 0.5f
                    : rx + rw * (static_cast<float>(tx) / mw);
                const float py = useCenter
                    ? ry + rh * 0.5f
                    : ry + rh * (static_cast<float>(ty) / mh);
                const u32 mx = static_cast<u32>(static_cast<float>(bounds[0]) + px + 32.0f);
                const u32 my = static_cast<u32>(static_cast<float>(bounds[1]) + py + 32.0f);

                /* Stage 3 stops before any per-NPC drawing: everything above
                 * (locations, characters, tiles, table lookup, coordinate
                 * maths) runs, but nothing is drawn from the computed values. */
                if (kMapStage == 3) {
                    placed++;
                    continue;
                }

                /* Draw the head from the character's own walk sheet.  When no
                 * sheet can be reached a small square is drawn instead, so
                 * "texture found" and "texture missing" stay distinguishable
                 * at a glance instead of one hiding the other.
                 *
                 * Character -> sprite: +0x50 holds a NetRef whose +0x68
                 * payload is the AnimatedSprite, and the sheet is somewhere on
                 * that object.  Both hops are scanned rather than assumed; see
                 * the MethodTable test below. */
                {
                    /* c+0x50 is documented as a NetRef whose +0x68 payload is
                     * the AnimatedSprite, but on retail 1.6.15.13 it sometimes
                     * points at the sprite directly.  Resolve both ways and keep
                     * the candidate that actually carries a name and/or a
                     * Texture2D, so the right one wins in a single hardware
                     * round regardless of which layout this build uses. */
                    void* raw50 = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(c) + 0x50);
                    void* sp = nullptr;
                    auto spriteScore = [&](void* s) -> int {
                        if (!Plausible(s)) {
                            return -1;
                        }
                        int score = 0;
                        void* nn = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(s) + 0x28);
                        if (Plausible(nn)) {
                            void* nmc = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(nn) + 0x68);
                            if (Plausible(nmc)) {
                                score += 1; /* has a readable name */
                            }
                        }
                        const auto* sb =
                            reinterpret_cast<const unsigned char*>(s);
                        for (int f = 0x08; f <= 0x100; f += 8) {
                            if (!Plausible(sb + f)) {
                                break;
                            }
                            void* v = *reinterpret_cast<void* const*>(sb + f);
                            if (!Plausible(v)) {
                                continue;
                            }
                            if (*reinterpret_cast<void**>(v) == mtTex) {
                                score += 10; /* direct Texture2D */
                                break;
                            }
                            if (Plausible(
                                    reinterpret_cast<unsigned char*>(v) + 0x68) &&
                                *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(v) + 0x68) ==
                                    mtTex) {
                                score += 10; /* Texture2D behind a NetRef */
                                break;
                            }
                        }
                        return score;
                    };
                    if (Plausible(raw50)) {
                        const int sa = spriteScore(raw50);             /* direct */
                        void* refPayload = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(raw50) + 0x68);
                        const int sb = spriteScore(refPayload);        /* netref */
                        sp = (sb > sa) ? refPayload : raw50;
                    }
                    void* nm = nullptr;
                    if (Plausible(sp)) {
                        void* nn = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(sp) + 0x28);
                        if (Plausible(nn)) {
                            nm = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(nn) + 0x68);
                        }
                    }
                    /* Diagnostics: snapshot every 120 frames, 5 snapshots.
                     * If the world keeps running while the map is open the
                     * villager coordinates must differ between snapshots. */
                    const bool doLog = (kMapStage == 0) &&
                        ((s_frame % 120u) == 0u) && (s_round < 30u);
                    if (doLog && nm != nullptr) {
                        char sn[48];
                        char ln[48];
                        if (NameToAscii(nm, sn, static_cast<int>(sizeof(sn))) &&
                            NameToAscii(locName, ln, static_cast<int>(sizeof(ln)))) {
                            const char* snb = sn;
                            for (const char* p = sn; *p != '\0'; p++) {
                                if (*p == '/') {
                                    snb = p + 1;
                                }
                            }
                            Logging.Log(EXL_LOG_PREFIX
                                        "r%d %s @ %s tile %d,%d -> %d,%d",
                                        static_cast<int>(s_round + 1u), snb, ln,
                                        tx, ty,
                                        static_cast<int>(mx), static_cast<int>(my));
                        }
                    }

                    /* Find the sheet without trusting a single offset: the
                     * character points at its sprite (directly or through a
                     * +0x68 NetRef) and the sprite points at a Texture2D the
                     * same two ways.  MethodTable equality with staminaRect is
                     * the only test, so a hit really is a Texture2D and the
                     * layout details stop mattering.
                     *
                     * sheetOff code: 0x0xx = field of the sprite,
                     * 0x1xx = behind a NetRef on the sprite,
                     * 0x2xx/0x3xx = the same two, one hop out from the
                     * character (only used if the sprite guess was wrong). */
                    void* sheet = nullptr;
                    unsigned sheetOff = 0;
                    /* Walk the cached paths from THIS character.  Every
                     * instance of a class shares its field layout, so the
                     * offsets found on the probe character apply here. */
                    void* looseSheet = nullptr;
                    unsigned looseOff = 0;
                    for (int q = 0; q < s_pathN; q++) {
                        void* o = c;
                        bool ok = true;
                        for (int k = 0; k < s_pathD[q]; k++) {
                            if (!Plausible(o)) {
                                ok = false;
                                break;
                            }
                            o = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(o) +
                                s_path[q][k]);
                            if (!Plausible(o)) {
                                ok = false;
                                break;
                            }
                        }
                        if (!ok || !IsTexture2D(o, mtTex)) {
                            continue;
                        }
                        if (LooksLikeWalkSheet(o)) {
                            if (sheet == nullptr) {
                                sheet = o;
                                sheetOff = 0x1000u + s_path[q][0];
                            }
                        }
                        else {
                            if (looseSheet == nullptr) {
                                looseSheet = o;
                                looseOff = 0x2000u + s_path[q][0];
                            }
                        }
                    }
                    /* A non-16-wide texture is still drawn when nothing better
                     * exists: seeing garbage heads tells us the path is right
                     * and the texture is wrong, which a yellow square cannot. */
                    if (sheet == nullptr && looseSheet != nullptr) {
                        sheet = looseSheet;
                        sheetOff = looseOff;
                    }
                    auto scanSprite = [&](void* obj) -> void* {
                        if (!Plausible(obj)) {
                            return nullptr;
                        }
                        const auto* ob =
                            reinterpret_cast<const unsigned char*>(obj);
                        for (int f = 0x08; f <= 0x100; f += 8) {
                            /* Stop before leaving the mapped page. */
                            if (!Plausible(ob + f)) {
                                break;
                            }
                            void* v = *reinterpret_cast<void* const*>(ob + f);
                            if (!Plausible(v)) {
                                continue;
                            }
                            if (*reinterpret_cast<void**>(v) == mtTex) {
                                sheetOff = static_cast<unsigned>(f);
                                return v;
                            }
                            if (!Plausible(
                                    reinterpret_cast<unsigned char*>(v) + 0x68)) {
                                continue;
                            }
                            void* u = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(v) + 0x68);
                            if (Plausible(u) &&
                                *reinterpret_cast<void**>(u) == mtTex) {
                                sheetOff = static_cast<unsigned>(f) | 0x100u;
                                return u;
                            }
                        }
                        return nullptr;
                    };
                    /* Blind offset guessing as a fallback: it does hit real
                     * textures for a handful of characters, so it stays. */
                    if (sheet == nullptr) {
                        sheet = scanSprite(sp);
                    }
                    if (sheet == nullptr) {
                        const auto* cb =
                            reinterpret_cast<const unsigned char*>(c);
                        for (int f = 0x08; f <= 0x100 && sheet == nullptr;
                             f += 8) {
                            if (!Plausible(cb + f)) {
                                break;
                            }
                            void* v = *reinterpret_cast<void* const*>(cb + f);
                            if (!Plausible(v)) {
                                continue;
                            }
                            sheet = scanSprite(v);
                            if (sheet != nullptr) {
                                sheetOff |= 0x200u;
                            }
                        }
                    }
                    /* Last resort: a Texture2D was reached during probing but
                     * none of them exposed a measurable size, so the size test
                     * can never pass.  Draw that texture anyway -- a 16x15
                     * crop either shows a head, which proves it is the villager
                     * sheet and that only the identification was wrong, or it
                     * shows something obviously not a head, which no readout
                     * could have told us. */
                    if (sheet == nullptr && s_dTex != nullptr) {
                        sheet = s_dTex;
                    }

                    if (sheet != nullptr) {
                        texOk++;
                        /* Crop offset is per-NPC, straight out of
                         * NPCMapLocations' NpcMarkerOffsets table.  A fixed
                         * offset is what made some markers clip the face while
                         * others dragged in the shoulders -- every villager's
                         * head sits at a different height in the 16x32 frame
                         * (Vincent +8, Demetrius -2, Abigail +3, ...). */
                        int off = 0;
                        char nmbuf[64];
                        if (NameToAscii(nm, nmbuf, static_cast<int>(sizeof(nmbuf)))) {
                            const char* base = nmbuf;
                            for (const char* p = nmbuf; *p != '\0'; p++) {
                                if (*p == '/' || *p == '\\') {
                                    base = p + 1;
                                }
                            }
                            off = npccrop::Lookup(base);
                        }
                        /* A negative offset starts above the sheet's top edge;
                         * clamp it and shrink the height so the sample never
                         * leaves the texture. */
                        int sy = off;
                        int sh = 15;
                        if (sy < 0) {
                            sh += sy;
                            sy = 0;
                        }
                        if (sh < 6) {
                            sh = 6;
                        }
                        NullableRect src;
                        src.x = 0; src.y = sy; src.w = 16; src.h = sh;
                        src.present = 1;
                        for (int q = 0; q < 7; q++) src.pad[q] = 0;
                        /* Keep the 16:15 aspect of the crop (44 -> 41) and
                         * centre it on the tile point, like the PC mod. */
                        const int dw = 44;
                        const int dh = (dw * sh) / 16;
                        const u64 hxy =
                            (static_cast<u64>(static_cast<u32>(
                                 static_cast<int>(my) - dh / 2)) << 32) |
                            (static_cast<u32>(static_cast<int>(mx) - dw / 2));
                        drawSprite(batch, sheet, hxy,
                                   (static_cast<u64>(static_cast<u32>(dh)) << 32) |
                                        static_cast<u32>(dw),
                                   &src, 0xFFFFFFFFu, 0.0f, 0.0f, 0.0f, 1.0f, 0);
                        headsDrawn++;
                    }
                    else {
                        /* No sheet reached: a small square still marks the
                         * villager, so a partial failure stays readable. */
                        const u64 mxy = (static_cast<u64>(my - 5u) << 32) |
                                        (mx - 5u);
                        draw(batch, texture, mxy, (10ull << 32) | 10u, 0xFF00FFFFu);
                    }
                }
                placed++;
            }
        }

        /* Stage 92 readout: is the 648-element chain really a list of NPCs?
         *
         * 648 characters across 79 locations is far too many.  The game has
         * roughly 45 villagers in total; even counting pets, farm animals and
         * monsters the world does not hold 648 characters at once, and 8.2 per
         * location is a suspiciously uniform average.  Every texture search so
         * far starts from those elements, so the count has to be settled before
         * another round goes into depth or crop rectangles.
         *
         * The same field is therefore read two ways and both are reported:
         *
         *   four-hop   loc+0x108 -> +0x68 -> +0x10 -> array   (what v84 walks)
         *   one-hop    loc+0x108 -> array                     (how `locs` reads)
         *
         * `locs` is obtained one hop (*(list+0x10) is the array, +0x18 is the
         * size) and returns all 83 real locations with names matching
         * Data/WorldMap, so the one-hop form is the proven one.  If the four-hop
         * form reports 648 while the one-hop form reports a sane number, the
         * extra two hops were the bug all along.
         *
         * A villager list must answer with letter-only names (3..20 chars,
         * no digits or brackets, which rules out qualified item ids like
         * "(O)388").  An objects / debris / temporary-sprite list cannot.
         *
         * Rows (8 hex digits each):
         *   0: four-hop total elements / locations / locations with a name
         *   1: four-hop name offset / first two chars of that name
         *   2: method tables seen / one-hop total elements / one-hop locations
         *   3: one-hop name offset / first two chars of that name
         *   4: one-hop locations with a name / method tables seen
         *   5: build tag, 92
         */
        if (kMapStage == 92) {
            static int s_done92 = 0;
            static unsigned s_o0 = 0;
            static unsigned s_o1 = 0;
            static unsigned s_o2 = 0;
            static unsigned s_o3 = 0;
            static unsigned s_o4 = 0;

            if (s_done92 == 0 && s_frame >= 2u) {
                s_done92 = 1;

                /* Letter-only managed string at some field of `e`. */
                auto nameOf = [](void* e, int* outOff,
                                 unsigned char* out2) -> bool {
                    for (int f = 0x08; f <= 0x400; f += 8) {
                        void* s = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(e) + f);
                        if (!Plausible(s)) {
                            continue;
                        }
                        const int len = *reinterpret_cast<int*>(
                            reinterpret_cast<unsigned char*>(s) + 0x10);
                        if (len < 3 || len > 20) {
                            continue;
                        }
                        const unsigned char* ch =
                            reinterpret_cast<const unsigned char*>(s) + 0x14;
                        bool ok = true;
                        for (int k = 0; k < len && ok; k++) {
                            const unsigned u = ch[k * 2] |
                                (static_cast<unsigned>(ch[k * 2 + 1]) << 8);
                            const bool letter = (u >= 'A' && u <= 'Z') ||
                                (u >= 'a' && u <= 'z');
                            if (!letter && u != ' ') {
                                ok = false;
                            }
                        }
                        if (!ok) {
                            continue;
                        }
                        *outOff = f;
                        out2[0] = ch[0];
                        out2[1] = ch[2];
                        return true;
                    }
                    return false;
                };

                constexpr int kSlots = 128;
                unsigned h4[kSlots] = {};
                unsigned h1[kSlots] = {};
                unsigned char ch4[2] = {0, 0};
                unsigned char ch1[2] = {0, 0};
                void* mtSeen[8] = {};
                int mtCount = 0;
                int locs4 = 0;
                int total4 = 0;
                int locs1 = 0;
                int total1 = 0;

                for (int i = 0; i < locCount; i++) {
                    void* l = locs[i];
                    if (!Plausible(l)) {
                        continue;
                    }
                    void* netRef = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(l) + 0x108);
                    if (!Plausible(netRef)) {
                        continue;
                    }

                    /* Four hops, the shape v84 walks. */
                    void* e4 = nullptr;
                    {
                        void* wrapper = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(netRef) + 0x68);
                        if (Plausible(wrapper)) {
                            void* inner = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                            if (Plausible(inner)) {
                                void* items = *reinterpret_cast<void**>(
                                    reinterpret_cast<unsigned char*>(inner) + 0x10);
                                const int n = *reinterpret_cast<int*>(
                                    reinterpret_cast<unsigned char*>(inner) + 0x18);
                                if (Plausible(items) && n > 0 && n <= 512) {
                                    locs4++;
                                    total4 += n;
                                    e4 = reinterpret_cast<void**>(items)[0];
                                }
                            }
                        }
                    }
                    if (Plausible(e4)) {
                        void* mt = *reinterpret_cast<void**>(e4);
                        bool seen = false;
                        for (int k = 0; k < mtCount; k++) {
                            if (mtSeen[k] == mt) {
                                seen = true;
                            }
                        }
                        if (!seen && mtCount < 8) {
                            mtSeen[mtCount++] = mt;
                        }
                        int off = -1;
                        unsigned char c2[2] = {0, 0};
                        if (nameOf(e4, &off, c2)) {
                            h4[(off - 0x08) / 8]++;
                            if (ch4[0] == 0) {
                                ch4[0] = c2[0];
                                ch4[1] = c2[1];
                            }
                        }
                    }

                    /* One hop, the shape `locs` is read with. */
                    void* arr = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(netRef) + 0x10);
                    const int n1 = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(netRef) + 0x18);
                    if (Plausible(arr) && n1 > 0 && n1 <= 512) {
                        locs1++;
                        total1 += n1;
                        void* e1 = reinterpret_cast<void**>(arr)[0];
                        if (Plausible(e1)) {
                            int off = -1;
                            unsigned char c2[2] = {0, 0};
                            if (nameOf(e1, &off, c2)) {
                                h1[(off - 0x08) / 8]++;
                                if (ch1[0] == 0) {
                                    ch1[0] = c2[0];
                                    ch1[1] = c2[1];
                                }
                            }
                        }
                    }
                }

                int off4 = -1;
                int agree4 = 0;
                int off1 = -1;
                int agree1 = 0;
                for (int k = 0; k < kSlots; k++) {
                    if (static_cast<int>(h4[k]) > agree4) {
                        agree4 = static_cast<int>(h4[k]);
                        off4 = k * 8 + 0x08;
                    }
                    if (static_cast<int>(h1[k]) > agree1) {
                        agree1 = static_cast<int>(h1[k]);
                        off1 = k * 8 + 0x08;
                    }
                }

                s_o0 = ((static_cast<unsigned>(total4) & 0xFFFFu) << 16) |
                    ((static_cast<unsigned>(locs4) & 0xFFu) << 8) |
                    (static_cast<unsigned>(agree4) & 0xFFu);
                s_o1 = ((static_cast<unsigned>(off4) & 0xFFFFu) << 16) |
                    (static_cast<unsigned>(ch4[0]) << 8) |
                    static_cast<unsigned>(ch4[1]);
                s_o2 = ((static_cast<unsigned>(mtCount) & 0xFFu) << 24) |
                    ((static_cast<unsigned>(total1) & 0xFFFFu) << 8) |
                    (static_cast<unsigned>(locs1) & 0xFFu);
                s_o3 = ((static_cast<unsigned>(off1) & 0xFFFFu) << 16) |
                    (static_cast<unsigned>(ch1[0]) << 8) |
                    static_cast<unsigned>(ch1[1]);
                s_o4 = ((static_cast<unsigned>(agree1) & 0xFFu) << 8) |
                    (static_cast<unsigned>(mtCount) & 0xFFu);
            }

            hexnum(bounds[0] + 120, bounds[1] + 110, s_o0, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 160, s_o1, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 210, s_o2, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 260, s_o3, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 310, s_o4, 8, 4);
            hexnum(bounds[0] + 120, bounds[1] + 360, 0x92, 2, 4);
        }
        /* One snapshot per 120 frames; count it after the full pass. */
        if ((s_frame % 120u) == 0u && s_round < 30u) {
            s_round++;
            Logging.Log(EXL_LOG_PREFIX "round %d done at frame %d",
                        static_cast<int>(s_round),
                        static_cast<int>(s_frame));
        }

        /* Stage 3 witness: a single square at a fixed map position, drawn only
         * once the whole reading pass has finished.  Seeing it means every
         * read above completed on retail hardware. */
        if (kMapStage == 3) {
            const u64 wxy = (static_cast<u64>(bounds[1] + 120) << 32) |
                            static_cast<u32>(bounds[0] + 120);
            draw(batch, texture, wxy, (24ull << 32) | 24u, 0xFFFF00FFu);
        }

        (void)lamp;
    }
}

    /* ---- Birthday HUD message, independent of the map page ----------------
     *
     * v152 pushed the toast from HookDrawMiniPortraits, so it only appeared
     * after opening the world map.  Game1.drawHUD (0x14888D0) runs every frame
     * of normal play and takes no arguments -- its prologue saves x19..x28 but
     * never x0.  addHUDMessage only queues, and the game draws the queue
     * itself, so pushing from here makes the toast appear right after a save
     * loads, the way a morning message does.
     */
    /* drawHUD ignores `this` entirely (its prologue never saves x0), but it is
     * still an instance method reached through a vtable slot, so x0 carries the
     * instance.  Forwarding it keeps the original call correct rather than
     * relying on x0 happening to survive our C++ code. */
    void (*s_origDrawHud)(void*) = nullptr;

    /* Self-contained list reader.  The map pass keeps its own copy as a
     * lambda, and a lambda cannot be shared with a separate function. */
    bool ReadListRaw(void* list, void*** out, int* count) {
        if (!Plausible(list) || !RangeReadable(list, 0x1Cu)) {
            return false;
        }
        auto* lb = reinterpret_cast<unsigned char*>(list);
        const int n = *reinterpret_cast<int*>(lb + 0x18);
        void* wrapper = *reinterpret_cast<void**>(lb + 0x10);
        if (n < 0 || n > 4096 || !Plausible(wrapper) ||
            !RangeReadable(wrapper, 0x28u)) {
            return false;
        }
        auto* wb = reinterpret_cast<unsigned char*>(wrapper);
        void* lengthPtr = *reinterpret_cast<void**>(wb + 0x20);
        if (!RangeReadable(lengthPtr, sizeof(int))) {
            return false;
        }
        const int capacity = *reinterpret_cast<int*>(lengthPtr);
        if (capacity < n || capacity < 0 || capacity > 65536) {
            return false;
        }
        void* buffer = *reinterpret_cast<void**>(wb + 0x10);
        if (n > 0 && (!Plausible(buffer) ||
                      !RangeReadable(buffer,
                                     static_cast<size_t>(n) * sizeof(void*)))) {
            return false;
        }
        *out = reinterpret_cast<void**>(buffer);
        *count = n;
        return true;
    }

    /* Managed display-name string of today's birthday villager, or null. */
    void* FindBirthdayName() {
        const int s = *reinterpret_cast<const int*>(g_base + kSeasonOff);
        const int d = *reinterpret_cast<const int*>(g_base + kDayOfMonthOff);
        if (s < 0 || s > 3 || d < 1 || d > 28) {
            return nullptr;
        }
        auto getLocations = reinterpret_cast<GetLocationsFn>(
            g_base + kGetLocationsOff);
        void* locList = getLocations();
        void** locs = nullptr;
        int locN = 0;
        if (!ReadListRaw(locList, &locs, &locN)) {
            return nullptr;
        }
        for (int i = 0; i < locN; i++) {
            void* loc = locs[i];
            if (loc == nullptr || !Plausible(loc) ||
                !RangeReadable(loc, 0xB8u)) {
                continue;
            }
            void* collection = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(loc) + 0xB0);
            if (collection == nullptr || !Plausible(collection) ||
                !RangeReadable(collection, 0x60u)) {
                continue;
            }
            void* inner = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(collection) + 0x58);
            void** npcs = nullptr;
            int n = 0;
            if (!ReadListRaw(inner, &npcs, &n)) {
                continue;
            }
            for (int j = 0; j < n; j++) {
                void* npc = npcs[j];
                if (npc == nullptr || !Plausible(npc) ||
                    !RangeReadable(npc, 0xC8u)) {
                    continue;
                }
                void* netName = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(npc) + 0xA8);
                if (netName == nullptr || !Plausible(netName) ||
                    !RangeReadable(netName, 0x70u)) {
                    continue;
                }
                void* nameStr = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(netName) + 0x68);
                if (nameStr == nullptr) {
                    continue;
                }
                char ascii[64];
                if (!NameToAscii(nameStr, ascii, 64)) {
                    continue;
                }
                int bs = 0;
                int bd = 0;
                if (!npcbday::Lookup(ascii, &bs, &bd) || bs != s || bd != d) {
                    continue;
                }
                /* Prefer the localized display name, like the avatars do. */
                void* display = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(npc) + 0xC0);
                if (display != nullptr && Plausible(display) &&
                    RangeReadable(display, 0x18u)) {
                    const int dl = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(display) + 0x10);
                    if (dl > 0 && dl < 64) {
                        return display;
                    }
                }
                return nameStr;
            }
        }
        return nullptr;
    }

    /* "今天是<name>的生日", built without allocating a literal.
     *
     * PadLeft(int totalWidth, char paddingChar) is 0x39AF40, confirmed against
     * a live caller at 0x15A3CEC (x0 = string, w1 = width, w2 = pad).  Padding
     * the name out to nameLen + 6 always allocates a fresh string of exactly
     * that length (it returns `this` only when no padding is needed, which
     * cannot happen here).  We then overwrite its character buffer: the length
     * field already matches and the string is not interned. */
    void PushBirthdayToast(void* nameStr) {
        if (nameStr == nullptr) {
            g_hudState = 3;
            return;
        }
        auto forCorner = reinterpret_cast<void* (*)(void*)>(g_base + 0x1586580);
        auto addHud = reinterpret_cast<void (*)(void*)>(g_base + 0x144EF50);
        const int nlen = ManagedStrLen(nameStr);
        void* final = nameStr;
        if (nlen > 0 && nlen <= 16) {
            const int total = nlen + 6;
            auto padLeft =
                reinterpret_cast<void* (*)(void*, int, char16_t)>(
                    g_base + 0x39AF40);
            void* buf = padLeft(nameStr, total, u'\u3000');
            if (buf != nullptr && buf != nameStr && Plausible(buf) &&
                ManagedStrLen(buf) == total &&
                RangeReadable(reinterpret_cast<unsigned char*>(buf) + 0x14,
                              static_cast<size_t>(total) * 2u)) {
                const char16_t* src = reinterpret_cast<const char16_t*>(
                    reinterpret_cast<unsigned char*>(nameStr) + 0x14);
                char16_t* dst = reinterpret_cast<char16_t*>(
                    reinterpret_cast<unsigned char*>(buf) + 0x14);
                dst[0] = u'\u4ECA';   /* 今 */
                dst[1] = u'\u5929';   /* 天 */
                dst[2] = u'\u662F';   /* 是 */
                for (int i = 0; i < nlen; i++) {
                    dst[3 + i] = src[i];
                }
                dst[3 + nlen] = u'\u7684';      /* 的 */
                dst[4 + nlen] = u'\u751F';      /* 生 */
                dst[5 + nlen] = u'\u65E5';      /* 日 */
                final = buf;
            }
        }
        void* msg = forCorner(final);
        if (msg != nullptr && Plausible(msg)) {
            addHud(msg);
            g_hudState = 1;
            unsigned pc = 0;
            const int fl = ManagedStrLen(final);
            if (fl > 0 && fl < 64) {
                const char16_t* fp = reinterpret_cast<const char16_t*>(
                    reinterpret_cast<unsigned char*>(final) + 0x14);
                for (int i = 0; i < 4 && i < fl; i++) {
                    pc |= static_cast<unsigned>(fp[i] & 0xFFu) << (i * 8);
                }
            }
            g_hudChars = pc;
        }
        else {
            g_hudState = 2;
        }
    }

    /* ---- FastAnimations slice 1: player single animations ---------------
     *
     * Hook the animation function ITSELF instead of Game1.Update.
     *
     * v154 called it from a Game1.Update hook, which meant rebuilding the
     * FarmerSprite pointer (player +0x50 -> NetRef -> +0x68) and guessing that
     * the second argument is the Farmer.  Neither could be verified: the callee
     * only forwards that argument to 0x92BC0, which is a bare field getter
     * (`ldr x0,[x0,#0x18]; ret`), so nothing identifies the object.
     *
     * Hooking 0x1400090 removes both unknowns -- the arguments are whatever the
     * game itself passed, so nothing has to be reconstructed or assumed.
     *
     * Scope: FarmerSprite is the PLAYER's sprite.  NPCs animate through
     * AnimatedSprite.UpdateSourceRect, which is a different function, so this
     * hook cannot touch villagers.
     *
     * The extra call is issued only after the original, and only while the
     * single animation is still playing (self+0xA4 is the flag the function
     * itself tests first), so a finished animation is never advanced again.
     * The original's return value is passed through untouched. */
    /* Defined below, next to the options plumbing.  The save is issued from
     * here because this is an Update-time hook, and the draw hook is the one
     * place it cannot happen (see TryPerformSave). */
    void TryPerformSave();

    bool HookCheckSingleAnimation(void* self, void* who) {
        const bool result = s_origCheckSingle(self, who);
        if (kDiagNoAnim) {
            return result;
        }
        /* Issue any pending preferences save from here, not from the draw
         * hook.  This runs in the normal Update pass, so allocating is
         * ordinary business; see the note on TryPerformSave for why drawHUD
         * is the one place it cannot happen.  Called once per animated
         * sprite, and g_saveWanted clears itself, so at most one save per
         * change. */
        TryPerformSave();
        if (self == nullptr || !Plausible(self) || !RangeReadable(self, 0xB0u)) {
            return result;
        }
        /* Latch the checkbox value only once the animation has ended. */
        if (*reinterpret_cast<unsigned char*>(
                reinterpret_cast<unsigned char*>(self) + 0xA4) == 0) {
            g_animFast = g_pendingFast;
            return result;
        }
        if (g_animFast <= 1 ||
            *reinterpret_cast<const unsigned char*>(g_base + kEventUpOff) != 0) {
            return result;          /* off, or a cutscene: vanilla speed */
        }
        g_animState = 1;
        for (int i = 1; i < g_animFast; i++) {
            if (*reinterpret_cast<unsigned char*>(
                    reinterpret_cast<unsigned char*>(self) + 0xA4) == 0) {
                break;
            }
            s_origCheckSingle(self, who);
            g_animExtra++;
            g_animTotal++;
        }
        return result;
    }
    /* Dialogue speed, fourth attempt -- the timer runs DOWN, not up.
     *
     * v172 added to DialogueBox+0xC0 and the text stopped appearing at all:
     * the player had to press A to make each line show.  Disassembling
     * DialogueBox.update (0x177BA70) explains why -- at 0x177C160:
     *
     *     w21 = *(int*)(self + 0xC0)        // characterAdvanceTimer
     *     w0  = elapsedMilliseconds()
     *     w8  = w21 - w0                    // subtract, not add
     *     *(int*)(self + 0xC0) = w8
     *     if (w8 < 1) {                     // expired -> reveal one character
     *         *(int*)(self + 0xC0) = 30;    // 0x1e, the vanilla reset value
     *         characterIndexInDialogue++;   // +0xC4
     *     }
     *
     * So the field is a countdown, not an accumulator: it starts at 30, loses
     * one frame's worth of milliseconds per tick, and reveals exactly one
     * character when it drops below 1.  Adding time therefore made it never
     * expire, which is exactly the symptom on hardware.
     *
     * Subtracting one extra frame's worth makes it expire on every tick
     * instead of every other tick.  That is the ceiling this mechanism allows:
     * the vanilla loop advances at most one character per update, so no matter
     * how large the nudge, the reveal cannot exceed one character a frame.
     * 2x is therefore the most dialogue text can do here; 3x would need
     * re-entering update, which is what crashed in v169/v170/v171.
     *
     * Still no re-entry, still nothing managed, still leaves safetyTimer and
     * the close sequence alone. */
    void HookDialogueUpdate(void* self, void* gameTime) {
        s_origDialogueUpdate(self, gameTime);
        if (g_animFast <= 1 || self == nullptr || !Plausible(self) ||
            !RangeReadable(self, 0xD0u)) {
            return;
        }
        if (*reinterpret_cast<const unsigned char*>(g_base + kEventUpOff) != 0) {
            return;                 /* cutscene: keep vanilla speed */
        }
        int* timer = reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(self) + 0xC0);
        const int t = *timer;
        /* After update returns the timer is either the vanilla reset value
         * (30) or a positive remainder, so anything outside that range means
         * this is not the countdown we think it is. */
        if (t < 0 || t > 2000) {
            return;
        }
        const int nudged = t - 16 * (g_animFast - 1);
        *timer = (nudged < -64) ? -64 : nudged;
    }

    /* Transport: the bus ride to the desert.
     *
     * BusStop owns the whole animation in its own fields (AOT metadata, type
     * factory 0x7168cdc, instance size 0x360):
     *
     *   busPosition     +0x320  Vector2   (8 bytes: X at 0x320, Y at 0x324)
     *   busMotion       +0x328  Vector2
     *   drivingOff      +0x330  bool
     *   drivingBack     +0x331  bool
     *   leaving         +0x332  bool
     *   forceWarpTimer  +0x334  int
     *
     * BusStop.UpdateWhenCurrentLocation (0x15DCBC0) is what advances it.  It is
     * an override -- its first statement calls the base at 0x14E5E30 -- and it
     * has zero direct `bl` callers, which is what a virtual override looks
     * like; the vtable still dispatches to this body, so hooking the body works.
     *
     * This one cannot be double-called the way checkForSingleAnimation is:
     * that base call ticks every character and object in the location, and the
     * tail runs forceWarpTimer and the warp itself.  So instead of re-entering,
     * read busPosition before and after the single vanilla call and extrapolate
     * along the same delta.  Nothing else is touched -- no callback can fire
     * twice, no timer is double-decremented -- and if the delta does not look
     * like one frame of movement the hook does nothing at all. */
    void (*s_origBusUpdate)(void*, void*) = nullptr;
    void (*s_origDesertUpdate)(void*, void*) = nullptr;

    /* Bus ride: where the speed actually comes from.
     *
     * A desert trip is: buy-ticket dialogue -> bus crossing -> fade -> warp.
     * Boarding (0x15DC0B8 `mov w8,#0x1f40`) sets forceWarpTimer (+0x334) to
     * 8000 ms and the bus then crosses ~502 px in ~116 frames (~1.9 s).  So
     * six of those eight seconds are pure waiting on the timer, not motion.
     * That is why v175 (position only, 2x) felt almost unchanged: the sprite
     * finished early and the ride still took the full eight seconds.
     *
     * v176 multiplied the position 4x on top of that.  On hardware the bus
     * visibly jumped forward, snapped back to where it had been, and carried
     * on -- our write is a position the vanilla state machine never produced,
     * and vanilla's own absolute corrections (busPosition.X is hard-set to
     * 1344.0f at 0x15DCDD0, and the stop path at 0x15DCCD4 depends on where
     * the bus is) then fight it.  Nothing in the game is expecting a 4x step.
     *
     * So the extra speed is taken from the TIMER, not from the sprite:
     *
     *   - position / door:  kBusPosMul while leaving the stop, 2x while
     *     arriving (see below -- the arrival test is why they differ).
     *   - forceWarpTimer:   kBusTimerMul, independent and much larger, because
     *     shortening a countdown is a plain integer subtract with no geometry
     *     involved -- there is nothing for it to disagree with.
     *
     * Net effect: the bus animates at the speed that was already proven safe,
     * and the ride ends when the bus is done instead of six seconds later. */
    /* 3, not 4.  `leaving` (which starts the 0.83 s fade) fires the moment the
     * bus crosses pos < -502, and at kBusCruiseSpeed that is frame ~85.  The
     * warp lives inside the countdown, so the countdown must not reach zero
     * before the fade finishes (~frame 135) or the map switches while the
     * screen is still ~0.8 alpha and the change is visible.  8000 ms at
     * 16 + 32 ms per frame is ~164 frames, which clears it. */
    constexpr int kBusTimerMul = 3;

    /* Take the sprite over and drive it ourselves.
     *
     * What the ride actually is, from the disassembly (update 0x15DCBC0):
     *
     *   departure  `drivingOff` (+0x330) = 1, set by 0x15DC944.  That branch
     *              (0x15DCCA8) only does `busMotion.X -= 0.075` and skips the
     *              stop test entirely, because the bus is leaving, not
     *              docking.  From a standstill that is a 3.7 s crawl: it needs
     *              222 frames at that acceleration to cover the 1846 px from
     *              the stop (1344) to the off-screen test `pos < -502`.
     *
     *   arrival    `drivingBack` (+0x331) = 1, set by 0x15DC808 together with
     *              `busMotion = (-12, 0)`.  That branch (0x15DCD20) decays the
     *              speed with `Math.Min(-1, motion * 0.98)`, i.e. 12 -> 1 in
     *              123 frames covering 550 px, and then CRAWLS THE REST AT
     *              1 px/frame -- 26 more frames for the last 26 px.  That tail
     *              is the "still slow" slide into the stop, on both maps.
     *
     * Neither branch can be sped up by writing busMotion: on the arrival the
     * Min(-1, ...) clamps any magnitude above 1 straight back to -1, and on
     * the departure the game adds its own -0.075 on top of whatever we wrote.
     * That is why v181 moved the speed and the bus did not move faster.
     *
     * So the position is written directly, once per frame, with an ABSOLUTE
     * step instead of a multiple of vanilla's delta:
     *
     *   - |step| = kBusCruiseSpeed, tapering linearly inside kBusSlowZone,
     *     floored at kBusMinSpeed.
     *   - direction = the sign of busMotion, which is the one honest signal
     *     that a ride is in progress (a parked bus has busMotion == 0, and
     *     both direction flags get cleared by 0x15DCC1C whenever the current
     *     location is not this bus stop -- i.e. exactly during the map switch,
     *     which is what made v178 hand half the trip back to vanilla).
     *
     * Docking is ours as well.  The stop test is
     *
     *     |pos - 1344| <= |busMotion| * 1.5   ->   busPosition.X = 1344.0f
     *
     * at 0x15DCDC0..0x15DCDCC.  Because we write the position ourselves, that
     * test reads OUR position, so we can guarantee what it sees: the taper
     * brings the step down to kBusMinSpeed, and once the remaining distance is
     * down to that, the position is set to exactly 1344 ourselves.  Vanilla
     * then trips its test on a zero-length correction and runs its own arrival
     * cleanup (sound, rumble, motion = 0) -- so nothing is skipped and there is
     * nothing left for it to snap.  Every "flew forward then snapped back"
     * report was the same thing: a lead vanilla did not know about, cancelled
     * by that hard write.  There is no lead any more.
     *
     * The same step is applied to the bus door.  Vanilla moves the door at
     * 0x15DCE38 by adding busMotion to its own position, so pushing only
     * busPosition detaches the door by exactly our step (the v174 lesson).
     *
     * kBusStopX is not a guess: 1344.0f is the literal in the hard write at
     * 0x15DCDC8 (mov w8, #0x44a80000), so it is the stop on whichever map this
     * runs -- town and desert share this body. */
    constexpr float kBusCruiseSpeed = 24.0f;
    constexpr float kBusSlowZone = 96.0f;
    constexpr float kBusMinSpeed = 2.0f;

    /* ONE implementation, TWO classes.
     *
     * StardewValley.Locations.BusStop (the road east of the farm) and
     * StardewValley.Locations.Desert carry the SAME ride code -- same
     * `busMotion.X -= 0.075` departure, same `Math.Min(-1, x * 0.98)`
     * arrival, same `|pos - stop| <= |motion| * 1.5` stop test followed by a
     * hard write -- but with different field offsets and a different stop
     * coordinate.  Both come straight out of the AOT metadata:
     *
     *                        BusStop (0x360)   Desert (0x370)
     *     busDoor            +0x318            +0x308
     *     busPosition        +0x320            +0x310
     *     busMotion          +0x328            +0x318
     *     drivingOff/Back    +0x330/+0x331     +0x320/+0x321
     *     leaving            +0x332            +0x322
     *     forceWarpTimer     +0x334            (none)
     *     update body        0x15DCBC0         0x15FD310
     *     stop X             1344.0f           1088.0f
     *
     * v181 hooked only BusStop, which is exactly why the town legs got fast
     * and the desert legs did not: the desert map is a different location
     * object and never reaches 0x15DCBC0. */
    void DriveBusRide(unsigned char* b, unsigned posOff, unsigned motOff,
                      unsigned doorOff, float stopX) {
        float* busPos = reinterpret_cast<float*>(b + posOff);
        const float mot = *reinterpret_cast<float*>(b + motOff);

        /* Zero means parked, or the arrival just cleared it.  Anything else
         * means a ride is in progress, whichever map it is on. */
        if (mot == 0.0f) {
            return;
        }
        const float dir = (mot < 0.0f) ? -1.0f : 1.0f;
        const float x = *busPos;
        float d = x - stopX;
        if (d < 0.0f) {
            d = -d;
        }
        float mag = kBusCruiseSpeed;
        if (d < kBusSlowZone) {
            mag = (d / kBusSlowZone) * kBusCruiseSpeed;
        }
        if (mag < kBusMinSpeed) {
            mag = kBusMinSpeed;
        }
        /* `toward` is what separates "docking" from "pulling out": both start
         * at distance 0, only one of them is heading at the stop. */
        const bool toward = ((stopX - x) * dir) > 0.0f;
        float step = dir * mag;
        if (toward && d <= kBusMinSpeed) {
            step = stopX - x;
        }
        *busPos = x + step;

        void* door = *reinterpret_cast<void**>(b + doorOff);
        if (door != nullptr && Plausible(door) && RangeReadable(door, 0xD4u)) {
            *reinterpret_cast<float*>(
                reinterpret_cast<unsigned char*>(door) + 0xCC) += step;
        }
    }

    void HookBusUpdate(void* self, void* gameTime) {
        const bool track = (g_animFast > 1) && self != nullptr &&
            Plausible(self) && RangeReadable(self, 0x340u);
        s_origBusUpdate(self, gameTime);
        if (!track) {
            return;
        }
        auto* b = reinterpret_cast<unsigned char*>(self);
        DriveBusRide(b, 0x320u, 0x328u, 0x318u, 1344.0f);

        int* warpTimer = reinterpret_cast<int*>(b + 0x334);
        const int t = *warpTimer;
        if (t > 1) {
            const int nudged = t - 16 * (kBusTimerMul - 1);
            *warpTimer = (nudged < 1) ? 1 : nudged;
        }
    }

    /* The desert side.  Same ride, different class -- see DriveBusRide.  There
     * is no forceWarpTimer here: `Desert` has no such field, the warp is
     * driven by busLeftToValley instead, so only the motion needs work. */
    void HookDesertUpdate(void* self, void* gameTime) {
        const bool track = (g_animFast > 1) && self != nullptr &&
            Plausible(self) && RangeReadable(self, 0x340u);
        s_origDesertUpdate(self, gameTime);
        if (!track) {
            return;
        }
        DriveBusRide(reinterpret_cast<unsigned char*>(self),
                     0x310u, 0x318u, 0x308u, 1088.0f);
    }

    /* Capture the StartupPreferences singleton.
     *
     * loadPreferences is an instance method with three extra arguments
     * (w1/w2/w3 are all read in the prologue), so they have to be forwarded
     * verbatim -- dropping them would change the call's behaviour, and the
     * trampoline would still return so nothing would look wrong.
     *
     * This exists so SyncFastFromOptions can call savePreferences on the same
     * object the boot path uses.  See the comment there for why. */
    void HookLoadPreferences(void* self, int a, int b, int c) {
        if (s_dbgLoadRuns < 0xFu) {
            ++s_dbgLoadRuns;      /* v387: proves hooks fire at all */
        }
        g_startupPrefs = self;
        /* Null-guarded only because a failed hook here would crash during boot,
         * before anything can report it.  Skipping the original loses the
         * loaded preferences, which is bad but survivable; a jump through null
         * is not. */
        if (s_origLoadPrefs != nullptr) {
            s_origLoadPrefs(self, a, b, c);
        }
        /* v346: the previous session's last action, straight out of the file.
         * Snapshotted here, before anything else can touch timesPlayed. */
        if (self != nullptr &&
            RangeReadable(reinterpret_cast<unsigned char*>(self) + 0x18, 4u)) {
            s_tracePrev = *reinterpret_cast<unsigned*>(
                reinterpret_cast<unsigned char*>(self) + 0x18);
            s_trace = s_tracePrev;
        }
    }

    /* v199 write-path probes.  Both are pass-through: set a flag, run the
     * original, set another flag.  The second flag is what matters -- if the
     * original throws, control never reaches it, so "entered but not back"
     * IS the evidence that the throw happened here.
     *
     * These run on a thread-pool thread (savePreferences wraps the write in
     * new Task(...) + Start), so nothing here may allocate or block. */
    void HookSavePreferencesImpl(void* self) {
        if (s_wrEntered < 15u) {
            s_wrEntered++;
        }
        if (s_origSavePrefsImpl != nullptr) {
            s_origSavePrefsImpl(self);
        }
        s_wrBack = 1;
        s_wrBackAll = 1;
    }

    void HookWriteSettings(void* self, void* writer) {
        if (s_wrWrite < 15u) {
            s_wrWrite++;
        }
        if (s_evtWrite == 0xFFu) {
            s_evtWrite = s_evtSeq & 0xFFu;
        }
        s_evtSeq = s_evtSeq + 1u;
        /* v208: NO memory access here.
         *
         * This runs on a THREAD-POOL thread (savePreferences wraps the write in
         * `new Task(...)` + Start).  v207 added a Plausible()/RangeReadable()
         * probe here and that was wrong: both share one unsynchronised
         * 256-slot page cache (Plausible's sPage/sOk), and until now every
         * caller was on the main thread -- Draw and Update are both main.  A
         * worker thread reading that cache can see sPage[slot] from one lookup
         * and sOk[slot] from another, and a wrong "mapped" verdict is a wild
         * dereference, i.e. a crash.  The probe is not worth that.
         *
         * Only the event counter is touched.  A lost increment on the volatile
         * counter is possible and harmless -- it can only misnumber an event,
         * never fault. */
        (void)self;
        if (s_origWriteSettings != nullptr) {
            s_origWriteSettings(self, writer);
        }
        s_wrWriteBack = 1;
        s_wrWriteBackAll = 1;
    }

    /* v212: append into a List<T> using the layout the options page already
     * relies on: _items@+0x10 -> wrapper, wrapper+0x10 = element array,
     * wrapper+0x20 = capacity pointer, list+0x18 = _size.
     *
     * _version (+0x1C) is deliberately NOT bumped.  The game's tab loop is a
     * foreach that snapshots _version and throws if it changes mid-iteration;
     * leaving it alone means an append between frames is invisible to it. */
    bool AppendToList(void* list, void* item) {
        if (list == nullptr || item == nullptr || !Plausible(list) ||
            !RangeReadable(list, 0x1Cu)) {
            return false;
        }
        const int cnt = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(list) + 0x18);
        void* wrapper = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(list) + 0x10);
        if (cnt < 0 || cnt > 255 || wrapper == nullptr || !Plausible(wrapper) ||
            !RangeReadable(wrapper, 0x28u)) {
            return false;
        }
        void* capPtr = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(wrapper) + 0x20);
        void* buf = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(wrapper) + 0x10);
        if (capPtr == nullptr || buf == nullptr || !RangeReadable(capPtr, 4u) ||
            !Plausible(buf)) {
            return false;
        }
        if (*reinterpret_cast<int*>(capPtr) <= cnt) {
            /* v375: full.  v373 called List.Add here and it reported success but
             * the element never showed up, so this version only asks the list to
             * GROW and then performs the same store as the path above -- no
             * dependency on Add's argument order.  Grow(1) is what Add itself
             * passes (0x170D3B8), and List.Grow doubles from the current size. */
            auto grow = reinterpret_cast<void (*)(void*, int)>(g_base + kOptListGrowOff);
            grow(list, 1);
            wrapper = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(list) + 0x10);
            if (wrapper == nullptr || !Plausible(wrapper) ||
                !RangeReadable(wrapper, 0x28u)) {
                return false;
            }
            buf = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(wrapper) + 0x10);
            if (buf == nullptr || !Plausible(buf)) {
                return false;
            }
            /* v376 SAFETY: v375 wrote without re-checking the capacity, so if
             * Grow had not actually grown the list that write went past the
             * end of the buffer.  Re-read the capacity and refuse to write
             * unless there really is a free slot. */
            void* cap2 = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(wrapper) + 0x20);
            if (cap2 == nullptr || !RangeReadable(cap2, 4u) ||
                *reinterpret_cast<int*>(cap2) <= cnt) {
                return false;
            }
            reinterpret_cast<void**>(buf)[cnt] = item;
            *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(list) + 0x18) = cnt + 1;
            return true;
        }
        reinterpret_cast<void**>(buf)[cnt] = item;
        *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(list) + 0x18) = cnt + 1;
        return true;
    }

    /* v216: fresh managed string, built with the recipe the shipped checkbox
     * has used since v168 -- deliberately NOT String.CreateString 0x3A0100.
     *
     * `PadLeft(n)` only allocates when the source is shorter than n, but
     * `Substring` always hands back a different object, so
     * `substr(padLeft(src, len), 0, len)` is a fresh len-char string whatever
     * src is.  We then overwrite its characters in place; the length lives at
     * +0x10 and the characters start at +0x14 (ManagedStrLen and the append
     * block both depend on that layout, and both are proven on hardware).
     *
     * Why not CreateString: the tab code was its first and only caller in this
     * project, and v212/v213 both froze without producing a reading.  An object
     * the GC cannot walk kills the mark phase, and a dead mark phase looks
     * exactly like the freeze we are chasing -- so the unproven path goes
     * first, before another boot is spent on it. */
    void* MakeFreshString(void* src, const char16_t* text, int len) {
        if (src == nullptr || text == nullptr || len <= 0 || len > 32) {
            return nullptr;
        }
        if (ManagedStrLen(src) < 1) {
            return nullptr;
        }
        auto padLeft = reinterpret_cast<void* (*)(void*, int, char16_t)>(
            g_base + kPadLeftOff);
        auto substr = reinterpret_cast<void* (*)(void*, int, int)>(
            g_base + kSubstrOff);
        void* wide = padLeft(src, len, u'\u3000');
        if (wide == nullptr || !Plausible(wide)) {
            return nullptr;
        }
        void* out = substr(wide, 0, len);
        if (out == nullptr || !Plausible(out)) {
            return nullptr;
        }
        if (out == src) {
            /* Substring handed back the source itself; overwriting it would
             * rename somebody else's tab.  Refuse rather than corrupt. */
            return nullptr;
        }
        if (ManagedStrLen(out) != len) {
            return nullptr;
        }
        if (!RangeReadable(reinterpret_cast<const unsigned char*>(out) + 0x14,
                           static_cast<size_t>(len) * 2u)) {
            return nullptr;
        }
        char16_t* p = reinterpret_cast<char16_t*>(
            reinterpret_cast<unsigned char*>(out) + 0x14);
        for (int i = 0; i < len; i++) {
            p[i] = text[i];
        }
        return out;
    }

    /* v232: which tab's icon we want, and how our own name is built.
     *
     * v231 borrowed the tab NAME itself (a copy with the same content) so the
     * game's own icon lookup would match.  On hardware the icon appeared but
     * clicking the tab FROZE the game -- a second tab whose name content
     * equals another tab's is not something the game tolerates.  GameMenu has
     * several CONTENT comparisons on the tab name (the icon lookup, the
     * hover-text check, a block at 0x1798AF4 that runs for one particular
     * name), and the click path resolves the name to a tab number.
     *
     * So the name goes back to being unique, and only the icon lookup is
     * fooled: String.Equals is hooked and answers "yes" for exactly one pair --
     * our name against the target tab's name object.  Everything else in the
     * game keeps seeing a name that matches nothing.
     *
     * Two constraints come from the lookup itself: it picks its branch by the
     * name's LENGTH (3..11) and then checks the FIRST character before the
     * full comparison.  So our name is built with the target's length and the
     * target's first character, and a body that is unique. */
    void* PickIconTarget(void** tabBuf, int gameTabCount) {
        /* v235: the icon strip in Game1.mouseCursors is laid out in tab order,
         * 16px per cell at y=368 (verified by cutting the strip out of the PC
         * Cursors.png):
         *
         *   x=0   背包(0)      x=32  心/社交(2)   x=64  锤子/制作(4)   x=96  头盔(6)
         *   x=16  空白          x=48  地图(3)      x=80  罐子(5)       x=112 禁止(7)
         *
         * The game has no gear at all, so the hammer is the closest thing to a
         * settings/tools icon -- it is the crafting tab's (index 4).  Prefer it,
         * then the rest in order, with the plain backpack last. */
        static const int kOrder[] = {4, 6, 7, 1, 2, 3, 5, 0};
        for (unsigned pass = 0;
             pass < sizeof(kOrder) / sizeof(kOrder[0]); pass++) {
            {
                const int i = kOrder[pass];
                if (i >= gameTabCount) {
                    continue;
                }
                void* comp = tabBuf[i];
                if (comp == nullptr || !Plausible(comp) ||
                    !RangeReadable(comp, 0x28u)) {
                    continue;
                }
                void* nm = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(comp) + 0x20);
                if (nm == nullptr || !Plausible(nm)) {
                    continue;
                }
                const int len = ManagedStrLen(nm);
                if (len < 3 || len > 11) {
                    continue;       /* the lookup skips these lengths anyway */
                }
                if (!RangeReadable(reinterpret_cast<unsigned char*>(nm) + 0x14,
                                   static_cast<size_t>(len) * 2u)) {
                    continue;
                }
                return nm;
            }
        }
        return nullptr;
    }

    /* v232: a unique name that still satisfies the lookup's pre-filter:
     * the target's length, the target's first character, and a body that
     * cannot equal any of the game's tab names. */
    void* MakeIconName(void* target) {
        const int len = ManagedStrLen(target);
        if (len < 3 || len > 11) {
            return nullptr;
        }
        static const char16_t kBody[] = u"ModTab";
        char16_t buf[16];
        const char16_t* from = reinterpret_cast<const char16_t*>(
            reinterpret_cast<unsigned char*>(target) + 0x14);
        buf[0] = from[0];
        for (int i = 1; i < len; i++) {
            buf[i] = kBody[(i - 1) % 6];
        }
        return MakeFreshString(target, buf, len);
    }

    /* Defined further down, next to the shrink it exists for. */
    bool OptionsBacking(void* page, void*** bufOut, int* capOut, int** sizeOut);

    void TryBuildModTab(void* gm) {
        if (!kModTab) {
            return;
        }
        /* v216: RE-ARM FOR A NEW GameMenu.
         *
         * GameMenu..ctor builds `tabs`/`pages` from scratch on every open, so
         * our entry disappears with the old instance while s_tabState stays 1 --
         * which is why the tab only ever showed up on the first open.  Tracking
         * the GameMenu pointer does not help: the allocator hands the same
         * address back.  Test the list instead, and only in the success state:
         * a FAILURE code must keep latching, otherwise a doomed attempt would
         * retry (and leak) on every frame. */
        if (s_tabState == 1u) {
            bool stillThere = false;
            if (s_modTab != nullptr && gm != nullptr && Plausible(gm) &&
                RangeReadable(gm, 0x90u)) {
                void* tabsNow = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(gm) + kGmTabsOff);
                if (tabsNow != nullptr && Plausible(tabsNow) &&
                    RangeReadable(tabsNow, 0x1Cu)) {
                    void* wrapper = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(tabsNow) + 0x10);
                    if (wrapper != nullptr && Plausible(wrapper) &&
                        RangeReadable(wrapper, 0x28u)) {
                        void** buf = *reinterpret_cast<void***>(
                            reinterpret_cast<unsigned char*>(wrapper) + 0x10);
                        const int n = *reinterpret_cast<int*>(
                            reinterpret_cast<unsigned char*>(tabsNow) + 0x18);
                        if (buf != nullptr && Plausible(buf) &&
                            n > kModTabIndex) {
                            stillThere = (buf[kModTabIndex] == s_modTab);
                        }
                    }
                }
            }
            if (stillThere) {
                return;
            }
            s_tabState = 0u;
            s_modTab = nullptr;
            s_modTabName = nullptr;   /* managed: let it go with the old menu */
            s_modTabLabel = nullptr;
            s_modIconTarget = nullptr;
            s_pageReady = 0u;
            s_modPage = nullptr;      /* v220: the page died with the menu */
            /* v221: and the checkbox label.  It used to survive the rebuild,
             * which meant the second menu open reused a string that died with
             * the first menu's element -- a dangling managed reference that
             * only shows up on the SECOND open.  Every managed object we keep
             * in a static has to be dropped here, not just the ones the tab
             * happens to need. */
            s_modElemLabel = nullptr;
            s_modElem2Label = nullptr;
            g_ourElem = nullptr;
            g_ourCount = 0;
            for (int i = 0; i < kRowCount; i++) {
                g_ourElems[i] = nullptr;
            }
            /* v317: and the saved list order, which belonged to that page. */
            s_origCount = 0;
            s_shrinkState = 0;
            s_shrinkCount = -1;
            /* v228: the page pointer and any outstanding shrink die with the
             * menu too.  Leaving s_savedOptCount set would make the next menu's
             * first frame "restore" a count onto a list it never shrank. */
            s_vanillaPage = nullptr;
            s_savedOptCount = -1;
            /* v302: the element died with that page too.  Dropping the pointer
             * keeps the next menu from comparing against a stale address. */
            g_ourElem = nullptr;
            g_appendedTo = nullptr;
            g_appendCount = 0;
        }
        else if (s_tabState != 0u) {
            return;
        }
        /* v214: RE-ENTRANCY GUARD.
         *
         * s_tabState used to be set to 1 only at the very end, so anything we
         * call that re-entered GameMenu.update would come back here with the
         * state still 0 and build ANOTHER page and tab -- and again, until the
         * stack blew.  A stack overflow on the main thread looks exactly like
         * the freeze we are chasing: the game stops, the console does not.
         *
         * Marking the attempt as in-progress up front makes re-entry a no-op.
         * 2 = in progress; the paths that bail out set their own code, and the
         * one that is merely "not ready yet" puts the state back to 0 so the
         * next frame retries. */
        s_tabState = 2u;
        if (gm == nullptr || !Plausible(gm) || !RangeReadable(gm, 0x90u)) {
            s_tabState = 10u;
            return;
        }
        unsigned char* g = reinterpret_cast<unsigned char*>(gm);
        void* tabs = *reinterpret_cast<void**>(g + kGmTabsOff);
        void* pages = *reinterpret_cast<void**>(g + kGmPagesOff);
        if (tabs == nullptr || pages == nullptr || !Plausible(tabs) ||
            !Plausible(pages) || !RangeReadable(tabs, 0x1Cu) ||
            !RangeReadable(pages, 0x1Cu)) {
            s_tabState = 11u;
            return;
        }
        const int pageN = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(pages) + 0x18);
        const int tabN = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(tabs) + 0x18);
        if (pageN < 10 || tabN < 10) {
            s_tabState = 0u;            /* ctor still running: retry next frame */
            return;
        }
        if (pageN != 10 || tabN != 10) {
            s_tabState = 12u;           /* already extended by something else */
            return;
        }

        void* pageWrapper = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(pages) + 0x10);
        void* tabWrapper = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(tabs) + 0x10);
        if (pageWrapper == nullptr || tabWrapper == nullptr ||
            !RangeReadable(pageWrapper, 0x28u) || !RangeReadable(tabWrapper, 0x28u)) {
            s_tabState = 13u;
            return;
        }
        void** pageBuf = *reinterpret_cast<void***>(
            reinterpret_cast<unsigned char*>(pageWrapper) + 0x10);
        void** tabBuf = *reinterpret_cast<void***>(
            reinterpret_cast<unsigned char*>(tabWrapper) + 0x10);
        if (pageBuf == nullptr || tabBuf == nullptr) {
            s_tabState = 14u;
            return;
        }
        void* optPage = pageBuf[8];     /* the vanilla Options page */
        void* optTab = tabBuf[8];
        void* exitTab = tabBuf[9];
        if (optPage == nullptr || optTab == nullptr || exitTab == nullptr ||
            !Plausible(optPage) || !Plausible(optTab) || !Plausible(exitTab) ||
            !RangeReadable(optPage, 0x38u) || !RangeReadable(optTab, 0x20u) ||
            !RangeReadable(exitTab, 0x20u)) {
            s_tabState = 15u;
            return;
        }

        /* v216: source string for the fresh managed strings below.  The exit
         * tab's own name is a live, already-validated managed string, and it is
         * longer than 3 characters -- so PadLeft(3) cannot hand it back
         * unchanged, and Substring(0,3) is guaranteed to allocate.  The
         * checkbox path uses the player's name for the same job, but the player
         * can still be null when a menu opens and the exit tab never is. */
        void* srcName = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(exitTab) + 0x20);
        if (srcName == nullptr || !Plausible(srcName)) {
            s_tabState = 16u;
            return;
        }

        /* ---- v221: NO MANAGED STRING MAY BE CREATED BEFORE ITS OWNER ----
         *
         * v217 moved the tab ahead of the page to shrink the window in which
         * s_modTabName is referenced only by a native static (not a GC root).
         * That was not enough: the string was still created *first*, and the
         * next allocation after it -- the tab's own constructor -- is exactly
         * where a collection can run.  v217 survived that, v219 (identical
         * code) did not, which is what makes this a timing bug and not a
         * logic bug.
         *
         * So invert it: build the component with an existing, game-held string
         * as a placeholder, then create ours and write it straight into the
         * object.  Between `MakeFreshString` returning and the two stores
         * below there is no allocation at all, so no collection can see the
         * string unrooted.  +0x20 = name, +0x28 = label (the same two words
         * ClickableComponent..ctor stores from its x1/x2). */

        /* ---- v217: TAB COMPONENT FIRST, THEN THE PAGE ----
         *
         * The order is about the GC, not about layout.  s_modTabName lives in a
         * native static, and a native static is NOT a GC root: between creating
         * the string and storing it into a managed object, a collection would
         * free it and leave the tab holding a dangling reference.  Building the
         * tab first shrinks that window to a few instructions, and it puts the
         * string behind a real root BEFORE the big allocation below --
         * OptionsPage..ctor builds ~40 elements and is by far the most likely
         * place for a collection to run.
         *
         * v216's reading (row 1 = 01 01 23 00) proved the component and the
         * append are safe on hardware: s_tabState = 1, appendState = 1, count
         * 35, and the menu did not freeze.  So this stage adds only the page.
         *
         * Tab component: same bounds as the exit tab, shifted one slot right by
         * the spacing the game itself uses between tabs 8 and 9. */
        const unsigned char* et = reinterpret_cast<const unsigned char*>(exitTab);
        const unsigned char* ot = reinterpret_cast<const unsigned char*>(optTab);
        const int dx = *reinterpret_cast<const int*>(et + 0x10) -
                       *reinterpret_cast<const int*>(ot + 0x10);
        const int bx = *reinterpret_cast<const int*>(et + 0x10) + dx;
        const int by = *reinterpret_cast<const int*>(et + 0x14);
        const int bw = *reinterpret_cast<const int*>(et + 0x18);
        const int bh = *reinterpret_cast<const int*>(et + 0x1C);
        const u64 b0 = static_cast<u64>(static_cast<u32>(bx)) |
                       (static_cast<u64>(static_cast<u32>(by)) << 32);
        const u64 b1 = static_cast<u64>(static_cast<u32>(bw)) |
                       (static_cast<u64>(static_cast<u32>(bh)) << 32);
        auto tabCtor = reinterpret_cast<void* (*)(void*, u64, u64, void*, void*)>(
            g_base + kTabCtorOff);
        /* Placeholder name/label: the exit tab's string, which the game owns
         * and keeps alive.  Renamed after the append -- see the v224 note
         * below: the rename must not happen while the component is still
         * unrooted, because MakeFreshString allocates. */
        void* tab = tabCtor(nullptr, b0, b1, srcName, srcName);
        if (tab == nullptr || !Plausible(tab) || !RangeReadable(tab, 0x70u)) {
            s_tabState = 22u;
            return;
        }
        /* v213: FILL IN THE IDENTITY AND NEIGHBOUR FIELDS.  v212 left them at
         * the constructor's defaults and that is what froze the console.
         *
         * The game writes these itself for every tab it creates (GameMenu..ctor,
         * e.g. 0x17968E4 for the options tab and 0x17969E4 for the exit tab):
         *
         *   +0x45 fullyImmutable               = 1
         *   +0x48 myID                          = 0x3034 + index
         *   +0x4C myAlternateID                 = -1
         *   +0x50 leftNeighborID                = previous tab's myID
         *   +0x54 rightNeighborID               = next tab's myID, 0 = none
         *   +0x58 upNeighborID                  = 0 (none)
         *   +0x5C downNeighborID                = own index
         *   +0x64 tryDefaultIfNoRightNeighbour  = 0
         *   +0x65 tryDefaultIfNoDownNeighbour   = 1
         *
         * The constructor leaves myID at 0xFFFFFE0C (-500) and the neighbour
         * ids at 0.  v216 shipped with these fields filled and did not freeze,
         * so they stay as they are -- but note that v216 also never let the
         * cursor reach the tab, so this is still unproven for navigation.
         *
         * The ids are a linked list: our left points at the exit tab, and the
         * exit tab's right has to point back at us, otherwise the chain ends at
         * the exit tab and the cursor can never reach the new one. */
        unsigned char* tb = reinterpret_cast<unsigned char*>(tab);
        *reinterpret_cast<int*>(tb + 0x48) = 0x3034 + kModTabIndex;  /* myID */
        *reinterpret_cast<int*>(tb + 0x4C) = -1;                     /* myAlternateID */
        *reinterpret_cast<int*>(tb + 0x50) = 0x3034 + 9;             /* left = exit */
        *reinterpret_cast<int*>(tb + 0x54) = 0;                      /* right = none */
        *reinterpret_cast<int*>(tb + 0x58) = 0;                      /* up = none */
        *reinterpret_cast<int*>(tb + 0x5C) = kModTabIndex;           /* down = self */
        tb[0x45] = 1;                                               /* fullyImmutable */
        tb[0x64] = 0;
        tb[0x65] = 1;
        if (!AppendToList(tabs, tab)) {
            s_tabState = 23u;
            return;
        }
        /* ---- v224: root first, then rename ----
         *
         * v223 rooted the PAGE before allocating, but the tab and the element
         * were still built, renamed and only then appended -- and the rename
         * itself (MakeFreshString -> PadLeft + Substring) is an allocation.
         * So the component sat unrooted across an allocation exactly like the
         * page used to, and could be collected out from under us.
         *
         * The order that is actually safe, for every object we create:
         *     construct with a placeholder -> append to the managed list
         *     -> create the string -> write it in (no allocation after that)
         *
         * If the string cannot be made we take the tab back out, so the strip
         * never keeps an entry still carrying the exit tab's name (that would
         * make getTabNumberFromName dispatch our tab to the exit tab). */
        if (s_modTabName == nullptr) {
            static const char16_t kName[] = u"Mod";
            s_modTabLabel = MakeFreshString(srcName, kName, 3);
            s_modTabName = s_modTabLabel;   /* replaced below if an icon name is built */
        }
        if (s_modTabName == nullptr) {
            *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(tabs) + 0x18) -= 1;
            s_tabState = 16u;
            return;
        }
        {
            unsigned char* nb = reinterpret_cast<unsigned char*>(tab);
            *reinterpret_cast<void**>(nb + 0x20) = s_modTabName;  /* name */
            *reinterpret_cast<void**>(nb + 0x28) = s_modTabLabel; /* label */

            /* v232: give the tab an icon -- WITHOUT giving it another tab's
             * name.
             *
             * v231 copied a game tab's name so the game's own icon lookup
             * would match by content.  The icon appeared, but clicking the tab
             * froze the game: GameMenu has several CONTENT comparisons on the
             * tab name, and a second tab carrying an existing name is not
             * something it survives.
             *
             * So the name stays unique and only the lookup is fooled: see
             * HookStringEquals, which answers "equal" for exactly one pair --
             * our name and s_modIconTarget (the tab whose icon we want).
             *
             * Our name still has to satisfy the lookup's pre-filter, which is
             * the target's LENGTH plus its FIRST CHARACTER -- MakeIconName
             * builds that.  Everything else about the name is ours, so no
             * other comparison in the game can match it.
             *
             * Done AFTER the append, because the string must be rooted in a
             * managed container before anything else allocates. */
            if (s_modIconTarget == nullptr) {
                s_modIconTarget = PickIconTarget(tabBuf, 10);
            }
            if (s_modIconTarget != nullptr) {
                void* iconName = MakeIconName(s_modIconTarget);
                if (iconName != nullptr) {
                    *reinterpret_cast<void**>(nb + 0x20) = iconName;
                    s_modTabName = iconName;
                }
            }
        }
        /* Point the exit tab's right neighbour at us so the chain is closed.
         * The exit tab's own right was never written by the game (it is the
         * last one), so 0 means "none" and we are the one that replaces it.
         * Done AFTER the append succeeds: if the list had no spare capacity we
         * must not leave the exit tab pointing at a component that is not in
         * any list. */
        *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(exitTab) + 0x54) =
            0x3034 + kModTabIndex;
        s_modTab = tab;

        /* ---- v222: pages[kModTabIndex] = the game's own Options page ----
         *
         * The same object as pages[8], appended a second time.  Nothing is
         * constructed, so there is no ctor, no truncated `options` list and no
         * element of ours in it; the checkbox the user sees there comes from
         * the v211 append path, which has been in every build since v211.
         *
         * Two references to one page object are fine: the page does not know
         * its own index, GameMenu.draw only ever draws pages[currentTab], and
         * both indices resolve to the same object.
         *
         * numberOfTabs has to follow the append, and it must not lead it --
         * receiveGamePadButton uses it as the bound for
         * pages.get_Item(currentTab + 1). */
        if (kDupPage) {
            if (optPage == nullptr || !Plausible(optPage) ||
                !RangeReadable(optPage, 0xA8u)) {
                s_tabState = 24u;
                return;
            }
            if (!AppendToList(pages, optPage)) {
                s_tabState = 21u;
                return;
            }
            /* v228: remember it so the per-frame shrink can reach its `options`
             * list.  `pages` keeps it alive, so this is not a dangling root. */
            s_vanillaPage = optPage;
            s_pageReady = 1u;
            /* v317: THE FREEZE.
             *
             * GameMenu.receiveGamePadButton does
             *
             *     else if (currentTab < numberOfTabs && pages[currentTab].readyToClose())
             *         changeTab(currentTab + 1);
             *
             * and changeTab's first act is `tabs[whichTab]`.  numberOfTabs is
             * therefore "highest index you may be ON", not "how many tabs there
             * are": in vanilla it is the static 9 while `tabs` holds ten entries,
             * which is exactly what keeps the trigger from walking off the end.
             *
             * We were writing kModTabIndex + 1 (11) for eleven tabs, so standing
             * on the last tab and pressing the right trigger called
             * changeTab(11) and indexed tabs[11] -- out of range, and an
             * unhandled exception in the menu loop is a freeze.  Fast tab
             * switching is what makes it easy to hit.
             *
             * Write tabs.Count - 1 instead of assuming, so the number always
             * matches the list we actually appended to. */
            {
                int tabsCount = 0;
                void* tabsList = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(gm) + kGmTabsOff);
                if (tabsList != nullptr && Plausible(tabsList) &&
                    RangeReadable(tabsList, 0x1Cu)) {
                    tabsCount = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(tabsList) + 0x18);
                }
                int limit = kModTabIndex;
                if (tabsCount > 1 && tabsCount - 1 < limit) {
                    limit = tabsCount - 1;
                }
                *reinterpret_cast<int*>(g_base + kNumberOfTabsOff) = limit;
            }

            /* v302: THE CHECKBOX IS BUILT HERE NOW.
             *
             * It used to be created by the HUD draw path, which appended into
             * whichever options page was being drawn.  That made the element's
             * existence depend on the Options page having been drawn at least
             * once, and it left the append tangled with a per-frame guard.
             *
             * This block runs once per menu, right after `pages` roots the page
             * and before anything can shrink the list: the shrink needs
             * s_vanillaPage, which is set two lines up, so on this frame it has
             * not run yet.  The s_savedOptCount guard is belt and braces for
             * the frame where it has.
             *
             * Order is the one v223 established for the page: append FIRST so
             * the element is reachable from the managed list, THEN allocate the
             * label.  Between those two lines it is referenced only by a C++
             * local, and a collection there would take it away. */
            if (kAppendInTab && s_savedOptCount < 0 && g_appendedTo != optPage) {
                void* pageOpts = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(optPage) + 0xA0);
                if (pageOpts == nullptr || !Plausible(pageOpts) ||
                    !RangeReadable(pageOpts, 0x1Cu)) {
                    g_appendState = 3;
                }
                else {
                    /* Row labels, in display order.  The leading U+3000 on the
                     * children is what makes them read as indented -- the game
                     * draws the label verbatim and has no notion of nesting. */
                    static const char16_t* const kRowText[kRowCount] = {
                        u"\u52A8\u753B\u52A0\u901F2\u500D",      /* 动画加速2倍 */
                        u"\u5730\u56FE\u589E\u5F3A",              /* 地图增强    */
                        u"\u542F\u7528\u62D6\u62C9\u673A",        /* 启用拖拉机  */
                        u"\u3000\u9504\u5730",                    /* 　锄地      */
                        u"\u3000\u6D47\u6C34",                    /* 　浇水      */
                        u"\u3000\u65A7\u5934",                    /* 　斧头      */
                        u"\u3000\u9550\u5B50",                    /* 　镐子      */
                        u"\u3000\u5272\u8349",                    /* 　割草      */
                        u"\u3000\u6253\u602A",                    /* 　打怪      */
                        u"\u3000\u6324\u5976\u526A\u6BDB",        /* 　挤奶剪毛  */
                        u"\u3000\u64AD\u79CD\u65BD\u80A5",        /* 　播种施肥  */
                        u"\u542F\u7528\u81EA\u52A8\u5316",        /* 启用自动化  */
                    };
                    static const int kRowLen[kRowCount] =
                        {6, 4, 5, 3, 3, 3, 3, 3, 3, 5, 5, 5};
                    /* Row 0 keeps the game's own `which` (35 -> +0x24), so the
                     * animation flag keeps working exactly as it always has.
                     * Every other row gets the sentinel: the click writes
                     * nothing real, the value lives in g_savedMask, and
                     * TryPerformSave puts it in the file. */
                    static const int kRowWhich[kRowCount] = {
                        35, kOptIndex2, kOptIndex2, kOptIndex2, kOptIndex2,
                        kOptIndex2, kOptIndex2, kOptIndex2, kOptIndex2,
                        kOptIndex2, kOptIndex2, kOptIndex2,
                    };
                    if (s_rowsApplied == 0) {
                        if (s_fileMaskOk != 0) {
                            for (int i = 1; i < kRowCount; i++) {
                                g_rowOn[i] = (s_fileMask >> i) & 1u;
                            }
                            g_savedMask = kMaskTag | s_fileMask;
                            g_tractorOn = g_rowOn[kRowMaster];
                            g_mapOn = g_rowOn[kRowMap];
                        }
                        s_rowsApplied = 1;
                    }

                    {
                        void** abuf = nullptr;
                        int acap = 0;
                        int* asize = nullptr;
                        if (OptionsBacking(optPage, &abuf, &acap, &asize)) {
                            g_listCap = acap;
                        }
                    }
                    auto elemCtor = reinterpret_cast<void* (*)(
                        void*, void*, int, int, int)>(g_base + 0x17F6780);
                    const int cnt = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(pageOpts) + 0x18);
                    int made = 0;
                    for (int i = 0; i < kRowCount; i++) {
                        void* el = elemCtor(nullptr, srcName, kRowWhich[i],
                                            -1, -1);
                        if (el == nullptr || !Plausible(el) ||
                            !RangeReadable(el, 0x58u) ||
                            !AppendToList(pageOpts, el)) {
                            break;              /* no capacity: stop cleanly */
                        }
                        void* lbl = MakeFreshString(srcName, kRowText[i],
                                                    kRowLen[i]);
                        if (lbl == nullptr) {
                            /* Undo this one append and stop. */
                            *reinterpret_cast<int*>(
                                reinterpret_cast<unsigned char*>(pageOpts) +
                                0x18) = cnt + made;
                            g_appendState = 5;
                            break;
                        }
                        *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(el) + 0x20) = lbl;
                        /* Rows 1..10 are ours alone: the sentinel `which` read
                         * nothing into the box, so its state has to be written
                         * in.  This is the value that came out of the file at
                         * boot (or the seeded default), which is what makes the
                         * switches survive a restart. */
                        if (i != 0) {
                            *reinterpret_cast<unsigned char*>(
                                reinterpret_cast<unsigned char*>(el) + 0x51) =
                                static_cast<unsigned char>(
                                    g_rowOn[i] != 0 ? 1 : 0);
                        }
                        g_ourElems[i] = el;
                        made++;
                    }
                    if (made > 0) {
                        g_ourElem = g_ourElems[0];
                        g_appendedTo = optPage;
                        g_ourCount = static_cast<unsigned>(made);
                        g_appendCount = static_cast<unsigned>(cnt + made);
                        g_appendState = (made == kRowCount) ? 1u : 6u;
                    }
                    else {
                        g_appendState = 2;
                    }
                }
            }
        }

        /* ---- v217: our own OptionsPage, holding one checkbox ---- */
        if (kModPage) {
            /* Geometry straight off the live options page: that reproduces the
             * gamepad-dependent width w22 without needing to know it. */
            const unsigned char* op =
                reinterpret_cast<const unsigned char*>(optPage);
            const int px = *reinterpret_cast<const int*>(op + 0x28);
            const int py = *reinterpret_cast<const int*>(op + 0x2c);
            const int pw = *reinterpret_cast<const int*>(op + 0x30);
            const int ph = *reinterpret_cast<const int*>(op + 0x34);

            auto pageCtor =
                reinterpret_cast<void* (*)(void*, int, int, int, int)>(
                    g_base + kPageCtorOff);
            void* page = pageCtor(nullptr, px, py, pw, ph);
            if (page == nullptr || !Plausible(page) ||
                !RangeReadable(page, 0xA8u)) {
                s_tabState = 18u;
                return;
            }
            /* The ctor fills `options` with the whole vanilla option list.
             * Empty it so the page shows only ours, and reset the scroll index
             * to match -- an empty list with a non-zero currentItemIndex is how
             * you get a blank page or an out-of-range slot.  OptionsPage field
             * offsets, from meta_optionspage.json: optionSlots +0x70,
             * currentItemIndex +0x78, options +0xA0. */
            void* pageOpts = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(page) + 0xA0);
            if (pageOpts != nullptr && Plausible(pageOpts) &&
                RangeReadable(pageOpts, 0x1Cu)) {
                *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(pageOpts) + 0x18) = 0;
            }
            *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(page) + kPageCurrentItemIndexOff) = 0;
            /* v220: and clear the two statics that outlive the page.  These are
             * the documented residue (§9.2) and the best remaining explanation
             * for the freeze on takeover: the vanilla page leaves
             * _lastCurrentItemIndex at a row number that our one-entry list
             * cannot index. */
            *reinterpret_cast<int*>(g_base + kLastSelectedIndexOff) = 0;
            *reinterpret_cast<int*>(g_base + kLastCurrentItemIndexOff) = 0;
            s_modPage = page;

            /* ---- v223: HAND THE PAGE TO THE GAME BEFORE ALLOCATING AGAIN ----
             *
             * This is the bug that took five builds to find, and it is the same
             * one the strings had: a freshly created managed object that is
             * referenced ONLY by a C++ local is invisible to the collector.
             *
             * v217/v221 built the page, then built the checkbox (which
             * allocates), and only then appended the page to `pages`.  In that
             * window the page existed nowhere the GC could see.  If a
             * collection ran inside the element's constructor, the page was
             * freed -- and we then put a freed object into `pages`, so entering
             * the tab drew a dead object.  Whether that happened depended on
             * where the collector happened to be: v217 survived, v218/v219/v221
             * did not.  That is exactly the flakiness we were chasing.
             *
             * So: append first, build the contents second.  `pages` is managed
             * and reachable from the GameMenu, so from this line on the page is
             * rooted and no collection can take it away.  The page is empty for
             * a few instructions, which nobody can observe: it is not current,
             * numberOfTabs still excludes index 10, and s_pageReady is still 0,
             * so the tab still points at the vanilla page. */
            if (!AppendToList(pages, page)) {
                s_tabState = 21u;
                return;
            }

            auto elemCtor =
                reinterpret_cast<void* (*)(void*, void*, int, int, int)>(
                    g_base + 0x17F6780);
            /* Same inversion as the tab: build with the game's own string,
             * then swap ours in with no allocation in between.  OptionsElement
             * stores its label at +0x20 -- `017f7cb4: str x20,[x25,#0x20]` in
             * OptionsElement..ctor 0x17F7C00, where x20 is the label argument. */
            void* elem = elemCtor(nullptr, srcName, kOptIndex, -1, -1);
            if (elem == nullptr || !Plausible(elem) ||
                !RangeReadable(elem, 0x58u)) {
                s_tabState = 19u;
                return;
            }
            /* v224: append BEFORE the rename, same rule as the tab.  The label
             * string is an allocation, and until the element is in the page's
             * `options` list nothing the collector can see refers to it. */
            if (!AppendToList(pageOpts, elem)) {
                s_tabState = 20u;
                return;
            }
            if (s_modElemLabel == nullptr) {
                /* 动画加速2倍 */
                static const char16_t kLabel[] =
                    u"\u52A8\u753B\u52A0\u901F2\u500D";
                s_modElemLabel = MakeFreshString(srcName, kLabel, 6);
            }
            if (s_modElemLabel == nullptr) {
                *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(pageOpts) + 0x18) -= 1;
                s_tabState = 17u;
                return;
            }
            *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(elem) + 0x20) = s_modElemLabel;
            /* The page has been in `pages` since right after its constructor;
             * this is only the point where it becomes REACHABLE.  s_pageReady is
             * what HookTabNumFromName consults, and numberOfTabs is the bound
             * receiveGamePadButton checks before doing pages.get_Item(currentTab
             * + 1) -- see 0x1797a7c: `ldr w8,[0xC95E840] / cmp w1,w8 / b.ge
             * skip`.  Both are set here, after the page is complete, so a
             * failure anywhere above leaves index 10 unreachable instead of
             * pointing at a half-built page. */
            s_pageReady = 1u;
            /* Same correction as the kDupPage branch above: this is the dead
             * kModPage path, but leaving a wrong constant in it would be a trap
             * for whoever turns that switch back on. */
            *reinterpret_cast<int*>(g_base + kNumberOfTabsOff) = kModTabIndex;
        }
        /* ---- v226: the one-variable probe ----
         *
         * Everything above is v222, unchanged: the tab exists, pages[10] is the
         * game's own Options page appended a second time, numberOfTabs is 11.
         * This is the only thing v226 adds.
         *
         * The object is built and then DROPPED.  It is not stored in any
         * static, not appended to `pages`, not referenced from the tab, and
         * numberOfTabs still excludes index 10.  Nothing in the game can reach
         * it, so a collector taking it away is harmless -- which is the point:
         * this build must not be able to fail for any reason other than the
         * ctor call itself.  (Do NOT "fix" this by caching the pointer in a
         * static; that reintroduces the exact lifetime question we are trying
         * to isolate.)
         *
         * Geometry is copied from the live options page, the same way the
         * gated kModPage block does, so the ctor sees the arguments the game
         * gave it when it built that page.
         *
         * If the ctor throws, there is no return value to inspect and the
         * console hangs -- that IS the result.  If it returns, s_tabState ends
         * at 1 (or 26 for a null return) and row 1 reports it. */
        if (kModProbe) {
            if (optPage == nullptr || !Plausible(optPage) ||
                !RangeReadable(optPage, 0xA8u)) {
                s_tabState = 24u;
                return;
            }
            const unsigned char* op =
                reinterpret_cast<const unsigned char*>(optPage);
            const int px = *reinterpret_cast<const int*>(op + 0x28);
            const int py = *reinterpret_cast<const int*>(op + 0x2c);
            const int pw = *reinterpret_cast<const int*>(op + 0x30);
            const int ph = *reinterpret_cast<const int*>(op + 0x34);

            auto pageCtor =
                reinterpret_cast<void* (*)(void*, int, int, int, int)>(
                    g_base + kPageCtorOff);
            void* probe = pageCtor(nullptr, px, py, pw, ph);
            if (probe == nullptr || !Plausible(probe) ||
                !RangeReadable(probe, 0xA8u)) {
                s_tabState = 26u;
                return;
            }
            /* Deliberately nothing.  `probe` goes out of scope unreferenced. */
        }

        s_tabState = 1u;
    }

    /* v220: two guards that run BEFORE GameMenu.update does its work.
     *
     * 1. currentTab past the end of `pages`.  GameMenu.update reads
     *    currentTab and calls pages.get_Item on it, and get_Item throws when
     *    the index is out of range -- a thrown exception in the update pass
     *    is a dead main thread, which is what every freeze so far looked
     *    like.  An out-of-range currentTab is reachable: the tab index is
     *    persisted between menu opens, so a session that ended on tab 10
     *    starts the next one pointing at a page that has not been appended
     *    yet.  Clamping it to the vanilla Options tab is always a valid page.
     *
     * 2. the static scroll index.  While our page is current, keep
     *    currentItemIndex and the two OptionsPage statics at 0.  Our list has
     *    exactly one entry, so 0 is the only index that exists, and the
     *    statics are shared with the vanilla page (41 entries) -- see
     *    docs/OPTIONS_PERSIST_PLAN.md §9.2.
     *
     * Both write plain ints into memory that has already been range-checked,
     * so neither can be the reason a frame dies. */
    void GuardModPageState(void* gm) {
        if (!kModTab || gm == nullptr || !Plausible(gm) || !RangeReadable(gm, 0x90u)) {
            return;
        }
        unsigned char* g = reinterpret_cast<unsigned char*>(gm);
        int cur = *reinterpret_cast<int*>(g + kGmCurrentTabOff);
        void* pages = *reinterpret_cast<void**>(g + kGmPagesOff);
        if (pages != nullptr && Plausible(pages) && RangeReadable(pages, 0x20u)) {
            const int n = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(pages) + 0x18);
            if (cur < 0 || cur >= n) {
                /* Land on the vanilla Options tab when it exists, otherwise on
                 * the last entry that does: clamping to 8 in a list shorter
                 * than 9 would be the same out-of-range call we are trying to
                 * avoid. */
                int safe = kVanillaOptsTab;
                if (n <= safe) {
                    safe = (n > 0) ? (n - 1) : 0;
                }
                *reinterpret_cast<int*>(g + kGmCurrentTabOff) = safe;
                cur = safe;
            }
        }
        if (cur != kModTabIndex || s_modPage == nullptr) {
            return;
        }
        if (!Plausible(s_modPage) || !RangeReadable(s_modPage, 0xA8u)) {
            return;
        }
        *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(s_modPage) + kPageCurrentItemIndexOff) = 0;
        *reinterpret_cast<int*>(g_base + kLastSelectedIndexOff) = 0;
        *reinterpret_cast<int*>(g_base + kLastCurrentItemIndexOff) = 0;
    }

    /* v229: resolve a page's `options` list down to the three things the swap
     * needs -- the element array, its capacity, and the size field itself.
     *
     * This is the same walk the v211 append path already performs, with the
     * same checks in the same order, so a list whose shape we do not recognise
     * ends the swap instead of corrupting it.  `List<T>`: `_items` wrapper at
     * +0x10, the element array at wrapper+0x10, the capacity at wrapper+0x20,
     * `_size` at +0x18. */
    bool OptionsBacking(void* page, void*** bufOut, int* capOut, int** sizeOut) {
        if (page == nullptr || !Plausible(page) || !RangeReadable(page, 0xA8u)) {
            return false;
        }
        void* list = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(page) + kPageOptionsOff);
        if (list == nullptr || !Plausible(list) || !RangeReadable(list, 0x1Cu)) {
            return false;
        }
        const int cnt = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(list) + kListSizeOff);
        void* wrapper = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(list) + 0x10);
        if (cnt < 0 || cnt > 512 || wrapper == nullptr || !Plausible(wrapper) ||
            !RangeReadable(wrapper, 0x28u)) {
            return false;
        }
        void* capPtr = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(wrapper) + 0x20);
        void* arr = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(wrapper) + 0x10);
        if (capPtr == nullptr || !RangeReadable(capPtr, 4u) || arr == nullptr ||
            !Plausible(arr)) {
            return false;
        }
        const int cap = *reinterpret_cast<int*>(capPtr);
        if (cap < 0 || cap > 512 || !RangeReadable(arr, static_cast<size_t>(cap) * sizeof(void*))) {
            return false;
        }
        *bufOut = reinterpret_cast<void**>(arr);
        *capOut = cap;
        *sizeOut = reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(list) + kListSizeOff);
        return true;
    }

    /* v229: move our element into slot 0, or put the list back the way it was.
     *
     * Nothing here allocates, calls a managed method, or holds a managed
     * reference in a C++ static.  Both pointers are read out of the array and
     * written straight back into it. */
    /* v317: REBUILD THE LIST, DO NOT SHUFFLE IT.
     *
     * Every version from 305 to 316 moved rows around with pairwise swaps and
     * remembered where each one had gone.  That is a permutation the code has to
     * keep in its head, and each version found a new way to get out of step with
     * reality: a swap that failed halfway, a list that was rebuilt underneath,
     * a count that no longer described the array.
     *
     * Instead: on the way in, save the whole original order, then write the rows
     * we want into slots 0..n-1 followed by every other entry in its original
     * order, and set _size = n.  On the way out, write the saved order back.
     * Both directions are idempotent -- running either twice changes nothing --
     * so a frame that repeats, or one that gets skipped, cannot corrupt the list.
     *
     * Everything is verified BEFORE the first write: all of our elements must be
     * in this list, the list must fit in kMaxOptSlots, and after writing we check
     * that the head really is our rows.  Any doubt and the shrink is disabled for
     * this menu, leaving the page exactly as the game built it. */
    bool ListHas(void** buf, int n, void* want) {
        if (want == nullptr) {
            return false;
        }
        for (int i = 0; i < n; i++) {
            if (buf[i] == want) {
                return true;
            }
        }
        return false;
    }

    /* Rows to show, in display order.  Returns how many. */
    int RowOrder(int* order) {
        int n = 0;
        order[n++] = 0;                       /* animation */
        order[n++] = kRowMap;                 /* independent of the master */
        order[n++] = kRowMaster;
        if (g_rowOn[kRowMaster] != 0) {
            for (int i = kRowFirstSub; i <= kRowLastSub; i++) {
                order[n++] = i;
            }
        }
        /* v377: THIS is why the 12th row never appeared.  ShrinkListToRows ends
         * with `*size = n` -- it keeps exactly the rows listed here and drops
         * everything else.  The new row was appended to the list correctly (the
         * readout proved that: 12 rows, capacity grown), and then the shrink set
         * the size back to 11, cutting it off.  Every new row must be listed. */
        order[n++] = kRowAutomate;
        return n;
    }

    int ShrinkListToRows(void* page) {
        void** buf = nullptr;
        int cap = 0;
        int* size = nullptr;
        if (!OptionsBacking(page, &buf, &cap, &size)) {
            return -1;
        }
        if (g_ourCount == 0 || g_appendedTo != page) {
            return -1;
        }
        if (cap < 1 || cap > kMaxOptSlots) {
            return -1;
        }
        /* v327: MEASURE AGAINST THE LIST WE FIRST SAW, NOT AGAINST `_size`.
         *
         * After a collapse `_size` is 3, but the array still holds all 52
         * entries -- the eight children are parked past `_size`.  Scanning only
         * `_size` entries therefore "loses" them, the rebuild is refused, and
         * expanding does nothing until the menu is reopened.  s_origCount is the
         * length of the list as the game built it, which is the array we are
         * actually reading.
         *
         * Only when nothing has been saved yet is `_size` the right answer. */
        int total = *size;
        if (s_origCount > 0) {
            total = s_origCount;
        }
        if (total < 1 || total > cap) {
            return -1;
        }
        for (unsigned i = 0; i < g_ourCount && i < kRowCount; i++) {
            if (!ListHas(buf, total, g_ourElems[i])) {
                return -1;                    /* not this list any more */
            }
        }
        int order[kRowCount];
        const int n = RowOrder(order);
        if (n <= 0 || n > cap || n > kRowCount) {
            return -1;
        }
        if (s_origCount == 0) {
            for (int i = 0; i < total; i++) {
                s_origOrder[i] = buf[i];
            }
            s_origCount = total;
            s_savedOptCount = total;
        }
        void* next[kMaxOptSlots];
        int w = 0;
        for (int i = 0; i < n; i++) {
            next[w++] = g_ourElems[order[i]];
        }
        for (int i = 0; i < s_origCount; i++) {
            void* e = s_origOrder[i];
            bool ours = false;
            for (int j = 0; j < n; j++) {
                if (e == g_ourElems[order[j]]) {
                    ours = true;
                    break;
                }
            }
            if (!ours) {
                next[w++] = e;
            }
        }
        if (w > cap) {
            s_origCount = 0;
            return -1;
        }
        for (int i = 0; i < w; i++) {
            buf[i] = next[i];
        }
        *size = n;
        /* self-check: the head has to be exactly our rows, in order */
        for (int i = 0; i < n; i++) {
            if (buf[i] != g_ourElems[order[i]]) {
                for (int k = 0; k < s_origCount; k++) {
                    buf[k] = s_origOrder[k];
                }
                *size = s_origCount;
                s_origCount = 0;
                return -1;
            }
        }
        return n;
    }

    void RestoreList(void* page) {
        if (s_origCount == 0) {
            return;
        }
        void** buf = nullptr;
        int cap = 0;
        int* size = nullptr;
        const int total = s_origCount;
        s_origCount = 0;
        s_savedOptCount = -1;
        if (!OptionsBacking(page, &buf, &cap, &size)) {
            return;
        }
        if (total > cap) {
            return;
        }
        for (int i = 0; i < total; i++) {
            buf[i] = s_origOrder[i];
        }
        *size = total;
    }

    /* v228: make the game's own Options page look like a one-entry page while
     * the Mod tab is current, and put it back the moment the tab is left.
     *
     * Everything here is a plain integer read or write on memory that has just
     * been range-checked.  No allocation, no managed call, no new object -- so
     * this function cannot throw, and the only way it can break the frame is
     * by writing a wrong value, which is why every step re-checks its own
     * preconditions and gives up silently instead of guessing.
     *
     * Ordering matters: this runs BEFORE the original update, so the shrink is
     * in place before anything reads `options`, and the restore happens on the
     * first frame after the tab changes, i.e. before that frame draws the
     * Options tab.  If the menu is closed while shrunk, the page dies with it
     * and the re-arm block forgets the saved count.
     *
     * v229 adds one step to each side: before the shrink, our element is
     * swapped into slot 0; before the restore, it is swapped back out.  See
     * kOurElemOnMod. */
    /* v316: is this page still one of THIS menu's pages?
     *
     * Fast menu switching frees the old menu's pages, and Plausible() only
     * proves the address is mapped -- the allocator hands the same block back
     * for the next menu.  Writing `_size` into a recycled block is heap
     * corruption, and heap corruption in the menu loop is exactly a freeze.
     * So walk `pages` and look for the pointer before touching anything. */
    /* v318: WHERE IS OUR PAGE IN `pages`?
     *
     * v317 asked "does index 10 exist" before letting the tab resolve there.
     * That was the wrong question twice over: it does not prove the entry is
     * OURS, and it turns any hiccup into "the tab silently opens Options".
     * Search for the page itself and answer with its real index -- changeTab
     * uses the return value to index `pages`, so that index is the only thing
     * that has to be right.
     *
     * Returns -1 when the page is not in this menu's list. */
    int PagesIndexOf(void* gm, void* page) {
        if (gm == nullptr || page == nullptr || !Plausible(gm) ||
            !RangeReadable(gm, 0x90u)) {
            return -1;
        }
        void* pages = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(gm) + kGmPagesOff);
        if (pages == nullptr || !Plausible(pages) ||
            !RangeReadable(pages, 0x1Cu)) {
            return -1;
        }
        void* wrapper = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(pages) + 0x10);
        if (wrapper == nullptr || !Plausible(wrapper) ||
            !RangeReadable(wrapper, 0x28u)) {
            return -1;
        }
        void** buf = *reinterpret_cast<void***>(
            reinterpret_cast<unsigned char*>(wrapper) + 0x10);
        const int n = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(pages) + 0x18);
        if (buf == nullptr || !Plausible(buf) || n <= 0 || n > 64) {
            return -1;
        }
        /* v319: LAST match, not first.
         *
         * The page we append is the game's OWN Options page -- the same object
         * that already sits at index 8.  Searching forwards therefore found the
         * original slot and the tab resolved back to the vanilla Options page,
         * which is exactly the "Mod tab shows Options" symptom.  Our copy is
         * always appended after it, so the last match is the one the tab has to
         * use. */
        for (int i = n - 1; i >= 0; i--) {
            if (buf[i] == page) {
                return i;
            }
        }
        return -1;
    }

    /* Kept for the shrink, which only needs "is it still in there". */
    bool PageIsLiveIn(void* gm, void* page) {
        if (gm == nullptr || page == nullptr || !Plausible(gm) ||
            !RangeReadable(gm, 0x90u)) {
            return false;
        }
        void* pages = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(gm) + kGmPagesOff);
        if (pages == nullptr || !Plausible(pages) ||
            !RangeReadable(pages, 0x1Cu)) {
            return false;
        }
        void* wrapper = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(pages) + 0x10);
        if (wrapper == nullptr || !Plausible(wrapper) ||
            !RangeReadable(wrapper, 0x28u)) {
            return false;
        }
        void** buf = *reinterpret_cast<void***>(
            reinterpret_cast<unsigned char*>(wrapper) + 0x10);
        const int n = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(pages) + 0x18);
        if (buf == nullptr || !Plausible(buf) || n <= 0 || n > 64) {
            return false;
        }
        for (int i = 0; i < n; i++) {
            if (buf[i] == page) {
                return true;
            }
        }
        return false;
    }

    void SyncModPageOptions(void* gm) {
        if (!kModTab || !kTruncOnMod || gm == nullptr ||
            s_vanillaPage == nullptr) {
            return;
        }
        if (!Plausible(gm) || !RangeReadable(gm, 0x90u) ||
            !Plausible(s_vanillaPage) || !RangeReadable(s_vanillaPage, 0xA8u)) {
            return;
        }
        /* v316: it has to still belong to THIS menu.  Plausible() only proves
         * the address is readable, and the allocator hands a freed page's block
         * straight back to the next menu. */
        if (!PageIsLiveIn(gm, s_vanillaPage)) {
            return;
        }
        const int cur = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(gm) + kGmCurrentTabOff);
        unsigned char* page = reinterpret_cast<unsigned char*>(s_vanillaPage);

        if (cur == kModTabIndex) {
            /* v317: build the shrunk list ONCE per visit.  Rebuilding every
             * frame was pure churn -- the list cannot change while the tab is
             * open, and the one append that could have changed it (v211) is
             * disabled.  A single deterministic write also means the state can
             * only be one of two things, which is what makes it safe. */
            /* v320: RETRY, do not latch a failure.
             *
             * This runs BEFORE TryBuildModTab on the frame the menu opens, and
             * the game reopens on the tab that was selected last -- so the very
             * first frame can arrive with cur == 10 and g_ourCount still 0,
             * i.e. before our rows exist.  Latching "failed" there left the page
             * showing the whole vanilla list for the rest of the session, which
             * is exactly what "the Mod tab is not ours" looked like.
             *
             * The call is cheap and idempotent, so just try again every frame
             * until it takes. */
            /* v326: rebuild whenever the row set changes, not once per visit.
             *
             * 317 made this a one-shot to reduce churn, which also removed
             * collapse/expand: toggling the master changes the row count from
             * 11 to 3, and nothing rebuilt the list for the new count.  Compare
             * the wanted count instead, and rebuild only when it differs --
             * ShrinkListToRows is idempotent and re-uses the saved original
             * order, so repeating it for the same count would be wasted work,
             * not a bug. */
            {
                int order[kRowCount];
                const int want = RowOrder(order);
                if (s_shrinkState == 0 || want != s_shrinkCount) {
                    if (ShrinkListToRows(s_vanillaPage) > 0) {
                        s_shrinkState = 1u;
                        s_shrinkCount = want;
                    }
                }
            }
            if (s_shrinkState == 1) {
                /* Keep the page's own scroll index inside what the list can
                 * show.  OptionsPage.snapCursorToCurrentSnappedComponent does
                 * `options[myID + currentItemIndex]` and only bounds myID, so
                 * an index left over from the Options tab would be out of range
                 * here and the menu loop would throw. */
                void* opts = *reinterpret_cast<void**>(page + kPageOptionsOff);
                if (opts != nullptr && Plausible(opts) &&
                    RangeReadable(opts, 0x1Cu)) {
                    const int cnt = *reinterpret_cast<int*>(
                        reinterpret_cast<unsigned char*>(opts) + kListSizeOff);
                    const int maxIdx = (cnt > 7) ? (cnt - 7) : 0;
                    int* idx = reinterpret_cast<int*>(
                        page + kPageCurrentItemIndexOff);
                    int v = *idx;
                    if (v < 0) {
                        v = 0;
                    }
                    if (v > maxIdx) {
                        v = maxIdx;
                    }
                    *idx = v;
                }
                *reinterpret_cast<int*>(g_base + kLastSelectedIndexOff) = 0;
                *reinterpret_cast<int*>(g_base + kLastCurrentItemIndexOff) = 0;
            }
            return;
        }

        if (s_shrinkState != 0) {
            RestoreList(s_vanillaPage);
            s_shrinkState = 0;
            *reinterpret_cast<int*>(page + kPageCurrentItemIndexOff) = 0;
        }
    }

    void HookGameMenuUpdate(void* self, void* time) {
        /* v318 diagnostics: one cheap read per menu frame. */
        g_dbgPages = -1;
        g_dbgPageIdx = -2;
        if (self != nullptr && Plausible(self) && RangeReadable(self, 0x90u)) {
            void* pages = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(self) + kGmPagesOff);
            if (pages != nullptr && Plausible(pages) &&
                RangeReadable(pages, 0x1Cu)) {
                g_dbgPages = *reinterpret_cast<int*>(
                    reinterpret_cast<unsigned char*>(pages) + 0x18);
            }
            g_dbgPageIdx = PagesIndexOf(self, s_vanillaPage);
            g_dbgCur = *reinterpret_cast<int*>(
                reinterpret_cast<unsigned char*>(self) + kGmCurrentTabOff);
            g_dbgNameMatch = -1;
            if (s_modTab != nullptr && Plausible(s_modTab) &&
                RangeReadable(s_modTab, 0x28u)) {
                void* tn = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(s_modTab) + 0x20);
                g_dbgNameMatch = (tn != nullptr && tn == s_modTabName) ? 1 : 0;
            }
            g_dbgTabIdx = -1;
            if (s_modTab != nullptr) {
                void* tabs = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(self) + kGmTabsOff);
                if (tabs != nullptr && Plausible(tabs) &&
                    RangeReadable(tabs, 0x1Cu)) {
                    void* tw = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(tabs) + 0x10);
                    if (tw != nullptr && Plausible(tw) &&
                        RangeReadable(tw, 0x28u)) {
                        void** tb = *reinterpret_cast<void***>(
                            reinterpret_cast<unsigned char*>(tw) + 0x10);
                        const int tn = *reinterpret_cast<int*>(
                            reinterpret_cast<unsigned char*>(tabs) + 0x18);
                        if (tb != nullptr && Plausible(tb) && tn > 0 && tn <= 64) {
                            for (int i = 0; i < tn; i++) {
                                if (tb[i] == s_modTab) {
                                    g_dbgTabIdx = i;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        SyncModPageOptions(self);
        if (g_dbgCur == kModTabIndex) {
            g_sawModShrink = static_cast<int>(s_shrinkState);
            g_sawModCount = static_cast<int>(g_ourCount);
            g_sawAppend = static_cast<int>(g_appendState);
        }
        GuardModPageState(self);
        if (s_origGameMenuUpdate != nullptr) {
            s_origGameMenuUpdate(self, time);
        }
        TryBuildModTab(self);
    }

    /* Both hooks are pointer-identity tests against the strings we made.  That
     * is why the name only has to be *a* unique managed string -- it never has
     * to be compared by content. */
    int HookTabNumFromName(void* self, void* name) {
        /* v231: our tab carries a BORROWED name (a copy of one of the game's
         * own tab names) so the icon lookup finds an icon for it.  The game's
         * own getTabNumberFromName dispatches by string IDENTITY, so it would
         * not recognise the copy -- we have to answer for it ourselves, or
         * the draw code would think our tab has no number and would draw its
         * icon in the wrong place. */
        /* v322: IDENTIFY OUR TAB BY THE POINTER ON THE COMPONENT ITSELF.
         *
         * Up to 321 this compared `name` against s_modTabName, a static that
         * the build path overwrites (the icon name replaces the "Mod" label).
         * Any path that leaves those two out of step makes the comparison miss,
         * and then the game's own switch(name) decides what our tab is -- and
         * our name is deliberately built to look like the crafting tab's, so
         * the answer is craftingTab.  Reading the name off the component that
         * is actually in `tabs` cannot drift. */
        void* ourTabName = nullptr;
        if (s_modTab != nullptr && Plausible(s_modTab) &&
            RangeReadable(s_modTab, 0x28u)) {
            ourTabName = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(s_modTab) + 0x20);
        }
        const bool isOurs = (name != nullptr) &&
            ((ourTabName != nullptr && name == ourTabName) ||
             (s_modTabName != nullptr && name == s_modTabName));
        if (kModTab && isOurs) {
            /* v217: kModTabIndex only once the page is actually in `pages`.
             * Until then the tab opens the vanilla Options page (8), which
             * still carries our checkbox -- so every failure path above leaves
             * the player with a working menu and a reachable checkbox.
             *
             * v227: the gate is now kTabToTen, not kModPage.  changeTab derives
             * currentTab from THIS return value, so while kModPage was the gate
             * and it was false, the tab never resolved to 10 at all -- the
             * game quietly redirected to the Options page.  That is why every
             * "working" build also never tested index 10.  kTabToTen lets the
             * tab resolve to 10 while pages[10] is still the game's own page,
             * so the two questions are finally separate. */
            /* v325: BACK TO THE CODE THAT WORKED.
             *
             * v311 shipped exactly this line and the Mod tab opened our page:
             * answer 10, and only once the page has actually been appended.
             * Everything I added between 317 and 324 -- an index-exists test, a
             * pointer search, an identity check against the component's name --
             * was meant to make this safer and each one instead turned a working
             * tab into one that opened the Options page.  The only change that
             * path actually needed was numberOfTabs, and that is fixed
             * separately.
             *
             * So: same answer, same gate.  The one thing kept from 322 is the
             * wider identity test (component name OR s_modTabName), because
             * matching on two pointers can only recognise more of our own tab,
             * never fewer. */
            return (kTabToTen && s_pageReady != 0u) ? kModTabIndex
                                                    : kVanillaOptsTab;
        }
        if (s_origTabNumFromName != nullptr) {
            return s_origTabNumFromName(self, name);
        }
        return -1;
    }

    void* HookTabLabelFromIdx(int index) {
        /* v232: the LABEL, not the name.  Since the icon work the two are
         * different strings: the name is built to satisfy the icon lookup and
         * would read as gibberish if it were ever drawn. */
        if (kModTab && index == kModTabIndex && s_modTabLabel != nullptr) {
            return s_modTabLabel;
        }
        if (s_origTabLabelFromIdx != nullptr) {
            return s_origTabLabelFromIdx(index);
        }
        return nullptr;
    }

    /* v232: make the icon lookup match our tab without giving it a game name.
     *
     * GameMenu.draw compares each tab's name against the game's own tab names
     * with String.Equals.  v231 copied a game name so that comparison would
     * succeed by content -- and a second tab with an existing name froze the
     * game on click.  This time our name matches nothing; we only lie about
     * ONE pair: our name against the object we want the icon from.
     *
     * The hook is deliberately the cheapest possible: two pointer compares,
     * then the real function.  Nothing is allocated, nothing managed is
     * called, and a stale s_modTabName / s_modIconTarget can only ever make
     * the compare fail -- never corrupt anything. */
    /* v237: patch one immediate field in an instruction.
     *
     * The ORIGINAL word has to match before anything is written: on a different
     * game build the offsets point at unrelated code, and writing there would
     * corrupt it.  A mismatch is not an error, it just means "this site is not
     * what we think it is", and the caller then refuses to grow the inventory. */
    static bool s_bagPatched = false;
    static int s_bagGrown = 0;          /* 0 = not yet, 1 = done, -1 = gave up */

    bool PatchInstr(uintptr_t off, u32 expected, u32 newImm, int shift, u32 mask) {
        /* NO Plausible()/RangeReadable() here: both are written for MANAGED
         * pointers, and Plausible() additionally demands 8-byte alignment --
         * five of the six sites are 4-mod-8, so it rejected them all and the
         * inventory never grew (v237 shipped exactly that bug).  The address is
         * a compile-time constant inside the module and the real guard is the
         * expected-word comparison below. */
        if (g_base == 0) {
            return false;
        }
        u32* ro = reinterpret_cast<u32*>(g_base + off);
        exl::util::RwPages ctrl(reinterpret_cast<uintptr_t>(ro), 4u);
        u32* rw = reinterpret_cast<u32*>(ctrl.GetRw());
        if (rw == nullptr) {
            return false;
        }
        const u32 old = *rw;
        if (old != expected) {
            return false;
        }
        *rw = (old & ~(mask << shift)) | ((newImm & mask) << shift);
        ctrl.Flush();
        return true;
    }

    /* v239: replace a whole instruction word (when the new one is not just a
     * different immediate in the same encoding). */
    bool PatchWord(uintptr_t off, u32 expected, u32 newWord) {
        if (g_base == 0) {
            return false;
        }
        u32* ro = reinterpret_cast<u32*>(g_base + off);
        exl::util::RwPages ctrl(reinterpret_cast<uintptr_t>(ro), 4u);
        u32* rw = reinterpret_cast<u32*>(ctrl.GetRw());
        if (rw == nullptr) {
            return false;
        }
        if (*rw != expected) {
            return false;
        }
        *rw = newWord;
        ctrl.Flush();
        return true;
    }

    /* Every site, or none of them.  If any site refuses, the inventory must
     * NOT be grown: 48 slots that the page cannot lay out would leave items in
     * slots 37-48 invisible while still being saved.
     *
     * v245 -- rebuilt from the game's own source instead of from guesswork.
     *
     * Ground truth is the decompiled PC 1.6.15.24354 assembly
     * (StardewValley.Menus.InventoryPage / InventoryMenu) plus the PC mod this
     * feature copies, Nexus 1845 "Bigger Backpack"
     * (.tmp/bb/BiggerBackpack.decompiled.cs).  That mod changes exactly four
     * things:
     *
     *   1. InventoryMenu ctor : capacity 36 -> 48, rows 3 -> 4
     *   2. InventoryPage ctor : height += 64          (the brown box grows)
     *   3. InventoryPage ctor : every equipment icon's bounds.Y += 64
     *   4. InventoryPage.draw : the shared base
     *        `yPositionOnScreen + borderWidth + spaceToClearTopBorder`
     *      is raised by 64, which lifts all nine offsets built on it
     *
     * Point 4 is why v241 and v244 kept missing.  The divider, the portrait,
     * the portrait's background and the five info texts do not each own a
     * "layout offset" -- they share one base and add a hardcoded constant on
     * top.  Patching constants one at a time missed three of them (player
     * name, farm name, funds), and the three patches that went into
     * MenuWithInventory.draw 0x17EFB10 were dead code: InventoryPage.draw is
     * an override that never calls base.draw().
     */
    /* v280 -- Utility.FilterDirtyWords, see ApplyNamingFixPatch(). */
    constexpr uintptr_t kFilterDirtyOff     = SDV_OFF(kFilterDirtyOff, 0x1BA9EC0u);

    /* v280 -- THE NAMING FREEZE.
     *
     * Proven by experiment, not by reasoning: with every bit of our code and
     * the romfs disabled and only main.npdm left in place, naming a horse still
     * froze; rename main.npdm and it works.  So the process descriptor is the
     * cause, and the missing service is "ngct" -- the console's word-filtering
     * service, which the npdm we ship does not grant.
     *
     * NamingMenu.textBoxEnter (NamingMenu.cs:91, FilterInput defaults to true)
     * calls Utility.FilterDirtyWords, which is a one-line forwarder:
     *
     *     public static string FilterDirtyWords(string words) {
     *         return Program.sdk.FilterDirtyWords(words);
     *     }
     *
     * Program.sdk.FilterDirtyWords is the ngct caller, and with the service
     * denied it never returns -- which is exactly what the user saw: the menu
     * stays on screen, forever.
     *
     * Fix: turn the forwarder into `return words;` by patching its first
     * instruction to `ret`.  x0 already holds the argument at entry, so a bare
     * ret returns the input string untouched and the SDK is never reached.
     *
     * A code patch rather than a hook, deliberately: the prologue is
     * `stp x29, x30, [sp, #-48]!`, a pre-indexed SP write, and that is the
     * shape that froze the console when GameMenu.draw was hooked.
     *
     * Consequence: horse names are no longer profanity-filtered on this build.
     * That is the trade -- the alternative is a menu that hangs.  Nothing else
     * calls this method (grep: NamingMenu is the only caller).
     *
     * Why not fix the npdm instead: v279 tried that (reference npdm + our
     * capability list) and the game would not start at all.  The npdm that
     * boots is the one we already have; leave it alone. */
    void ApplyNamingFixPatch() {
        /* a9bd7bfd  stp x29, x30, [sp, #-48]!   ->   d65f03c0  ret */
        const bool ok = PatchWord(kFilterDirtyOff, 0xA9BD7BFDu, 0xD65F03C0u);
        LOGF("naming: FilterDirtyWords bypass %s", ok ? "applied" : "REFUSED");
    }

    void ApplyBiggerBackpackPatches() {
        if (!kBiggerBackpack) {
            return;
        }
        bool ok = true;

        /* ---- 1. the grid: 48 slots, 4 rows of 12 ---------------------- */
        /* `capacity == -1 ? 36 : capacity`, and the slot-building loop bound. */
        ok = PatchInstr(0x17A7DF4u, 0x52800488u, 0x30u, 5, 0xFFFFu) && ok;
        ok = PatchInstr(0x17A8114u, 0x7100901Fu, 0x30u, 10, 0xFFFu) && ok;
        /* The rows argument (arg 8, w7).  The column count is derived as
         * capacity / rows, so leaving rows at 3 with 48 slots gives 3x16.
         * InventoryMenu.height = 64 * rows + 16 comes from that same register
         * and becomes 272 on its own -- v242's extra patch on 0x17A7E04 is
         * gone.  That field is not drawn; the brown box is the PAGE height. */
        ok = PatchWord(0x17A7D20u, 0x2A0703F7u, 0x52800097u) && ok;

        /* ---- 2. the brown box: page height +64 ------------------------ */
        /* GameMenu.draw does
         *     Game1.drawDialogueBox(x, y, pages[currentTab].width,
         *                           pages[currentTab].height, ...)
         * so the box IS the page height.  InventoryPage's ctor keeps the
         * incoming height in w20 (`0x17AD3E4 mov w20, w4`) and uses it twice:
         * for base(IClickableMenu) and for the organize button's
         * `y + height / 3 - 64 + 8`.  Adding here covers both.
         *
         * v245 used +32 and the hardware run showed the lowest text line
         * ("test", the player name) sitting 12px above the bottom border.
         * That is exactly what the arithmetic predicts: the name text is at
         * `base + 456`, so its margin to the border is
         *     (680 - 136 - 456 - 44) + growth - shift = 44 + growth - 64
         * -- 12px at +32.  Vanilla has 44px, so the growth has to be 64, which
         * is also what the PC mod uses.
         *
         * Is there room?  The box starts at `viewportHeight / 2 - 340`.  The
         * v245 screenshot shows the bottom border fully on screen with the box
         * 712 tall, i.e. `vh/2 - 340 + 712 <= vh`, so vh >= 744 -- 720p is
         * ruled out, and at 1080p the box ends at 944 with 136px to spare. */
        ok = PatchWord(0x17AD3E4u, 0x2A0403F4u, 0x11010094u) && ok;
        /* CraftingPage needs the same treatment, and it is the same one-word
         * patch: its ctor also parks the height argument in w20
         * (`0x176DECC mov w20, w4`, ctor at 0x176DEA0), then hands it to
         * base(IClickableMenu) at 0x176DF64 and reuses it at 0x176E070 for the
         * trash can's `y + height - borderWidth - 328`.
         *
         * Its inventory sits at `base + 304` and is 4 rows now, so the grid
         * ends at `base + 568` while the box bottom was `base + 544` -- the
         * whole fourth row hung outside, which is what the screenshot shows.
         * +64 puts the bottom at `base + 608`, a 40px margin. */
        ok = PatchWord(0x176DECCu, 0x2A0403F4u, 0x11010094u) && ok;

        /* ---- 3. components below the grid: +64 ------------------------ */
        /* Equipment icons: two columns at x+48 and x+256, Y 248 / 312 / 376.
         * These are `add w2, w8, #imm` -- w2 is a Rectangle's Y.  The X sites
         * are `add w1` and must NOT be touched (v239 did, and shoved three
         * icons sideways on top of the grid).  Then the portrait component
         * (y+312), the trash can (y+256) and the trinket loop's base. */
        ok = PatchInstr(0x17AD568u, 0x1103E102u, 0x138u, 10, 0xFFFu) && ok;  /* left ring   248->312 */
        ok = PatchInstr(0x17AD5ECu, 0x1104E102u, 0x178u, 10, 0xFFFu) && ok;  /* right ring  312->376 */
        ok = PatchInstr(0x17AD664u, 0x1105E102u, 0x1B8u, 10, 0xFFFu) && ok;  /* boots       376->440 */
        ok = PatchInstr(0x17AD6CCu, 0x1104E102u, 0x178u, 10, 0xFFFu) && ok;  /* portrait    312->376 */
        ok = PatchInstr(0x17AD730u, 0x11040102u, 0x140u, 10, 0xFFFu) && ok;  /* trash can   256->320 */
        ok = PatchInstr(0x17AD8C4u, 0x1103E102u, 0x138u, 10, 0xFFFu) && ok;  /* hat         248->312 */
        ok = PatchInstr(0x17AD94Cu, 0x1104E102u, 0x178u, 10, 0xFFFu) && ok;  /* shirt       312->376 */
        ok = PatchInstr(0x17AD9D0u, 0x1105E102u, 0x1B8u, 10, 0xFFFu) && ok;  /* pants       376->440 */
        ok = PatchWord(0x17ADA64u, 0x52801F1Bu, 0x5280271Bu) && ok;          /* trinket     248->312 */

        /* ---- 4. InventoryPage.draw: all nine Y offsets, +64 ----------- */
        /* Every one of these blocks reads
         *     ldr  w8, [x20, #0x2c]   ; yPositionOnScreen
         *     add  w8, w8, w9         ; + borderWidth
         *     add  w8, w8, w10        ; + spaceToClearTopBorder
         *     add  w8, w8, #<K>       ; <- the constant patched below
         * so each K simply grows by one row.  The X coordinates use
         * [x20, #0x28] = xPositionOnScreen and stay untouched
         * (0x17B1DEC, 0x17B1F1C, 0x17B2130). */
        ok = PatchInstr(0x17B1504u, 0x11030102u, 0x100u, 10, 0xFFFu) && ok;  /* divider         192->256 */
        ok = PatchInstr(0x17B1E10u, 0x1103E108u, 0x138u, 10, 0xFFFu) && ok;  /* portrait bg     248->312 */
        ok = PatchInstr(0x17B1F40u, 0x11046108u, 0x158u, 10, 0xFFFu) && ok;  /* portrait        280->344 */
        ok = PatchInstr(0x17B2084u, 0x11046108u, 0x158u, 10, 0xFFFu) && ok;  /* night portrait  280->344 */
        ok = PatchInstr(0x17B2184u, 0x11072108u, 0x208u, 10, 0xFFFu) && ok;  /* player name     456->520 */
        ok = PatchInstr(0x17B2264u, 0x11041108u, 0x144u, 10, 0xFFFu) && ok;  /* farm name       260->324 */
        ok = PatchInstr(0x17B2354u, 0x11051108u, 0x184u, 10, 0xFFFu) && ok;  /* funds           324->388 */
        ok = PatchInstr(0x17B2444u, 0x11060108u, 0x1C0u, 10, 0xFFFu) && ok;  /* total earnings  384->448 */
        ok = PatchInstr(0x17B24D8u, 0x11070108u, 0x200u, 10, 0xFFFu) && ok;  /* date            448->512 */

        s_bagPatched = ok;
        LOGF("bag: patches applied=%d", ok ? 1 : 0);
    }

    /* v247: make the 48th slot something you BUY at Pierre's, like vanilla.
     *
     * The vanilla flow is already in the game; it just stops at 36:
     *
     *   GameLocation.performAction  case "BuyBackpack"
     *       maxItems == 12 -> "buy the large backpack"      (2000g, 12 -> 24)
     *       maxItems <  36 -> "buy the deluxe backpack"     (10000g, 24 -> 36)
     *       (at 36: nothing happens at all)
     *
     *   GameLocation.answerDialogueAction  case "Backpack_Purchase"
     *       maxItems == 12 && money >= 2000   -> increaseBackpackSize(12)
     *       maxItems <  36 && money >= 10000  -> maxItems += 12 + pad the list
     *       maxItems != 36                    -> "not enough money"
     *
     *   SeedShop.draw
     *       maxItems == 12  -> small backpack sprite
     *       maxItems <  36  -> deluxe backpack sprite
     *       else            -> paint the empty shelf over it
     *
     * So three of the four gates are a single comparison against 36.  Raise
     * each to 48 and the third upgrade becomes buyable, using the game's own
     * money handling, item-list padding, hold-up animation and chat message.
     * No new managed code, no new hook.
     *
     * Note how the compiler encoded `maxItems < 36`: as `cmp #0x23` + `b.gt`
     * (i.e. "<= 35"), not `cmp #0x24` + `b.ge`.  That is why grepping for
     * `cmp #36` only ever found the `!= 36` site.
     *
     * Site identification:
     *   0x16C2BC8  SeedShop.draw -- right before it builds
     *              Rectangle(452, 1184, 112, 20), the empty-shelf blit.
     *   0x150D75C  performAction -- on the "< 36" side it builds a 2-element
     *              Response[] and calls createQuestionDialogue.
     *   0x151ABE4  answerDialogueAction -- the "< 36 && money >= 10000" gate
     *              (the 10000 is loaded at 0x151ABF4).
     *   0x151AC0C  answerDialogueAction -- the trailing "maxItems != 36"
     *              that makes an unaffordable purchase silent instead of
     *              showing "not enough money".
     */
    static bool s_shopPatched = false;

    void ApplyBackpackShopPatches() {
        if (!kBiggerBackpack) {
            return;
        }
        bool ok = true;
        ok = PatchInstr(0x16C2BC8u, 0x71008D1Fu, 0x2Fu, 10, 0xFFFu) && ok;
        ok = PatchInstr(0x150D75Cu, 0x71008D1Fu, 0x2Fu, 10, 0xFFFu) && ok;
        ok = PatchInstr(0x151ABE4u, 0x71008D1Fu, 0x2Fu, 10, 0xFFFu) && ok;
        ok = PatchInstr(0x151AC0Cu, 0x7100911Fu, 0x30u, 10, 0xFFFu) && ok;
        s_shopPatched = ok;
        LOGF("bag: shop patches applied=%d", ok ? 1 : 0);
    }

    /* Ask the game to grow the inventory to 48, once, using its own routine.
     *
     * v247: only used as a FALLBACK.  If the shop patches above installed, the
     * player is meant to buy the upgrade and nothing is granted for free.  If
     * they did not install (a different game build), the old behaviour comes
     * back so the 48 slots are still reachable instead of being dead forever. */
    void MaybeGrowBackpack() {
        if (!kBiggerBackpack || s_bagGrown != 0 || s_shopPatched) {
            return;
        }
        if (!s_bagPatched) {
            s_bagGrown = -1;            /* layout not patched: do not grow */
            return;
        }
        auto getPlayer = reinterpret_cast<void* (*)(void*)>(g_base + kGetPlayerOff);
        void* pl = getPlayer(nullptr);
        if (pl == nullptr || !Plausible(pl) || !RangeReadable(pl, kMaxItemsOff + 0x10u)) {
            return;                     /* player not loaded yet: retry next frame */
        }
        void* ref = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(pl) + kMaxItemsOff);
        if (ref == nullptr || !Plausible(ref) ||
            !RangeReadable(reinterpret_cast<unsigned char*>(ref) + 0x6Cu, 4u)) {
            return;
        }
        const int cur = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(ref) + 0x68);
        if (cur >= kBigMaxItems) {
            s_bagGrown = 1;             /* already big (a previous run saved it) */
            return;
        }
        if (cur != kVanillaMaxItems) {
            return;                     /* only grow from the vanilla size */
        }
        auto grow = reinterpret_cast<void (*)(void*, int)>(g_base + kGrowInventoryOff);
        grow(pl, kBigMaxItems - cur);
        s_bagGrown = 1;
        LOGF("bag: grew inventory %d -> %d", cur, kBigMaxItems);
    }

    /* ================= v328: the 48-slot tier buys on its own ==========
     *
     * v247 made the third upgrade buyable by raising the three "maxItems < 36"
     * gates to 48.  That reuses the SECOND tier wholesale, so the third
     * purchase inherited the second one's price (10000) and its wording
     * ("背包升级：36格" / "购买（10,000金）").  Both are now their own.
     *
     * PRICE -- GameLocation.answerDialogueAction 0x15182D0
     *   The third tier has its own branch.  When maxItems == 36 and the answer
     *   is "Backpack_Purchase" we take over completely:
     *       money >= 20000 -> money -= 20000
     *                         Farmer.increaseBackpackSize(12)   (0x13C3E50)
     *                         return true  -- vanilla never runs
     *       money <  20000 -> run vanilla against a zero balance so its own
     *                         ">= 10000" test cannot pass, then restore the
     *                         balance.  That is only to reuse its
     *                         NotEnoughMoney2 dialogue; nothing is bought.
     *
     *   increaseBackpackSize is the game's own routine and it does the two
     *   things that are easy to get wrong:
     *       maxItems.Value += 12
     *       while (Items.Count < maxItems) Items.Add(null)
     *   so both the slot count and the item list are right.  It is the same
     *   call MaybeGrowBackpack() uses.
     *
     *   KNOWN GAP: the vanilla branch also plays the hold-up animation and
     *   posts a chat line ("豪华背包").  Those need a managed Item and a
     *   localised string to be built from native code, so this branch does not
     *   do them -- the purchase is silent apart from the money and the slots.
     *
     * TEXT -- Response..ctor 0x1A80AB0 + DialogueBox..ctor 0x17786A0
     *   These two strings are localised ASSETS, not numbers formatted at
     *   runtime: they come out of Content/Strings/Locations.zh-CN.xnb.  Both
     *   are the same length as the 48-slot wording, so the text can be
     *   rewritten in place:
     *       "背包升级：36格"     -> "定制背包：48格"      (all 8 units)
     *       "购买（10,000金）"   -> "购买（20,000金）"    (char 3: '1' -> '2')
     *   The rewrite is driven by the CURRENT maxItems, so it is self-healing:
     *   36 items -> 48-slot wording, 24 items -> back to 36-slot wording.
     *   Tier one (maxItems == 12) uses different string keys and is never
     *   touched.  Nothing else in the game reads these two strings.
     *
     * Both halves are switchable on their own so a bad build can be bisected
     * by flipping a constant instead of rewriting code. */
    constexpr bool kBagTier3Price = true;
    constexpr bool kBagTier3Text  = true;
    /* v331 shipped this as true and the console froze on the purchase; v333
     * re-enables it with the item's own DisplayName getter removed. */
    constexpr bool kBagTier3HoldUp = true;

    /* The tier-3 dialogue line, captured when we rewrite it (see
     * BagFixQuestionText).  Used as the hold-up item's name so the item never
     * has to resolve one itself. */
    static void* s_tier3Question = nullptr;

    constexpr int kBagTier3Cost = 20000;    /* 36 -> 48, ours    */

    constexpr uintptr_t kAnswerDialogueActionOff =
        SDV_OFF(kAnswerDialogueActionOff, 0x15182D0u);
    constexpr uintptr_t kDialogueBoxCtorOff =
        SDV_OFF(kDialogueBoxCtorOff, 0x17786A0u);
    constexpr uintptr_t kResponseCtorOff =
        SDV_OFF(kResponseCtorOff, 0x1A80AB0u);
    constexpr uintptr_t kGetMoneyOff = SDV_OFF(kGetMoneyOff, 0x13B97E0u);
    constexpr uintptr_t kSetMoneyOff = SDV_OFF(kSetMoneyOff, 0x13B9810u);
    /* SpecialItem(int which, string displayName) -- the two-argument ctor the
     * vanilla backpack purchase uses; passing this = 0 makes the runtime
     * allocate, same trick as Horse/DialogueBox above. */
    constexpr uintptr_t kSpecialItemCtorOff = SDV_OFF(kSpecialItemCtorOff, 0x1A3F010u);
    constexpr uintptr_t kSpecialItemDisplayNameOff =
        SDV_OFF(kSpecialItemDisplayNameOff, 0x1A3F390u);
    constexpr uintptr_t kHoldUpItemOff = SDV_OFF(kHoldUpItemOff, 0x13C0E90u);

    using AnswerDialogueActionFn = bool (*)(void*, void*, void*);
    using DialogueBoxCtorFn      = void* (*)(void*, void*, void*, int);
    using ResponseCtorFn         = void* (*)(void*, void*, void*);

    AnswerDialogueActionFn s_origAnswerDialogueAction = nullptr;
    DialogueBoxCtorFn      s_origDialogueBoxCtor      = nullptr;
    ResponseCtorFn         s_origResponseCtor         = nullptr;

    /* The player, or nullptr before one exists. */
    void* BagPlayer() {
        auto getPlayer = reinterpret_cast<void* (*)(void*)>(g_base + kGetPlayerOff);
        void* pl = getPlayer(nullptr);
        if (pl == nullptr || !Plausible(pl) ||
            !RangeReadable(reinterpret_cast<unsigned char*>(pl) + kMaxItemsOff, 8u)) {
            return nullptr;
        }
        return pl;
    }

    /* maxItems, or -1 when it cannot be read.  Same unwrap as MaybeGrowBackpack:
     * the field is a NetInt and the value lives at +0x68 of the wrapper. */
    int BagMaxItems(void* pl) {
        if (pl == nullptr) {
            return -1;
        }
        void* ref = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(pl) + kMaxItemsOff);
        if (ref == nullptr || !Plausible(ref) ||
            !RangeReadable(reinterpret_cast<unsigned char*>(ref) + 0x6Cu, 4u)) {
            return -1;
        }
        return *reinterpret_cast<int*>(reinterpret_cast<unsigned char*>(ref) + 0x68);
    }

    /* Is this bare managed string exactly these `n` UTF-16 units?  Length is
     * compared first, so nothing longer or shorter can match. */
    bool ManagedStrIsExact(const void* s, const char16_t* lit, int n) {
        if (lit == nullptr || ManagedStrLen(s) != n) {
            return false;
        }
        const unsigned char* chars = reinterpret_cast<const unsigned char*>(s) + 0x14;
        if (!RangeReadable(chars, static_cast<size_t>(n) * 2u)) {
            return false;
        }
        const char16_t* c = reinterpret_cast<const char16_t*>(chars);
        for (int i = 0; i < n; i++) {
            if (c[i] != lit[i]) {
                return false;
            }
        }
        return true;
    }

    bool ManagedStrPutChar(void* s, int idx, char16_t c) {
        const int len = ManagedStrLen(s);
        if (idx < 0 || idx >= len) {
            return false;
        }
        unsigned char* at = reinterpret_cast<unsigned char*>(s) + 0x14 +
                            static_cast<size_t>(idx) * 2u;
        if (!RangeReadable(at, 2u)) {
            return false;
        }
        *reinterpret_cast<char16_t*>(at) = c;
        return true;
    }

    /* One UTF-16 unit of a bare managed string, or 0 when unreadable. */
    char16_t ManagedStrChar(const void* s, int idx) {
        const int len = ManagedStrLen(s);
        if (idx < 0 || idx >= len) {
            return 0;
        }
        const unsigned char* at = reinterpret_cast<const unsigned char*>(s) + 0x14 +
                                  static_cast<size_t>(idx) * 2u;
        if (!RangeReadable(at, 2u)) {
            return 0;
        }
        return *reinterpret_cast<const char16_t*>(at);
    }

    /* The question line: vanilla "背包升级：36格" for the 24-item tier, our own
     * "定制背包：48格" for the 36-item one.
     *
     * Both are exactly 8 UTF-16 units, which is what makes this possible at
     * all -- the string object is rewritten in place, so its length can never
     * change.  Matched by SHAPE (8 units, ending in 格, and either the vanilla
     * "背包" head with two digits at 5/6 or our own "定制背包" head), so an
     * unrelated 8-character string cannot be hit. */
    void BagFixQuestionText(void* s, bool tier3) {
        if (ManagedStrLen(s) != 8 || ManagedStrChar(s, 7) != u'\u683C') {
            return;
        }
        const bool vanillaShape =
            ManagedStrChar(s, 0) == u'\u80CC' && ManagedStrChar(s, 1) == u'\u5305' &&
            ManagedStrChar(s, 5) == u'3' && ManagedStrChar(s, 6) == u'6';
        const bool customShape =
            ManagedStrChar(s, 0) == u'\u5B9A' && ManagedStrChar(s, 1) == u'\u5236' &&
            ManagedStrChar(s, 2) == u'\u80CC' && ManagedStrChar(s, 3) == u'\u5305';
        if (!vanillaShape && !customShape) {
            return;
        }
        /* 定 制 背 包 ： 4 8 格   /   背 包 升 级 ： 3 6 格 */
        static const char16_t kTier3Text[8] = {
            u'\u5B9A', u'\u5236', u'\u80CC', u'\u5305',
            u'\uFF1A', u'4', u'8', u'\u683C'};
        static const char16_t kTier2Text[8] = {
            u'\u80CC', u'\u5305', u'\u5347', u'\u7EA7',
            u'\uFF1A', u'3', u'6', u'\u683C'};
        const char16_t* want = tier3 ? kTier3Text : kTier2Text;
        for (int i = 0; i < 8; i++) {
            ManagedStrPutChar(s, i, want[i]);
        }
        if (tier3) {
            /* v333: keep the string so the hold-up item can be handed a real
             * name and its own getter (which calls LoadString) never runs.
             * Lifetime is not a worry: this IS the dictionary value that came
             * out of Content/Strings, so it lives as long as the asset does. */
            s_tier3Question = s;
        }
    }

    /* "购买（10,000金）" <-> "购买（20,000金）".
     *
     * Same idea: 11 characters, starting "购买", "0,000" at 4..8, ending "金）".
     * Only the leading digit at 3 is rewritten. */
    void BagFixResponseText(void* s, bool tier3) {
        if (ManagedStrLen(s) != 11) {
            return;
        }
        if (ManagedStrChar(s, 0) != u'\u8D2D' || ManagedStrChar(s, 1) != u'\u4E70' ||
            ManagedStrChar(s, 9) != u'\u91D1' || ManagedStrChar(s, 10) != u'\uFF09') {
            return;
        }
        if (ManagedStrChar(s, 4) != u'0' || ManagedStrChar(s, 5) != u',' ||
            ManagedStrChar(s, 6) != u'0' || ManagedStrChar(s, 7) != u'0' ||
            ManagedStrChar(s, 8) != u'0') {
            return;
        }
        const char16_t lead = ManagedStrChar(s, 3);
        if (lead < u'1' || lead > u'9') {
            return;
        }
        ManagedStrPutChar(s, 3, tier3 ? u'2' : u'1');
    }

    /* "Backpack_Purchase" -- 17 characters, ASCII. */
    bool IsBackpackPurchaseAnswer(const void* s) {
        return ManagedStrIsExact(s, u"Backpack_Purchase", 17);
    }

    bool HookAnswerDialogueAction(void* self, void* qa, void* params) {
        if (s_origAnswerDialogueAction == nullptr) {
            return false;               /* hook not installed: never called */
        }
        if (!kBagTier3Price) {
            return s_origAnswerDialogueAction(self, qa, params);
        }
        void* pl = BagPlayer();
        if (pl != nullptr && BagMaxItems(pl) == kVanillaMaxItems &&
            IsBackpackPurchaseAnswer(qa)) {
            auto getMoney = reinterpret_cast<int (*)(void*)>(g_base + kGetMoneyOff);
            auto setMoney = reinterpret_cast<void (*)(void*, int)>(g_base + kSetMoneyOff);
            const int money = getMoney(pl);
            if (money >= kBagTier3Cost) {
                /* OUR branch: charge 20000 and grow by 12.
                 *
                 * Farmer.increaseBackpackSize 0x13C3E50 does the two things
                 * that are easy to get wrong, in the game's own way:
                 *     maxItems.Value = maxItems.Value + 12      (NetInt setter)
                 *     while (Items.Count < maxItems) Items.Add(null)
                 * so the slots exist AND the item list is padded.  The same
                 * call is what MaybeGrowBackpack() already uses (kGrowInventoryOff). */
                setMoney(pl, money - kBagTier3Cost);
                /* Grow FIRST, then build and hold the item -- that is the order
                 * vanilla uses, and it matters for a different reason here: the
                 * only allocation between creating the item and handing it to
                 * the game is none, so the GC never gets a chance to look at an
                 * object that native code is the only holder of.
                 *
                 * The name is the dialogue line we already rewrote
                 * ("定制背包：48格"), so SpecialItem's own DisplayName getter --
                 * and the LoadString inside it -- never runs.  That getter was
                 * the one thing v331 did that vanilla's purchase does not. */
                auto grow = reinterpret_cast<void (*)(void*, int)>(g_base + kGrowInventoryOff);
                grow(pl, kBigMaxItems - kVanillaMaxItems);
                if (kBagTier3HoldUp && s_tier3Question != nullptr) {
                    auto makeItem = reinterpret_cast<void* (*)(void*, int, void*)>(
                        g_base + kSpecialItemCtorOff);
                    void* item = makeItem(nullptr, 99, s_tier3Question);
                    if (item != nullptr) {
                        auto holdUp = reinterpret_cast<void (*)(void*, void*, int)>(
                            g_base + kHoldUpItemOff);
                        holdUp(pl, item, 1);
                    }
                }
                return true;            /* handled: vanilla must not run */
            }
            /* Not enough money.  The only thing borrowed from vanilla here is
             * its own "not enough money" dialogue: run it against a zero
             * balance so its ">= 10000" test cannot pass, then put the real
             * balance back.  Nothing is bought either way. */
            setMoney(pl, 0);
            const bool handled = s_origAnswerDialogueAction(self, qa, params);
            setMoney(pl, money);
            return handled;
        }
        return s_origAnswerDialogueAction(self, qa, params);
    }

    void* HookDialogueBoxCtor(void* self, void* dialogue, void* responses, int width) {
        if (s_origDialogueBoxCtor == nullptr) {
            return nullptr;             /* hook not installed: never called */
        }
        if (kBagTier3Text) {
            const int maxItems = BagMaxItems(BagPlayer());
            if (maxItems == 24 || maxItems == kVanillaMaxItems) {
                BagFixQuestionText(dialogue, maxItems == kVanillaMaxItems);
            }
        }
        return s_origDialogueBoxCtor(self, dialogue, responses, width);
    }

    void* HookResponseCtor(void* self, void* key, void* text) {
        if (s_origResponseCtor == nullptr) {
            return nullptr;             /* hook not installed: never called */
        }
        if (kBagTier3Text) {
            const int maxItems = BagMaxItems(BagPlayer());
            if (maxItems == 24 || maxItems == kVanillaMaxItems) {
                BagFixResponseText(text, maxItems == kVanillaMaxItems);
            }
        }
        return s_origResponseCtor(self, key, text);
    }

    /* ================= v334: Carry Chest (Nexus 1333) ==================
     *
     * Carry a chest around with everything still inside it.  The PC mod
     * (spacechase0/CarryChest 1.3.6) is only four small changes, and its whole
     * trick is one fact: Chest derives from Item, so a whole chest -- items
     * and all -- can be shoved into a backpack slot as a single item.
     *
     *   pickup    Mod.OnButtonPressed
     *       left click + empty hand -> player.addItemToInventoryBool(chest)
     *                                  + location.objects.Remove(tile)
     *       i.e. it bypasses Chest.checkForAction entirely, whose gate is
     *       "only take an EMPTY chest".
     *
     *   put back  Mod.OnObjectListChanged
     *       Vanilla's Object.placementAction big-craftable fallback is
     *           Chest v = new Chest(playerChest: true, tile, ItemId);
     *           location.objects.Add(tile, v);
     *       -- a brand new EMPTY chest, so the contents would be gone.  The
     *       mod therefore remembers the chest in hand every tick and copies
     *       items / modData / Name / colour onto whatever chest appears.
     *
     *   Item.canStackWith   big craftables report maximumStackSize() == 999,
     *       so two chest items would stack; the one with contents would then
     *       be discarded, i.e. the contents silently eaten.
     *
     *   Object.getDescription   appends "Contains N items."
     *
     * ---- what this build does differently ------------------------------
     *
     * Putting it back does not need the mod's bookkeeping.  Chest.placement-
     * Action 0x1A0B530 is three instructions and a tail branch into
     * Object.placementAction, so hooking it means: let vanilla run (it does
     * the placement validation and writes Location/TileLocation), then swap
     * the empty chest it just built for the item itself.  Item and chest are
     * the same object, so contents, custom name, colour and modData are all
     * simply already there -- no PreviousHeldChest, no per-tick tracking.
     *
     * Picking up cannot be copied: the mod uses the left mouse button, but on
     * Switch
     *     Game1.didPlayerJustRightClick -> input.GetGamePadState().Buttons.A
     * so A is the right click (open the chest) and it is the only interact
     * button.  Instead this uses a heavy-hitter swing, the same gesture
     * vanilla already uses to knock an empty chest apart:
     *
     *   Chest.HandleChestHit 0x1A0A3C0
     *       lambda <HandleChestHit>b__0 0x1A11220
     *         0x1A11290  tbz w0,#0, #0x1A11430   ; !isEmpty() -> "kick it"
     *       so vanilla is: empty -> destroy and drop the chest item;
     *                      not empty -> shove it one tile.
     *   This takes over at the entry: not empty + ToolCanHit -> the whole
     *   chest goes into the backpack and vanilla never runs (so there is no
     *   shove any more).  Empty chests keep the vanilla behaviour.
     *
     *   ToolCanHit is filled in by Chest.performToolAction as
     *   `t.isHeavyHitter() && !(t is MeleeWeapon)`, i.e. axe / pickaxe / hoe.
     *   ChestHitArgs layout:
     *       +0x10 Location   +0x18 ChestTile.X   +0x1C ChestTile.Y
     *       +0x34 HoldDownClick   +0x35 ToolCanHit   +0x36 RecentlyHit
     *
     *   v334 acted on ToolCanHit alone, so an AXE swing carried the chest too --
     *   and in the one hardware test where that happened the chest left the
     *   world without reaching the backpack.  v335 restricts pickup to the
     *   PICKAXE: ChestHitArgs carries no tool reference, so the equipped tool's
     *   item id is tested for the "Pickaxe" suffix.  Every other tool now falls
     *   straight through to vanilla, untouched.
     *
     * ORDER MATTERS (the v331 lesson): add to the backpack FIRST, remove from
     * the world SECOND.  A chest only referenced by the native stack is
     * invisible to the GC; this way there is always a managed reference.
     *
     * Supported chests copy ChestHelper.IsSupported: 130 (Chest) and 232
     * (Stone Chest), and only playerChest -- so big chests, junimo chests,
     * mini shipping bins, fridges and giftboxes are all left alone.
     */
    constexpr bool kCarryChest = true;

    /* ---- addresses (E7F84509 / 1.6.15.13) ---- */
    constexpr uintptr_t kChestHitOff     = SDV_OFF(kChestHitOff,     0x1A0A3C0u);
    constexpr uintptr_t kChestPlaceOff   = SDV_OFF(kChestPlaceOff,   0x1A0B530u);
    constexpr uintptr_t kChestIsEmptyOff = SDV_OFF(kChestIsEmptyOff, 0x1A0E3B0u);
    constexpr uintptr_t kCanStackOff     = SDV_OFF(kCanStackOff,     0x15B0E20u);
    constexpr uintptr_t kAddItemBoolOff  = SDV_OFF(kAddItemBoolOff,  0x13CD110u);
    constexpr uintptr_t kGetObjectsOff   = SDV_OFF(kGetObjectsOff,   0x14C6DF0u);
    constexpr uintptr_t kDictAddOff      = SDV_OFF(kDictAddOff,      0x19A1220u);
    constexpr uintptr_t kDictRemoveOff   = SDV_OFF(kDictRemoveOff,   0x19A15D0u);

    /* ---- field offsets (.tmp/symtab_full.json) ---- */
    constexpr uintptr_t kNetValueOff         = 0x68u;   /* every Net* value lives here */
    constexpr uintptr_t kItemParentSheetOff  = 0x68u;   /* Item.parentSheetIndex (NetInt) */
    constexpr uintptr_t kItemStackOff        = 0x98u;   /* Item.stack (NetInt) */
    constexpr uintptr_t kChestPlayerChestOff = 0x260u;  /* Chest.playerChest (NetBool) */

    /* ---- extra call targets ---- */
    constexpr uintptr_t kGetCurrentToolOff = SDV_OFF(kGetCurrentToolOff, 0x13B8E20u);
    constexpr uintptr_t kGetIndexInInvOff  = SDV_OFF(kGetIndexInInvOff,  0x13CD3F0u);
    /* Item.get_ItemId -- the string getter, not the field.  itemId is a
     * NetString (Item.cs:68), so the field at +0x70 is a wrapper, and the
     * getter is the only honest way to read the value. */
    constexpr uintptr_t kGetItemIdOff      = SDV_OFF(kGetItemIdOff,      0x15AE860u);

    /* ---- ABI note ----
     * Vector2 is {float X, float Y}: an HFA, so it travels in s0/s1 and NOT in
     * a GPR.  Declaring the two components as two floats reproduces that
     * exactly.  Confirmed at 0x1A112DC: Utility.PointToVector2 leaves its
     * result in s0/s1, then `mov x0,x20` loads the dictionary and `bl Remove`
     * runs -- x1 is never written. */
    using ChestHitFn     = void (*)(void*, void*);
    using ChestPlaceFn   = bool (*)(void*, void*, int, int, void*);
    using CanStackFn     = bool (*)(void*, void*);
    using ChestIsEmptyFn = bool (*)(void*);
    using AddItemBoolFn  = bool (*)(void*, void*, bool);
    using GetObjectsFn   = void* (*)(void*);
    using DictRemoveFn   = bool (*)(void*, float, float);
    using DictAddFn      = void (*)(void*, float, float, void*);

    static ChestHitFn   s_origChestHit   = nullptr;
    static ChestPlaceFn s_origChestPlace = nullptr;
    static CanStackFn   s_origCanStack   = nullptr;

    /* MethodTable of StardewValley.Objects.Chest, learned from a real instance
     * (Chest.HandleChestHit's `this`, or the item Chest.placementAction is
     * placing).  Comparing object[0] against it is the only safe way to ask
     * "is this a Chest?" -- reading Chest fields off a non-Chest is a wild
     * pointer. */
    static void* s_chestType = nullptr;

    void* CarryPlayer() {
        auto getPlayer = reinterpret_cast<void* (*)(void*)>(g_base + kGetPlayerOff);
        void* pl = getPlayer(nullptr);
        if (pl == nullptr || !Plausible(pl)) {
            return nullptr;
        }
        return pl;
    }

    /* --- daily luck -------------------------------------------------------
     *
     * Sampled in the update hook and cached; the draw hook only reads the
     * cached number.  get_DailyLuck is a managed method, and calling managed
     * code from a render hook is the exact thing that has frozen this console
     * before (v392 notes).
     *
     * The value is kept as luck*100 rounded, so "did it change" is an integer
     * compare and the HUD string is only rebuilt when the number really moves.
     */
    int   s_luckHundredths = 0;
    int   s_luckShown = 0x7FFFFFFF;   /* value the HUD string was built from */
    int   s_luckFrame = 0;

    void SampleDailyLuck() {
        if (++s_luckFrame < 60) {
            return;
        }
        s_luckFrame = 0;
        void* pl = CarryPlayer();
        if (pl == nullptr) {
            return;
        }
        auto getDailyLuck = reinterpret_cast<double (*)(void*)>(
            g_base + kGetDailyLuckOff);
        const double v = getDailyLuck(pl);
        /* Daily luck is clamped to +-0.2 in the game.  Anything outside that
         * means the read went wrong, and a garbage number on the HUD is worse
         * than a stale one. */
        if (!(v >= -1.0 && v <= 1.0)) {
            return;
        }
        int h = static_cast<int>(v * 100.0 + (v >= 0.0 ? 0.5 : -0.5));
        if (h < -100) {
            h = -100;
        }
        if (h > 100) {
            h = 100;
        }
        s_luckHundredths = h;
    }

    /* Tier colours, straight out of UI Info Suite 2's LuckOfDay.  XNA packs
     * Color as AABBGGRR, so the bytes go in reversed order. */
    unsigned LuckColour(int h) {
        int r = 255;
        int g = 255;
        int b = 255;
        int a = 255;
        if (h > 7) {
            r = 87;  g = 255; b = 106;          /* very good */
        }
        else if (h > 2) {
            r = 148; g = 255; b = 210;          /* good */
        }
        else if (h < 0 && h >= -2) {
            r = 246; g = 255; b = 145;          /* slightly bad */
        }
        else if (h == 0) {
            r = 255; g = 255; b = 255;          /* neutral */
        }
        else if (h >= -7) {
            r = 255; g = 155; b = 155;          /* bad */
        }
        else if (h < -7) {
            r = 165; g = 165; b = 165; a = 204; /* very bad */
        }
        return (static_cast<unsigned>(a) << 24) |
               (static_cast<unsigned>(b) << 16) |
               (static_cast<unsigned>(g) << 8) |
               static_cast<unsigned>(r);
    }

    /* Unwrap a Net* field of `owner` and return a pointer to the value it
     * holds.  Null when either hop is not readable. */
    void* NetValueSlot(void* owner, uintptr_t off, size_t bytes) {
        if (owner == nullptr ||
            !RangeReadable(reinterpret_cast<unsigned char*>(owner) + off, 8u)) {
            return nullptr;
        }
        void* wrapper = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(owner) + off);
        if (wrapper == nullptr ||
            !RangeReadable(reinterpret_cast<unsigned char*>(wrapper) + kNetValueOff,
                           bytes)) {
            return nullptr;
        }
        return reinterpret_cast<unsigned char*>(wrapper) + kNetValueOff;
    }

    /* --- tool upgrade icon ------------------------------------------------
     *
     * The tool in the shop is a real object, so we can show its actual sprite
     * instead of a generic one.  Everything here runs in the update hook:
     * getSquareSourceRectForNonStandardTileSheet calls Texture2D.get_Width,
     * which is a managed getter, and managed getters do not belong in a render
     * hook (v392 froze a console that way).  The draw hook only reads the
     * cached rectangle.
     */
    struct SourceRect {
        int x, y, w, h;
    };

    int   s_toolRectX = 0;
    int   s_toolRectY = 0;
    int   s_toolRectW = 0;
    int   s_toolRectH = 0;
    int   s_toolRectValid = 0;
    int   s_toolFrame = 0;
    void* s_toolSheet = nullptr;

    void SampleToolUpgrade() {
        s_toolRectValid = 0;
        void* pl = CarryPlayer();
        if (pl == nullptr) {
            return;
        }
        void* refSlot = NetValueSlot(pl, kFarmerToolUpgradingOff, 8u);
        if (refSlot == nullptr) {
            return;
        }
        void* tool = *reinterpret_cast<void**>(refSlot);
        if (tool == nullptr || !Plausible(tool)) {
            return;
        }
        void* idxSlot = NetValueSlot(tool, kToolMenuIndexOff, 4u);
        if (idxSlot == nullptr) {
            return;
        }
        const int idx = *reinterpret_cast<int*>(idxSlot);
        if (idx < 0 || idx > 1023) {
            return;
        }
        void* sheet = *reinterpret_cast<void**>(g_base + kToolSpriteSheetOff);
        if (sheet == nullptr || !Plausible(sheet)) {
            return;
        }
        /* The game's own helper: it divides by the texture width, so no
         * assumption about the sheet layout is baked in here. */
        auto getRect = reinterpret_cast<SourceRect (*)(void*, int, int, int)>(
            g_base + kGetSquareRectOff);
        const SourceRect r = getRect(sheet, 16, 16, idx);
        if (r.w != 16 || r.h != 16 || r.x < 0 || r.y < 0) {
            return;
        }
        s_toolRectX = r.x;
        s_toolRectY = r.y;
        s_toolRectW = r.w;
        s_toolRectH = r.h;
        s_toolSheet = sheet;
        s_toolRectValid = 1;
    }

    bool IsChestObj(const void* obj) {
        if (obj == nullptr || s_chestType == nullptr) {
            return false;
        }
        if (!RangeReadable(obj, 8u)) {
            return false;
        }
        return *reinterpret_cast<void* const*>(obj) == s_chestType;
    }

    int ChestParentSheetIndex(void* chest) {
        void* slot = NetValueSlot(chest, kItemParentSheetOff, 4u);
        return slot != nullptr ? *reinterpret_cast<int*>(slot) : -1;
    }

    bool ChestFlag(void* chest, uintptr_t off) {
        void* slot = NetValueSlot(chest, off, 1u);
        return slot != nullptr && *reinterpret_cast<unsigned char*>(slot) != 0;
    }

    bool ChestIsEmpty(void* chest) {
        auto isEmpty = reinterpret_cast<ChestIsEmptyFn>(g_base + kChestIsEmptyOff);
        return isEmpty(chest);
    }

    /* ChestHelper.IsSupported: 130 / 232, player-placed only. */
    bool IsCarryableChest(void* chest) {
        if (!IsChestObj(chest)) {
            return false;
        }
        const int idx = ChestParentSheetIndex(chest);
        if (idx != 130 && idx != 232) {
            return false;
        }
        return ChestFlag(chest, kChestPlayerChestOff);
    }

    /* ---- pickaxe only -------------------------------------------------
     *
     * The tool that produced the hit is NOT part of ChestHitArgs -- all
     * HandleChestHit gets is a ToolCanHit bool, which is true for axe, pickaxe
     * and hoe alike.  v334 acted on that bool alone, so an axe swing took the
     * chest too.  v335 therefore reads the equipped tool instead.
     *
     * Item ids are not localised, so a suffix test on "Pickaxe" is enough:
     * "Pickaxe", "CopperPickaxe", "SteelPickaxe", "GoldPickaxe" and
     * "IridiumPickaxe" all end with it and no axe id does.  Same managed-string
     * accessors the backpack text rewriting already uses (length at +0x10,
     * UTF-16 from +0x14). */
    bool StrEndsWithAscii(const void* s, const char* suffix) {
        int n = 0;
        while (suffix[n] != '\0') {
            ++n;
        }
        const int len = ManagedStrLen(s);
        if (len < n) {
            return false;
        }
        for (int i = 0; i < n; ++i) {
            if (ManagedStrChar(s, len - n + i) != static_cast<char16_t>(suffix[i])) {
                return false;
            }
        }
        return true;
    }

    bool EquippedToolIsPickaxe(void* pl) {
        auto getTool = reinterpret_cast<void* (*)(void*)>(g_base + kGetCurrentToolOff);
        void* tool = getTool(pl);
        if (tool == nullptr || !Plausible(tool)) {
            return false;
        }
        auto getItemId = reinterpret_cast<void* (*)(void*)>(g_base + kGetItemIdOff);
        void* id = getItemId(tool);
        return StrEndsWithAscii(id, "Pickaxe");
    }

    /* Farmer.getIndexOfInventoryItem compares by reference first
     * (Items[i] == item), so this answers "is THIS chest object, not a
     * lookalike, sitting in the backpack".  That is the check that makes
     * content loss impossible: nothing leaves the world until it passes. */
    bool ChestIsInBackpack(void* pl, void* chest) {
        auto indexOf = reinterpret_cast<int (*)(void*, void*)>(g_base + kGetIndexInInvOff);
        return indexOf(pl, chest) >= 0;
    }

    /* ---- pickup: Chest.HandleChestHit(ChestHitArgs) ---- */
    void HookChestHit(void* self, void* args) {
        /* `this` is always a Chest here, so this is the one place the type
         * pointer can be learned without already knowing it. */
        if (self != nullptr && RangeReadable(self, 8u)) {
            s_chestType = *reinterpret_cast<void**>(self);
        }
        if (s_carryCalls < 0xFu) {
            ++s_carryCalls;
        }
        if (kCarryChest && args != nullptr && IsCarryableChest(self) &&
            *reinterpret_cast<unsigned char*>(
                reinterpret_cast<unsigned char*>(args) + 0x35u) != 0) {
            void* loc = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(args) + 0x10u);
            /* Player before Chest.isEmpty(): that call goes through
             * GetItemsForPlayer() -> Game1.player.UniqueMultiplayerID and
             * would fault if there were no player. */
            void* pl = CarryPlayer();
            /* v335: pickaxe only.  ToolCanHit above is true for axe/hoe too,
             * and v334 acted on it alone -- an axe swing carried the chest. */
            const bool isPickaxe = pl != nullptr && EquippedToolIsPickaxe(pl);
            const bool notEmpty = pl != nullptr && !ChestIsEmpty(self);
            /* v336: record the gate trace before anything can bail out. */
            s_carryState = 0xCu | (isPickaxe ? 0x2u : 0u) | (notEmpty ? 0x1u : 0u);
            s_carryAdd = 0;
            s_carryInBag = 0;
            s_carryRemoved = 0;
            if (loc != nullptr && isPickaxe && notEmpty) {
                auto getObjects = reinterpret_cast<GetObjectsFn>(g_base + kGetObjectsOff);
                void* dict = getObjects(loc);
                if (dict != nullptr) {
                    auto addItem = reinterpret_cast<AddItemBoolFn>(
                        g_base + kAddItemBoolOff);
                    void* stackSlot = NetValueSlot(self, kItemStackOff, 4u);
                    /* v336: a world chest's Stack must be 1.  addItemToInventory
                     * starts its countdown at item.Stack, so a zeroed Stack makes
                     * it skip both the merge loop and the free-slot loop and
                     * still report success -- the chest would be "added" nowhere. */
                    if (stackSlot != nullptr &&
                        *reinterpret_cast<int*>(stackSlot) < 1) {
                        *reinterpret_cast<int*>(stackSlot) = 1;
                    }
                    /* false: let the game pick the slot; forcing it active
                     * would yank the player's held tool away. */
                    s_carryAdd = addItem(pl, self, false) ? 1u : 0u;
                    s_carryInBag = ChestIsInBackpack(pl, self) ? 1u : 0u;
                    if (s_carryAdd != 0 && s_carryInBag != 0) {
                        const int tx = *reinterpret_cast<int*>(
                            reinterpret_cast<unsigned char*>(args) + 0x18u);
                        const int ty = *reinterpret_cast<int*>(
                            reinterpret_cast<unsigned char*>(args) + 0x1Cu);
                        auto remove = reinterpret_cast<DictRemoveFn>(
                            g_base + kDictRemoveOff);
                        s_carryRemoved =
                            remove(dict, static_cast<float>(tx),
                                   static_cast<float>(ty)) ? 1u : 0u;
                        LOGF("carrychest: picked up chest at %d,%d (removed=%d)",
                             tx, ty, static_cast<int>(s_carryRemoved));
                        return;     /* vanilla never runs: no shove, no delete */
                    }
                    /* Either the backpack was full, or addItem reported success
                     * without this exact object landing in a slot.  Leave the
                     * chest standing: one that refuses to move is recoverable,
                     * eaten contents are not. */
                    if (stackSlot != nullptr) {
                        *reinterpret_cast<int*>(stackSlot) = 1;
                    }
                    LOGF("carrychest: not in backpack, chest left in place");
                }
            }
        }
        if (s_origChestHit != nullptr) {
            s_origChestHit(self, args);
        }
    }

    /* ---- put back: Chest.placementAction(location, x, y, who) ---- */
    bool HookChestPlace(void* self, void* loc, int x, int y, void* who) {
        if (self != nullptr && RangeReadable(self, 8u)) {
            s_chestType = *reinterpret_cast<void**>(self);
        }
        const bool ok = s_origChestPlace != nullptr &&
                        s_origChestPlace(self, loc, x, y, who);
        if (!ok || !kCarryChest || loc == nullptr || !IsCarryableChest(self) ||
            ChestIsEmpty(self)) {
            return ok;          /* empty chest: vanilla's fresh copy is fine */
        }
        auto getObjects = reinterpret_cast<GetObjectsFn>(g_base + kGetObjectsOff);
        void* dict = getObjects(loc);
        if (dict == nullptr) {
            /* Cannot swap, and returning true would let the caller consume the
             * item while its contents sit in an empty chest on the ground.
             * Refuse instead: the item stays in the backpack (a stray empty
             * chest is recoverable, eaten contents are not). */
            LOGF("carrychest: no object dictionary, placement refused");
            return false;
        }
        const float tx = static_cast<float>(x / 64);
        const float ty = static_cast<float>(y / 64);
        /* Remove first: OverlaidDictionary.Add throws if the key is present. */
        reinterpret_cast<DictRemoveFn>(g_base + kDictRemoveOff)(dict, tx, ty);
        reinterpret_cast<DictAddFn>(g_base + kDictAddOff)(dict, tx, ty, self);
        LOGF("carrychest: placed chest at %d,%d", x / 64, y / 64);
        return ok;
    }

    /* ---- no stacking: two chests with contents must never merge ---- */
    bool ChestBlocksStack(void* item) {
        if (!IsChestObj(item)) {
            return false;
        }
        /* Chest.isEmpty -> GetItemsForPlayer -> Game1.player.  canStackWith can
         * run while a save is still loading, when there is no player yet. */
        if (CarryPlayer() == nullptr) {
            return false;
        }
        return !ChestIsEmpty(item);
    }

    bool HookCanStack(void* self, void* other) {
        const bool r = s_origCanStack != nullptr && s_origCanStack(self, other);
        if (!r || !kCarryChest) {
            return r;
        }
        if (ChestBlocksStack(self) || ChestBlocksStack(other)) {
            return false;
        }
        return true;
    }

    /* ================= v337: Automate Lite (箱子 <-> 机器) ==============
     *
     * 箱子挨着机器放，机器自动进料、自动出货。
     *
     * 这不是照搬 PC 的 Automate（10983 行 / 149 个文件，深度依赖 SMAPI
     * 的事件、配置 UI、i18n、多 mod 集成、联机消息），而是重写它最核心的
     * 那一段。Automate 2.6.x 改成数据驱动之后，
     * DataBasedObjectMachine.SetInput 的核心只有一句：
     *
     *     machine.AttemptAutoLoad(container.Inventory, Game1.player)
     *
     * 而 AttemptAutoLoad 是 **1.6 原版**给自动装载箱（275）写的：
     *
     *     if (heldObject.Value != null) return false;
     *     foreach (Item item in inventory)
     *         if (performObjectDropInAction(item, probe:false, who)) return true;
     *     return false;
     *
     * 也就是「遍历箱子里的东西、逐个试能不能塞进机器」这半边原版已经写好了
     * —— 配方、数量、燃料规则全由游戏自己判断。我们只负责两件事：
     * 找到「箱子 + 相邻机器」，以及出货时把产物塞进箱子。
     *
     * ---- v1 的范围 ----------------------------------------------------
     *   做：当前场景、上下左右紧邻、箱子→机器进料、机器→箱子出货
     *   不做：木头小径等连接器、鱼塘、祝尼魔屋、洒水器、树液采集器、
     *         OutputMachine 的额外产出（种子机的额外种子等）、后台场景、UI
     *
     * 出货的清理顺序照抄原版 Object.checkForAction 里手动收成品那一段：
     *     heldObject = null; readyForHarvest = false; showNextIndex = false;
     *     ResetParentSheetIndex();
     * 但**必须先让箱子收下**（Chest.addItem 返回 null）再清状态 —— 否则
     * 箱子满了就会把产物吞掉。
     *
     * 树液采集器（IsTapper）**直接跳过**：原版收完还要
     * Tree.UpdateTapperProduct 才能进入下一轮，v1 不做那一步，动了会让
     * 采集器停产。
     *
     * ---- 遍历方式 ----------------------------------------------------
     * 不用字典内部布局（OverlaidDictionary 的私有字段没进元数据，照抄别人
     * 的 +0x20/+0x28 不可靠），改成按格子扫 + objects.TryGetValue：
     *     019a1710: ldr x0,[x0,#0x20]   ; compositeDict
     *     019a1714: b   #0x1557770      ; Dictionary<Vector2,Object>.TryGetValue
     * 100x100 覆盖所有实际地图（农场 80x65），每 30 tick 一次。
     *
     * ---- 箱子识别 ----------------------------------------------------
     * 用 ParentSheetIndex 130/232（普通箱 / 石箱）。世界里的箱子一定是
     * Chest 实例（Object.placementAction 用 new Chest(...) 造），所以可以
     * 直接调 Chest 的方法。这也是 PC Automate 的做法。
     */
    constexpr bool kAutomate = true;

    /* v372: the Mod page switch ("启用自动化").  The hooks themselves are
     * installed unconditionally -- that is a boot-time decision -- so this is
     * the per-frame gate every entry point checks. */
    bool AutomateOn() {
        return kAutomate && g_rowOn[kRowAutomate] != 0;
    }

    constexpr uintptr_t kGame1CurrentLocOff = SDV_OFF(kGame1CurrentLocOff, 0x14407B0u);
    constexpr uintptr_t kDictTryGetOff      = SDV_OFF(kDictTryGetOff,      0x19A1710u);
    constexpr uintptr_t kChestAddItemOff    = SDV_OFF(kChestAddItemOff,    0x1A0D680u);
    constexpr uintptr_t kChestGetItemsOff   = SDV_OFF(kChestGetItemsOff,   0x1A0D350u);
    constexpr uintptr_t kAutoLoadOff        = SDV_OFF(kAutoLoadOff,        0x19EBF70u);
    constexpr uintptr_t kItemResetPsiOff    = SDV_OFF(kItemResetPsiOff,    0x15AF470u);
    constexpr uintptr_t kGetMachineDataOff  = SDV_OFF(kGetMachineDataOff,  0x19DB930u);
    constexpr uintptr_t kIsTapperOff        = SDV_OFF(kIsTapperOff,        0x19F6460u);
    /* v338: Chest.updateWhenCurrentLocation -- see the note above the hook. */
    constexpr uintptr_t kChestUpdateOff     = SDV_OFF(kChestUpdateOff,     0x1A0E610u);
    /* v371: fish ponds.  GameLocation.updateWhenCurrentLocation already does
     *     foreach (Building b in buildings) b.Update(time);
     * and FishPond overrides Update, so hooking it hands us every pond in the
     * current location every frame -- the same trick as Chest.updateWhenCurrentLocation.
     * Field offsets: Building.tileX/tileY/tilesWide/tilesHigh are NetInts at
     * 0x40/0x48/0x50/0x58; FishPond.output is a NetRef<Item> at 0x178. */
    constexpr uintptr_t kFishPondUpdateOff  = SDV_OFF(kFishPondUpdateOff,  0x120AE00u);
    constexpr uintptr_t kPondOutputOff      = 0x178u;
    constexpr uintptr_t kBuildingTileXOff   = 0x40u;
    constexpr uintptr_t kBuildingTileYOff   = 0x48u;
    constexpr uintptr_t kBuildingWideOff    = 0x50u;
    constexpr uintptr_t kBuildingHighOff    = 0x58u;
    /* Object.updateWhenCurrentLocation -- the base implementation every plain
     * machine uses (Chest/Furniture/CrabPot/... override it). */
    constexpr uintptr_t kObjUpdateOff       = SDV_OFF(kObjUpdateOff,       0x19DF2D0u);

    /* ---- v358: re-priming a collected tapper -------------------------
     *
     * Vanilla, after taking a tapper's product, does
     *
     *     if (IsTapper() && location.terrainFeatures.TryGetValue(tile, out var f)
     *         && f is Tree tree)
     *         tree.UpdateTapperProduct(this, product);
     *
     * without which the tapper stays empty until Tree.DayUpdate re-primes it
     * the next morning.  All four pieces below were read off that very call
     * site in Object.checkForAction (0x19E7CE4..0x19E7D68):
     *
     *     019e7ce4: ldr x23, [x23, #0x148]   ; location.terrainFeatures
     *     019e7cf8: ldr x0,  [x23, #0x58]    ; the inner Dictionary
     *     019e7d0c: bl  #0xdd0d70            ; Dictionary<Vector2,TerrainFeature>
     *                                        ;   .TryGetValue(x0, s0/s1, x1=out)
     *     019e7d68: bl  #0x1b29830           ; Tree.UpdateTapperProduct(
     *                                        ;   tree, tapper, prev, false)
     */
    /* The three values below are the same ones kTerrainFeaturesOff /
     * kNetDictInnerOff / kTryGetValueOff hold further down (v265 read them off
     * Hoe.DoFunction); they are repeated here only because those declarations
     * come after this point in the file.  Two independent derivations landing
     * on the same numbers is the reason to trust them. */
    constexpr uintptr_t kTapperTerrainOff = 0x148u; /* GameLocation.terrainFeatures */
    constexpr uintptr_t kTapperDictOff    = 0x58u;  /* NetDictionary -> Dictionary */
    constexpr uintptr_t kTapperTryGetOff  = SDV_OFF(kTapperTryGetOff, 0x0DD0D70u);
    constexpr uintptr_t kTapperUpdateOff  = SDV_OFF(kTapperUpdateOff, 0x1B29830u);
    constexpr uintptr_t kObjToStringOff   = SDV_OFF(kObjToStringOff,  0x0259EE0u);
    /* Character.getTileX / getTileY -- ints, so no HFA return to get wrong. */
    /* v388: Farmer.disconnectDay -- a NetInt that single player never reads.
     * Candidate carrier for the switch mask: it is in netFields, so it rides
     * the FARM save (which is written on sleep), not preferences. */
    constexpr uintptr_t kFarmerDiscDayOff = 0x848u;
    /* v394: the marker lives in BYTE 2, not in the whole high half.
     *
     * Hardware, v393: we wrote 0x5A5A0EFE and read back 0x985A0EFE -- bytes
     * 2/1/0 (5A 0E FE) survived byte for byte, only the TOP byte changed
     * (5A -> 98).  So byte 3 of that int is not ours to trust, and the
     * `(v & 0xFFFF0000) == 0x5A5A0000` test rejected a value that was in fact
     * ours, which is why the switches came back at their defaults even though
     * the field itself survived a full game restart.
     *
     * I do NOT know what rewrites byte 3 (see docs/HANDOFF.md); the design
     * therefore stops depending on it:
     *
     *   bits  0..11  switch mask          (bit 0 unused, always 0)
     *   bits 12..15  must be 0            (part of the same check)
     *   bits 16..23  marker 0x5A
     *   bits 24..31  ignored -- whatever the game puts there
     *
     * That is 13 constrained bits across the three bytes hardware proved
     * stable, so an accidental match is as unlikely as before while the top
     * byte is free to be anything.
     *
     * The old single-bit tag kMaskTag (0x4000) is still wrong on its own:
     * the field's untouched default 0xFEFFFEFF has bit 14 set, which decoded
     * to mask 0xEFE and silently switched row 8 (打怪) off. */
    constexpr unsigned kDiscMark     = 0x005A0000u;  /* byte 2 */
    constexpr unsigned kDiscMarkMask = 0x00FF0000u;
    constexpr unsigned kDiscMaskBits = 0x0FFEu;      /* bits 1..11 */
    constexpr unsigned kDiscMustZero = 0xF001u;      /* bit 0 + bits 12..15 */

    /* v390: disconnectDay persistence.
     *
     * Farmer.disconnectDay is a NetInt at Farmer+0x848.  Verified three ways:
     *   1. aot_meta decodes it at 0x848 with type Netcode.NetInt (size 0x78)
     *   2. Farmer.initNetFields (0x13b9bf0) does `ldr x1,[x19,#0x848]` at
     *      0x13ba244 and registers it -- and the very next two loads are
     *      0x850 / 0x858, i.e. disconnectLocation / disconnectPosition.  So the
     *      field really is in netFields and really is serialised with the farm.
     *   3. NetInt's own value initialiser 0x117e450 -> 0xda1010 shows the exact
     *      layout (see WriteDisconnectDay below).
     *
     * Single-player never reads or writes disconnectDay -- it is a multiplayer
     * "when did the connection drop?" field.  We borrow it as a carrier for the
     * switch mask:
     *
     *   - WRITE: when a switch changes, write kMaskTag|mask into the NetInt.
     *     This rides the next farm save (sleep) -- no TryPerformSave needed.
     *   - READ: when a farm loads, disconnectDay already has the value from
     *     the save.  If it has our tag, apply bits 1..11 as the switch mask.
     *
     * The preferences path (preferredResolutionX) still handles boot-time
     * persistence.  disconnectDay handles the "sleep + reload farm" path that
     * preferences miss (they only write at boot). */

    /* Read the raw int from Farmer.disconnectDay's NetInt value slot.
     * Returns 0xFFFFFFFF when the player or wrapper is not readable. */
    unsigned ReadDisconnectDay() {
        void* pl = CarryPlayer();
        if (pl == nullptr) {
            return 0xFFFFFFFFu;
        }
        void* slot = NetValueSlot(pl, kFarmerDiscDayOff, 4u);
        if (slot == nullptr) {
            return 0xFFFFFFFFu;
        }
        return *reinterpret_cast<unsigned*>(slot);
    }

    /* v392 -- THE FIX.  Writing +0x68 alone does not stick.
     *
     * NetInt's value initialiser (0x117e450 -> 0xda1010) does:
     *
     *     00da1044  ldr w22, [x21, #0x70]      ; old targetValue
     *     00da1050  ldr w20, [x21, #0x68]      ; old value
     *     00da1054  stp wzr, w19, [x21, #0x6c] ; previousValue = 0
     *                                          ; targetValue   = new
     *     00da1058  str w19,      [x21, #0x68] ; value         = new
     *
     * so a NetInt keeps THREE copies of the number, and NetFieldBase's
     * per-tick interpolation lerps `value` from `previousValue` towards
     * `targetValue`.  v390/v391 stored only +0x68, left targetValue at the old
     * number, and the interpolator pulled it straight back -- which is exactly
     * what hardware showed: the write counter reached 1 but the value stayed
     * FEFF.
     *
     * Writing all three the way the real setter does makes the interpolator
     * converge on our number instead of away from it.  Plain 32-bit stores, no
     * managed call, so this is still safe from the draw hook. */
    void WriteDisconnectDay(unsigned value) {
        void* pl = CarryPlayer();
        if (pl == nullptr) {
            return;
        }
        /* Wrapper base: NetValueSlot already validated owner+off and the
         * wrapper's value slot, so base+0x68 is the slot it returned. */
        void* slot = NetValueSlot(pl, kFarmerDiscDayOff, 4u);
        if (slot == nullptr) {
            return;
        }
        unsigned char* base =
            reinterpret_cast<unsigned char*>(slot) - kNetValueOff;
        if (!RangeReadable(base + 0x68u, 0xCu)) {
            return;
        }
        /* v392: all three copies get the SAME number.
         *
         * The real setter stores previousValue = 0 because a Set() is a jump
         * and the netcode wants to animate towards the new number.  We do not
         * want any animation at all -- we want the field to simply hold our
         * value -- so previousValue = targetValue = value leaves the
         * interpolator nothing to move.  NetFieldBase's lerp is
         * `value = previous + (target - previous) * t`, which is a no-op when
         * all three are equal, whatever t is. */
        *reinterpret_cast<unsigned*>(base + 0x68u) = value;  /* value      */
        *reinterpret_cast<unsigned*>(base + 0x6Cu) = value;  /* previous   */
        *reinterpret_cast<unsigned*>(base + 0x70u) = value;  /* target     */
    }

    /* If disconnectDay carries our marker, extract the mask and apply it. */
    void ApplyDisconnectDayMask() {
        unsigned v = ReadDisconnectDay();
        if (v == 0xFFFFFFFFu) {
            return;
        }
        /* v394: byte 2 is the marker, bytes 3 is ignored (see kDiscMark). */
        if ((v & kDiscMarkMask) != kDiscMark) {
            return;
        }
        if ((v & kDiscMustZero) != 0u) {
            return;
        }
        unsigned mask = v & kDiscMaskBits;
        s_fileMask = mask;
        s_fileMaskOk = 2;    /* 2 = from disconnectDay, not preferences */
        for (int i = 1; i < kRowCount; i++) {
            g_rowOn[i] = (mask >> i) & 1u;
        }
        g_tractorOn = g_rowOn[kRowMaster];
        g_mapOn = g_rowOn[kRowMap];
    }

    /* Write the current switch state into disconnectDay. */
    void FlushMaskToDisconnectDay() {
        unsigned m = 0;
        for (int i = 1; i < kRowCount; i++) {
            if (g_rowOn[i] != 0) {
                m |= (1u << i);
            }
        }
        WriteDisconnectDay(kDiscMark | (m & kDiscMaskBits));
    }

    constexpr uintptr_t kCharTileXOff     = SDV_OFF(kCharTileXOff,    0x1224440u);
    constexpr uintptr_t kCharTileYOff     = SDV_OFF(kCharTileYOff,    0x1224470u);
    /* v363: TerrainFeature subclass identity without touching a string.
     * MeleeWeapon.DoDamage (0x1B5B508) dispatches performToolAction as
     *
     *     ldr x8, [obj]          ; EEType
     *     ldr x8, [x8, #8]       ; method table
     *     ldr x9, [x8, #0x120]   ; slot 0x120
     *     ldrb w8, [x8, #0x128]  ; interface offset
     *     add x0, obj, x8
     *     blr x9
     *
     * so the implementation an object uses for that slot is
     *     *(void**)(*(void**)(*(void**)obj + 8) + 0x120)
     * and comparing it against a known body address identifies the type with no
     * managed call and no string.  If the layout ever stops matching, the
     * comparison simply never succeeds -- it cannot match another type unless
     * that type's slot really is the same address. */
    /* v370: kFlooringPtaOff / kTreePtaOff are gone -- hardware proved slot 0x120
     * is not performToolAction (it read back 0x3D90).  Type identity comes from
     * the class name (v369), cached as a klass pointer (v370). */
    /* v366: the size lives at klass+0, NOT klass+4.
     *
     * This build is IL2CPP, not NativeAOT -- tools/aot_meta.py decodes an
     * Il2CppClass with CLASS_SIZE = 0x00 and CLASS_NAME = 0x30, and an object
     * is { klass*, monitor*, fields... } (TerrainFeature.NeedsTick sits at
     * 0x10, which is the giveaway).  v365 read +4, i.e. the upper half of that
     * u64, which is always 0 -- so the check never matched and the readout
     * would have shown 0000.
     *
     * Among every TerrainFeature subclass these two sizes are unique:
     *     Flooring 0x60, Tree 0xE8
     * (Grass/HoeDirt 0xA0, Bush 0xA8, FruitTree 0xD8, GiantCrop 0x80,
     *  ResourceClump 0x78, Tent 0x58, TerrainFeature 0x30.)
     * Pure native, no string, no vtable.  If the offset is ever wrong the read
     * simply never equals these values -- it cannot turn grass into a wire. */
    constexpr unsigned kFlooringSize = 0x60u;
    constexpr unsigned kTreeSize     = 0xE8u;

    /* Object 字段，逐个和 1.6.15.3 的布局对过（那边 5 个偏移完全一致）。 */
    constexpr uintptr_t kObjTileLocOff  = 0x0A0u;   /* Object.tileLocation (NetVector2) */
    constexpr uintptr_t kObjBigCraftOff = 0x100u;   /* Object.bigCraftable (NetBool) */
    constexpr uintptr_t kObjReadyOff    = 0x118u;   /* Object.readyForHarvest       */
    constexpr uintptr_t kObjShowNextOff = 0x120u;   /* Object.showNextIndex         */
    constexpr uintptr_t kObjHeldOff     = 0x138u;   /* Object.heldObject (NetRef)   */
    /* Object._hasHeldObject -- a plain bool cache kept in sync by
     * heldObject's change event.  A raw write to heldObject leaves it stale,
     * and Object.updateWhenCurrentLocation dereferences heldObject whenever it
     * is set.  See the fix in the collect loop. */
    constexpr uintptr_t kHeldCacheOff   = 0x208u;

    /* Frames between automation attempts.  At 60 fps this is ~7.5 Hz, which is
     * far faster than any machine changes state and keeps the managed-call
     * volume per frame tiny. */
    /* v358: back to 8.  v345 ran at 1 (every frame, 8x the load) purely to
     * squeeze the crash into a shorter window; that hunt is over, so the
     * production cadence is restored. */
    constexpr unsigned kAutomateInterval = 8;

    /* v341: one line per automation cycle into sd:/sdv_map.log.  There is no
     * other way to see what the mod was doing when the console reports
     * "the software was closed because an error occurred", so every risky step
     * logs before and after itself.
     *
     * OFF, because it turned out this project already documented the answer:
     * on retail hardware svcConnectToNamedPort("sm:") fails with OutOfResource
     * inside the game process, so fs never comes up and sdlog::Line writes
     * nowhere -- and sdlog::Open() is never called either.  There is NO log
     * file on the console; the only channel is the on-screen readout.
     * Kept as a switch in case that ever changes. */
    /* v378: writing the trace to the preferences file requests a game save.
     * Off -- see the note in HookGameUpdate. */
    constexpr bool kTraceFlush = false;

    constexpr bool kAutomateLog = true;
    /* v352 bisect switch: false = harvest only, no grouping / feeding /
     * collecting.  Flip back to true once this is settled. */
    constexpr bool kAutomateRun = true;
    /* v353 bisect: output (collecting a finished machine into a chest) off,
     * feeding still on.  v352 proved the crash lives in RunAutomation, and the
     * output loop is what runs first when a location loads. */
    constexpr bool kAutomateOutput = true;
    /* v355 bisect: the other half.  Output ON, feeding OFF.
     *
     * v353 (output off) ran a session without crashing while v351 (output on)
     * died on entering the cellar, and output is the only difference -- so the
     * collect path is now the prime suspect and this build tests it alone.
     * Machines must be filled by hand: with feeding off nothing refills them,
     * but a charcoal kiln finishes in ~20 real seconds, so one hand-fed kiln
     * is enough to exercise Chest.addItem + the NetSet* resets. */
    constexpr bool kAutomateInput = true;
    /* v356 bisect: the collect path is confirmed as the crash site (v355 dies
     * on entering the cellar, v353 with output off does not).  It makes only
     * two managed calls, and this switches off the second one:
     *
     *   Item.ResetParentSheetIndex() = ItemRegistry.GetDataOrErrorItem(
     *       QualifiedItemId).SpriteIndex
     *
     * That rewrites ParentSheetIndex on the machine.  For most machines it is
     * a no-op (the sprite index never changes), which is why it is safe to
     * drop for a test -- worst case a finished machine keeps its "done"
     * sprite until something else resets it. */
    constexpr bool kAutomateResetPsi = true;

    /* v340: frame counter for the harvest/consume cycle. */
    static unsigned s_automateTick = 0;
    static unsigned s_autoHb = 0;
    static void (*s_origChestUpdate)(void*, void*) = nullptr;
    static void (*s_origPondUpdate)(void*, void*) = nullptr;
    static void (*s_origObjectUpdate)(void*, void*) = nullptr;

    /* Read / write a Net* field's value.  NetValueSlot already validates both
     * hops; writing straight into it bypasses the netcode change events, which
     * is fine for single player (saves serialise the object as a whole). */
    void* NetObjValue(void* owner, uintptr_t off) {
        void* slot = NetValueSlot(owner, off, 8u);
        return slot != nullptr ? *reinterpret_cast<void**>(slot) : nullptr;
    }

    bool NetBoolValue(void* owner, uintptr_t off) {
        void* slot = NetValueSlot(owner, off, 1u);
        return slot != nullptr && *reinterpret_cast<unsigned char*>(slot) != 0;
    }

    void NetSetObj(void* owner, uintptr_t off, void* value) {
        void* slot = NetValueSlot(owner, off, 8u);
        if (slot != nullptr) {
            *reinterpret_cast<void**>(slot) = value;
        }
    }

    void NetSetBool(void* owner, uintptr_t off, bool value) {
        void* slot = NetValueSlot(owner, off, 1u);
        if (slot != nullptr) {
            *reinterpret_cast<unsigned char*>(slot) = value ? 1u : 0u;
        }
    }

    /* Plain chest (130) or stone chest (232).  Fridges (216), junimo chests
     * (256), mini shipping bins (248) and auto-loaders (275) are deliberately
     * not matched, so they keep their vanilla behaviour.
     *
     * ParentSheetIndex alone is not enough: in 1.6 it is a sprite index, and a
     * big craftable with a custom name can report something else (PC Automate
     * carries the same fallback).  The item id is authoritative. */
    bool StrEqualsAscii(const void* s, const char* want) {
        int n = 0;
        while (want[n] != '\0') {
            ++n;
        }
        if (ManagedStrLen(s) != n) {
            return false;
        }
        for (int i = 0; i < n; ++i) {
            if (ManagedStrChar(s, i) != static_cast<char16_t>(want[i])) {
                return false;
            }
        }
        return true;
    }

    bool IsPlainChest(void* obj) {
        const int idx = ChestParentSheetIndex(obj);
        if (idx == 130 || idx == 232) {
            return true;
        }
        auto getItemId = reinterpret_cast<void* (*)(void*)>(g_base + kGetItemIdOff);
        void* id = getItemId(obj);
        return StrEqualsAscii(id, "130") || StrEqualsAscii(id, "232");
    }

    /* ---- v340: registry-based grouping --------------------------------
     *
     * v339 flooded the tile grid with OverlaidDictionary.TryGetValue, one
     * managed call per tile.  On hardware the group never grew past the one
     * machine touching the chest: machines two tiles away stayed dead, and
     * the group's machine count was stuck at 1 (readout F01 = passes F,
     * output 0, input 1).  Rather than keep guessing why a per-tile lookup
     * into a locked OverlaidDictionary misbehaves, this version stops looking
     * tiles up entirely.
     *
     * GameLocation.updateWhenCurrentLocation already walks every object in
     * the current location:
     *
     *     objects.Lock();
     *     foreach (Object o in objects.Values) o.updateWhenCurrentLocation(time);
     *     objects.Unlock();
     *
     * so the two hooks below harvest what the game hands us.  Machines are
     * plain Objects and never override the method, so hooking the base
     * implementation catches them; Chest overrides it, hence the second hook.
     * Grouping is then pure native array work -- no managed calls at all.
     *
     * The registry is rebuilt only on collection frames (one in
     * kAutomateInterval) and consumed at the end of that same Game1.Update,
     * once the walk is known to be over.
     */
    constexpr int kRegMaxChests   = 64;
    constexpr int kRegMaxMachines = 160;
    /* v359: 96 -> 160.  A wire run (see kConnMax) consumes group nodes, and a
     * group that hits the cap just stops expanding -- which would silently cut
     * the far end of a path line off. */
    constexpr int kGrpMaxNodes    = 160;

    static void* s_regChests[kRegMaxChests];
    static float s_regChestX[kRegMaxChests];
    static float s_regChestY[kRegMaxChests];
    static int   s_regChestCount = 0;

    /* v371: fish ponds seen during the same object walk. */
    constexpr int kRegMaxPonds = 16;
    static void* s_regPonds[kRegMaxPonds];
    static int   s_regPondX[kRegMaxPonds];
    static int   s_regPondY[kRegMaxPonds];
    static int   s_regPondW[kRegMaxPonds];
    static int   s_regPondH[kRegMaxPonds];
    static int   s_regPondCount = 0;

    static void* s_regMachines[kRegMaxMachines];
    static float s_regMachX[kRegMaxMachines];
    static float s_regMachY[kRegMaxMachines];
    /* v342: GetMachineData() != null, evaluated ONCE per object per harvest.
     * Feeding without this test means AttemptAutoLoad runs on every big
     * craftable (signs, lamps, decorative junk) and performObjectDropInAction
     * calls GetMachineData() once PER ITEM in the chest -- dozens of managed
     * calls per object per pass.  That amplification is new in v340. */
    static unsigned char s_regIsMachine[kRegMaxMachines];
    static int   s_regMachineCount = 0;

    /* True only during the frame whose object walk we are harvesting. */
    static bool  s_collecting = false;

    /* One group's scratch.  Fixed size, no allocation. */
    static void* s_grpChests[kRegMaxChests];
    static int   s_grpChestCount = 0;
    static void* s_grpMachines[kRegMaxMachines];
    static unsigned char s_grpIsMachine[kRegMaxMachines];
    static int   s_grpMachineCount = 0;
    static float s_frX[kGrpMaxNodes];
    static float s_frY[kGrpMaxNodes];
    static int   s_markKeys[kGrpMaxNodes];
    static int   s_markCount = 0;
    static int   s_chestDone[kRegMaxChests];
    static int   s_chestDoneCount = 0;
    static unsigned s_grpNodes = 0;

    /* ---- v359: flooring as a wire -------------------------------------
     *
     * In 1.6 a path/floor is a TerrainFeature (StardewValley.TerrainFeatures.
     * Flooring), NOT an object -- so it never shows up in the object walk the
     * registry is built from, and it cannot be found by reading the object
     * dictionary.  The only way in is
     * location.terrainFeatures.TryGetValue(tile), which is a managed call per
     * tile -- exactly the shape that froze the console in v337.
     *
     * So the wire is discovered once and cached:
     *   - a rebuild pass may spend at most kConnBudget lookups
     *   - it is rebuilt when the location changes, or every kConnRebuild
     *     automation passes (~5 s), so a path placed or removed later is
     *     picked up without leaving the map
     *   - every other pass is pure native: a scan of a small int array
     *
     * Only the tile coordinates are kept, never the Flooring object, so a
     * stale entry can at worst connect the wrong tiles -- it cannot dangle. */
    constexpr int kConnMax      = 160;   /* wire tiles remembered */
    constexpr int kConnBudget   = 512;   /* terrain lookups per rebuild pass */
    constexpr int kConnRebuild  = 80;    /* passes between rebuilds (~10 s) */

    static int   s_connTiles[kConnMax];
    static int   s_connPass = 0;         /* passes since the last rebuild */
    static void* s_connLoc = nullptr;    /* location the set was built for */
    static int   s_connBudget = 0;
    /* s_connCount / s_connScan are declared next to the other readout counters,
     * because the draw code sits above this point. */

    int PackTile(float x, float y) {
        const unsigned ix = static_cast<unsigned>(static_cast<int>(x)) & 0xFFFFu;
        const unsigned iy = static_cast<unsigned>(static_cast<int>(y)) & 0xFFFFu;
        return static_cast<int>((ix << 16) | iy);
    }

    /* Returns false if this tile was already walked, or the budget is spent. */
    bool MarkTile(float x, float y) {
        if (s_markCount >= kGrpMaxNodes) {
            return false;
        }
        const int key = PackTile(x, y);
        for (int i = 0; i < s_markCount; ++i) {
            if (s_markKeys[i] == key) {
                return false;
            }
        }
        s_markKeys[s_markCount++] = key;
        return true;
    }

    void PushTile(int& tail, float x, float y) {
        if (tail >= kGrpMaxNodes) {
            return;
        }
        s_frX[tail] = x;
        s_frY[tail] = y;
        ++tail;
    }

    void* GetMachineDataOf(void* machine) {
        return reinterpret_cast<void* (*)(void*)>(g_base + kGetMachineDataOff)(machine);
    }

    /* Tappers need Tree.UpdateTapperProduct after collection, otherwise they
     * sit empty until Tree.DayUpdate re-primes them the next morning.  Vanilla
     * tests HasContextTag("tapper"); a sprite test is the same answer for the
     * two tapper types and costs no managed call -- this runs twice per machine
     * per pass, so it is the hottest test in the loop. */
    bool IsTapperBySprite(void* obj) {
        const int idx = ChestParentSheetIndex(obj);
        return idx == 105 || idx == 264;
    }

    /* location.terrainFeatures -> the inner Dictionary<Vector2,TerrainFeature>.
     * Null when it cannot be read.  Same two hops the tapper code uses. */
    void* TerrainDict() {
        void* loc = reinterpret_cast<void* (*)()>(g_base + kGame1CurrentLocOff)();
        if (loc == nullptr || !Plausible(loc) ||
            !RangeReadable(reinterpret_cast<unsigned char*>(loc) + kTapperTerrainOff, 8u)) {
            return nullptr;
        }
        void* netDict = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(loc) + kTapperTerrainOff);
        if (netDict == nullptr ||
            !RangeReadable(reinterpret_cast<unsigned char*>(netDict) + kTapperDictOff, 8u)) {
            return nullptr;
        }
        return *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(netDict) + kTapperDictOff);
    }

    /* Is this TerrainFeature a path/floor?
     *
     * Same problem as the tree test below: `f is Flooring` needs an EEType we
     * cannot obtain, so the first positive answer comes from
     * Object.ToString() (the full type name) and its EEType is cached.  Every
     * later call is then a single pointer compare -- which matters, because
     * this runs inside the wire rebuild.
     *
     * A wrong answer here is harmless in the "false" direction (that tile just
     * does not bridge).  A wrong answer in the "true" direction would let grass
     * or dirt act as wire, so the name test is exact: ".Flooring", which no
     * other TerrainFeature ends with. */
    /* v367: the class NAME, as a bare UTF-16 C-string in .rodata.
     *
     * tools/aot_meta.py reads names with metadata_name(klass + CLASS_NAME) and
     * documents that "the pointer already lands on the character data", UTF-16.
     * So the whole test is: object -> klass (first word) -> klass+0x30 -> chars.
     * No managed string, no vtable slot, no size field -- and the two earlier
     * attempts (size at klass+0 read back 0xEB80; the vtable slot read back
     * 0x3D90) are why this is now the only mechanism. */
    /* v368: go back to the MANAGED string, which is the one thing that is
     * proven to read correctly here (the backpack/pickaxe code has used
     * ManagedStrLen/ManagedStrChar on hardware for days), and fix the TEST
     * instead: Object.ToString() returns the assembly-qualified name
     * ("StardewValley.TerrainFeatures.Flooring, StardewValley, Version=...,
     * Culture=neutral, PublicKeyToken=null"), which is why a ".Flooring"
     * SUFFIX never matched.  A PREFIX does.
     *
     * The bare UTF-16 name at klass+0x30 read back 0000 on hardware, so that
     * route is out too. */
    bool ManagedStrStartsWithAscii(const void* s, const char* prefix) {
        int n = 0;
        while (prefix[n] != '\0') {
            ++n;
        }
        if (ManagedStrLen(s) < n) {
            return false;
        }
        for (int i = 0; i < n; ++i) {
            if (ManagedStrChar(s, i) !=
                static_cast<char16_t>(static_cast<unsigned char>(prefix[i]))) {
                return false;
            }
        }
        return true;
    }

    bool FeatureNameIs(void* feature, const char* prefix) {
        auto toStr = reinterpret_cast<void* (*)(void*)>(g_base + kObjToStringOff);
        return ManagedStrStartsWithAscii(toStr(feature), prefix);
    }

    /* v369: the dictionary value may be a NetRef wrapper, not the feature.
     *
     * Hardware read the value's type name and got 0x004E = 'N', with a length
     * of 134 -- "Netcode.NetRef`1[StardewValley.TerrainFeatures.TerrainFeature],
     * StardewValley, Version=..., Culture=neutral, PublicKeyToken=null" is
     * exactly that long and starts with N.  terrainFeatures is a
     * NetVector2Dictionary<TerrainFeature, NetRef<TerrainFeature>>, so the
     * inner dictionary it looks up in maps to the wrapper.
     *
     * So: if the value is not a StardewValley.TerrainFeatures.* object, follow
     * the Net* value slot (+0x68, the same kNetValueOff every other Net field
     * in this project uses) and check again.  If that does not help either,
     * return the original pointer -- callers only ever compare types, so a
     * wrong answer stays a no-op. */
    void* UnwrapFeature(void* raw) {
        if (raw == nullptr) {
            return nullptr;
        }
        /* Learned once.  1 = the dictionary hands back NetRef wrappers (what
         * 1.6.15.13 does), 2 = bare features. */
        if (s_featureWrapped == 1) {
            if (!RangeReadable(reinterpret_cast<unsigned char*>(raw) + kNetValueOff, 8u)) {
                return raw;
            }
            void* inner = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(raw) + kNetValueOff);
            return inner != nullptr ? inner : raw;
        }
        if (s_featureWrapped == 2) {
            return raw;
        }
        if (FeatureNameIs(raw, "StardewValley.TerrainFeatures.")) {
            s_featureWrapped = 2;
            return raw;
        }
        if (RangeReadable(reinterpret_cast<unsigned char*>(raw) + kNetValueOff, 8u)) {
            void* inner = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(raw) + kNetValueOff);
            if (inner != nullptr && RangeReadable(inner, 8u) &&
                FeatureNameIs(inner, "StardewValley.TerrainFeatures.")) {
                s_featureWrapped = 1;
                return inner;
            }
        }
        s_featureWrapped = 2;
        return raw;
    }

    bool IsFlooringFeature(void* feature) {
        if (!RangeReadable(feature, 8u)) {
            return false;
        }
        void* klass = *reinterpret_cast<void**>(feature);
        if (s_flooringKlass != nullptr) {
            return klass == s_flooringKlass;    /* no managed call at all */
        }
        if (!FeatureNameIs(feature, "StardewValley.TerrainFeatures.Flooring")) {
            return false;
        }
        s_flooringKlass = klass;
        return true;
    }

    /* v362: look up the tile the player is standing on.  Deliberately bypasses
     * the flood, the budget and the cache -- it answers one question: does the
     * terrain dictionary see a Flooring where I am standing? */
    void ProbePlayerTile() {
        s_probeState = 0;
        void* pl = CarryPlayer();
        void* dict = TerrainDict();
        if (pl == nullptr || dict == nullptr) {
            return;
        }
        auto tileX = reinterpret_cast<int (*)(void*)>(g_base + kCharTileXOff);
        auto tileY = reinterpret_cast<int (*)(void*)>(g_base + kCharTileYOff);
        void* feature = nullptr;
        auto tryGet = reinterpret_cast<bool (*)(void*, float, float, void**)>(
            g_base + kTapperTryGetOff);
        if (!tryGet(dict, static_cast<float>(tileX(pl)),
                    static_cast<float>(tileY(pl)), &feature) ||
            feature == nullptr) {
            return;                 /* 0: nothing on this tile at all */
        }
        feature = UnwrapFeature(feature);
        s_probeState = IsFlooringFeature(feature) ? 3u : 1u;
    }

    /* Does this tile carry a wire?  Native when the set is already built. */
    bool ConnectorAt(void* tfDict, float x, float y) {
        const int key = PackTile(x, y);
        for (int i = 0; i < s_connCount; ++i) {
            if (s_connTiles[i] == key) {
                return true;
            }
        }
        if (!s_connScan || s_connBudget <= 0 || tfDict == nullptr) {
            return false;
        }
        --s_connBudget;
        if (s_connLookups < 0xFFFFu) {
            ++s_connLookups;
        }
        void* feature = nullptr;
        auto tryGet = reinterpret_cast<bool (*)(void*, float, float, void**)>(
            g_base + kTapperTryGetOff);
        if (!tryGet(tfDict, x, y, &feature) || feature == nullptr) {
            return false;
        }
        feature = UnwrapFeature(feature);
        if (s_connFound < 0xFu) {
            ++s_connFound;
        }
        if (!IsFlooringFeature(feature)) {
            return false;
        }
        if (s_connHits < 0xFFFFu) {
            ++s_connHits;
        }
        if (s_connCount < kConnMax) {
            s_connTiles[s_connCount++] = key;
        }
        return true;
    }

    /* Is this TerrainFeature a Tree?
     *
     * Vanilla's test is `f is Tree`, which is an EEType compare against a
     * handle we have no way to obtain.  Calling UpdateTapperProduct on a Bush
     * or a HoeDirt instead would read and write past the end of that object
     * (Tree is 0xE8 bytes, TerrainFeature only 0x30), so the test cannot be
     * skipped.
     *
     * Object.ToString() is GetType().ToString(), i.e. the full type name, and
     * no TerrainFeature subclass overrides it.  A name ending in ".Tree" is a
     * Tree; "FruitTree" ends in "tTree" and is correctly rejected -- which
     * matters, because vanilla only re-primes wild trees too.
     *
     * If the runtime ever returns something unexpected here the answer is just
     * "not a tree": the tapper then falls back to the day-update re-prime,
     * which is the behaviour this version would have had without any of it. */
    bool IsTreeFeature(void* feature) {
        if (!RangeReadable(feature, 8u)) {
            return false;
        }
        void* klass = *reinterpret_cast<void**>(feature);
        if (s_treeKlass != nullptr) {
            return klass == s_treeKlass;
        }
        if (!FeatureNameIs(feature, "StardewValley.TerrainFeatures.Tree")) {
            return false;
        }
        s_treeKlass = klass;
        return true;
    }

    /* Give a just-emptied tapper its next product, exactly as vanilla does
     * after a manual collection.
     *
     * previousOutput is deliberately null instead of the item that was just
     * stored: Chest.addItem merges into an existing stack by calling
     * ConsumeStack, and when the whole stack is absorbed nothing in the managed
     * heap references that item any more.  The only reference left would be
     * this native frame, which the GC cannot see (the v331 lesson), so passing
     * it on would be a dangling pointer.  Null is what Tree.DayUpdate's own
     * re-prime passes, so this is a shape the game already runs. */
    void ReprimeTapper(void* tapper) {
        void* dict = TerrainDict();
        if (dict == nullptr) {
            return;
        }
        void* slot = NetValueSlot(tapper, kObjTileLocOff, 8u);
        if (slot == nullptr) {
            return;
        }
        const float tx = *reinterpret_cast<float*>(slot);
        const float ty = *(reinterpret_cast<float*>(slot) + 1);

        void* feature = nullptr;
        auto tryGet = reinterpret_cast<bool (*)(void*, float, float, void**)>(
            g_base + kTapperTryGetOff);
        if (!tryGet(dict, tx, ty, &feature) || feature == nullptr) {
            return;                 /* tapper not on a terrain feature any more */
        }
        /* v369: the value is a NetRef wrapper; calling UpdateTapperProduct on
         * that would write Tree fields into a NetRef object. */
        feature = UnwrapFeature(feature);
        if (!IsTreeFeature(feature)) {
            return;
        }
        auto update = reinterpret_cast<void (*)(void*, void*, void*, bool)>(
            g_base + kTapperUpdateOff);
        update(feature, tapper, nullptr, false);
    }

    /* v346: record where the automation is, so a crash leaves a trace in the
     * preferences file.  Codes:
     *   11 run      12 pass
     *   21 out try  22 out stored  23 out reset ok  24 out chest full
     *   31 in try   32 in ok
     * Low 16 bits carry the machine's sprite index. */
    void TraceStep(unsigned code, void* machine) {
        const int psi = (machine != nullptr) ? ChestParentSheetIndex(machine) : 0;
        /* v348 layout: A5 | step | sprite | counter.
         *
         * The sprite moved OUT of the low bits on purpose: the game increments
         * timesPlayed once per launch, so the low byte comes back one higher
         * than we wrote it (v347 measured A5120000 -> A5120001).  With the
         * counter down there the drift is harmless, and the sprite stays
         * readable in bits 8-15. */
        s_trace = kTraceMagic | ((code & 0xFFu) << 16) |
                  ((static_cast<unsigned>(psi) & 0xFFu) << 8) |
                  (s_traceSeq & 0xFFu);
        ++s_traceSeq;
        s_traceDirty = 1;
        s_traceHot = 1;
    }

    /* An idle marker (run / pass).  These fire every cycle, so they must never
     * overwrite an action and must never ask for a write.
     *
     * Both bugs were measured:
     *   v347  a pass overwrote every action 130 ms later
     *   v349  "run" did the same, and the file only ever held A511 -- the live
     *         readout sat on A511 while the automation was clearly running.
     *
     * One baseline write per session is allowed so the file does not keep the
     * previous session's action and get misread as this one's. */
    void TraceIdle(unsigned code) {
        const unsigned cur = (s_trace >> 16) & 0xFFu;
        if (s_trace != 0 && cur != 0x11u && cur != 0x12u) {
            return;                 /* keep the action */
        }
        s_trace = kTraceMagic | ((code & 0xFFu) << 16) | (s_traceSeq & 0xFFu);
        if (!s_traceBaseline) {
            s_traceBaseline = 1;
            s_traceDirty = 1;
        }
    }

    void RegAddChest(void* obj) {
        if (s_regChestCount >= kRegMaxChests) {
            return;
        }
        for (int i = 0; i < s_regChestCount; ++i) {
            if (s_regChests[i] == obj) {
                return;             /* the walk visits each object once */
            }
        }
        void* slot = NetValueSlot(obj, kObjTileLocOff, 8u);
        if (slot == nullptr) {
            return;
        }
        const float x = *reinterpret_cast<float*>(slot);
        const float y = *(reinterpret_cast<float*>(slot) + 1);
        if (x == 0.0f && y == 0.0f) {
            return;                 /* not placed on the map */
        }
        s_regChests[s_regChestCount] = obj;
        s_regChestX[s_regChestCount] = x;
        s_regChestY[s_regChestCount] = y;
        ++s_regChestCount;
    }

    void RegAddMachine(void* obj) {
        if (s_regMachineCount >= kRegMaxMachines) {
            return;
        }
        for (int i = 0; i < s_regMachineCount; ++i) {
            if (s_regMachines[i] == obj) {
                return;
            }
        }
        void* slot = NetValueSlot(obj, kObjTileLocOff, 8u);
        if (slot == nullptr) {
            return;
        }
        const float x = *reinterpret_cast<float*>(slot);
        const float y = *(reinterpret_cast<float*>(slot) + 1);
        if (x == 0.0f && y == 0.0f) {
            return;
        }
        s_regMachines[s_regMachineCount] = obj;
        s_regMachX[s_regMachineCount] = x;
        s_regMachY[s_regMachineCount] = y;
        s_regIsMachine[s_regMachineCount] =
            GetMachineDataOf(obj) != nullptr ? 1u : 0u;
        ++s_regMachineCount;
    }

    /* Machine candidates are simply "big craftable that is not one of our
     * chests".  GetMachineData would be the precise test, but it costs a
     * managed call (and a qualified-id string) per object, and a machine the
     * game does not know about is harmless: AttemptAutoLoad just declines.
     * Being permissive here is what keeps the charcoal kiln working even if
     * its Data/Machines entry is not what we expect. */
    void RegHarvest(void* obj) {
        if (!NetBoolValue(obj, kObjBigCraftOff)) {
            return;                 /* pure native reject: most objects */
        }
        /* v341: ParentSheetIndex only -- NO managed call here.
         * IsPlainChest falls back to Item.get_ItemId (a managed call) for
         * anything that is not 130/232, and this runs once per big craftable
         * per harvest frame.  Chests are registered by the Chest hook (which
         * still uses the full IsPlainChest), so nothing is lost by keeping the
         * walk entirely native. */
        const int idx = ChestParentSheetIndex(obj);
        if (idx == 130 || idx == 232) {
            RegAddChest(obj);
            return;
        }
        RegAddMachine(obj);
    }

    /* Put `item` into the first group chest that takes all of it. */
    bool StoreInGroup(void* item, int chestCount) {
        auto addItem = reinterpret_cast<void* (*)(void*, void*)>(g_base + kChestAddItemOff);
        for (int i = 0; i < chestCount; ++i) {
            if (addItem(s_grpChests[i], item) == nullptr) {
                return true;
            }
        }
        return false;
    }

    /* Offer the group's storage to one machine.  Vanilla's AttemptAutoLoad
     * walks the inventory and stops at the first item the machine accepts, so
     * each chest has to be offered separately (they are separate IInventory). */
    void FeedFromGroup(void* machine, void* pl, int chestCount) {
        auto isEmpty = reinterpret_cast<bool (*)(void*)>(g_base + kChestIsEmptyOff);
        auto getItems = reinterpret_cast<void* (*)(void*)>(g_base + kChestGetItemsOff);
        auto autoLoad = reinterpret_cast<bool (*)(void*, void*, void*)>(g_base + kAutoLoadOff);
        TraceStep(0x31u, machine);
        if (kAutomateLog) {
            LOGF("auto: in try psi=%d", ChestParentSheetIndex(machine));
        }
        for (int i = 0; i < chestCount; ++i) {
            if (isEmpty(s_grpChests[i])) {
                continue;
            }
            void* inv = getItems(s_grpChests[i]);
            if (inv == nullptr || !Plausible(inv)) {
                continue;
            }
            if (autoLoad(machine, inv, pl)) {
                TraceStep(0x32u, machine);
                if (s_autoInput < 0xFu) {
                    ++s_autoInput;
                }
                if (kAutomateLog) {
                    LOGF("auto: in ok psi=%d", ChestParentSheetIndex(machine));
                }
                return;
            }
        }
    }

    bool ChestAlreadyGrouped(int idx) {
        for (int i = 0; i < s_chestDoneCount; ++i) {
            if (s_chestDone[i] == idx) {
                return true;
            }
        }
        return false;
    }

    /* Flood outwards from one chest over the harvested registry.  Only tiles
     * that hold a chest or a machine carry the walk further, which is exactly
     * PC Automate's "connected through machines and chests" rule. */
    void BuildGroup(int rootIdx, void* tfDict) {
        s_grpChestCount = 0;
        s_grpMachineCount = 0;
        s_markCount = 0;
        s_grpNodes = 0;

        const float cx = s_regChestX[rootIdx];
        const float cy = s_regChestY[rootIdx];

        MarkTile(cx, cy);
        s_grpChests[s_grpChestCount++] = s_regChests[rootIdx];
        if (s_chestDoneCount < kRegMaxChests) {
            s_chestDone[s_chestDoneCount++] = rootIdx;
        }

        int head = 0;
        int tail = 0;
        PushTile(tail, cx + 1.0f, cy);
        PushTile(tail, cx - 1.0f, cy);
        PushTile(tail, cx, cy + 1.0f);
        PushTile(tail, cx, cy - 1.0f);

        while (head < tail) {
            const float x = s_frX[head];
            const float y = s_frY[head];
            ++head;

            if (!MarkTile(x, y)) {
                continue;
            }

            bool bridge = false;
            for (int i = 0; i < s_regChestCount; ++i) {
                if (s_regChestX[i] != x || s_regChestY[i] != y) {
                    continue;
                }
                bridge = true;
                if (ChestAlreadyGrouped(i)) {
                    continue;
                }
                if (s_chestDoneCount < kRegMaxChests) {
                    s_chestDone[s_chestDoneCount++] = i;
                }
                if (s_grpChestCount < kRegMaxChests) {
                    s_grpChests[s_grpChestCount++] = s_regChests[i];
                }
            }
            for (int i = 0; i < s_regMachineCount; ++i) {
                if (s_regMachX[i] != x || s_regMachY[i] != y) {
                    continue;
                }
                bridge = true;
                if (s_grpMachineCount < kRegMaxMachines) {
                    s_grpMachines[s_grpMachineCount] = s_regMachines[i];
                    s_grpIsMachine[s_grpMachineCount] = s_regIsMachine[i];
                    ++s_grpMachineCount;
                }
            }

            if (!bridge) {
                /* Nothing placed here -- the only thing that can still carry the
                 * group is a path/floor (v359).  Native on every pass that is
                 * not a rebuild; on a rebuild this may cost one managed lookup
                 * per tile, capped by kConnBudget for the whole pass. */
                bridge = ConnectorAt(tfDict, x, y);
            }
            if (!bridge) {
                continue;           /* empty tile / grass / anything else */
            }
            ++s_grpNodes;

            PushTile(tail, x + 1.0f, y);
            PushTile(tail, x - 1.0f, y);
            PushTile(tail, x, y + 1.0f);
            PushTile(tail, x, y - 1.0f);
        }
    }

    /* Defined below with the pond hook; RunAutomation is above it. */
    void CollectPonds();

    void RunAutomation() {
        s_dbgRegChests = static_cast<unsigned>(s_regChestCount);
        s_dbgRegMachines = static_cast<unsigned>(s_regMachineCount);

        /* v359: the wire set.  Decided first, before any early return, so
         * s_connScan can never be left true from a pass that bailed out --
         * that would let a later pass spend lookups without a fresh budget. */
        void* tfDict = TerrainDict();
        void* locNow = reinterpret_cast<void* (*)()>(g_base + kGame1CurrentLocOff)();
        if (tfDict != nullptr &&
            (locNow != s_connLoc || s_connPass >= kConnRebuild)) {
            s_connLoc = locNow;
            s_connPass = 0;
            s_connCount = 0;
            s_connScan = true;
            s_connBudget = kConnBudget;
            if (s_connRebuilds < 0xFFFFu) {
                ++s_connRebuilds;
            }
        }
        else {
            if (s_connPass < 0x10000) {
                ++s_connPass;
            }
            s_connScan = false;
        }

        /* v362: refresh the player-tile probe once per pass.  One terrain
         * lookup every 8 frames -- it costs nothing and it works even when
         * there is nothing to automate, which is exactly when it is needed. */
        ProbePlayerTile();

        /* v392: read-only probe -- the full 32-bit value of disconnectDay.
         *
         * Row 4 shows the raw NetInt value (8 hex digits) so one glance answers
         * all three questions:
         *   FEFFFEFF  untouched default        -> nothing of ours has landed
         *   5A5A0xxx  our magic + mask         -> the write stuck in RAM
         *   anything else                      -> the game wrote the field
         *
         * Before sleep: 5A5A0xxx means the write works.
         * After reload: 5A5A0xxx means the farm save carried it.
         *                FEFFFEFF means the write works but the save does not
         *                carry the field (then disconnectDay is not the answer). */
        s_dbgDisc = 0xFFFFFFFFu;
        {
            void* pl0 = CarryPlayer();
            if (pl0 != nullptr &&
                RangeReadable(reinterpret_cast<unsigned char*>(pl0) +
                                  kFarmerDiscDayOff, 8u)) {
                void* wr = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(pl0) + kFarmerDiscDayOff);
                if (wr != nullptr &&
                    RangeReadable(reinterpret_cast<unsigned char*>(wr) + 0x6Bu, 1u)) {
                    s_dbgDisc = *reinterpret_cast<unsigned*>(
                        reinterpret_cast<unsigned char*>(wr) + 0x68u);
                }
            }
        }

        if (s_regChestCount == 0 || s_regMachineCount == 0) {
            return;
        }
        void* pl = CarryPlayer();
        if (pl == nullptr) {
            return;
        }

        auto resetPsi = reinterpret_cast<void (*)(void*)>(g_base + kItemResetPsiOff);

        s_chestDoneCount = 0;

        TraceIdle(0x12u);
        if (kAutomateLog) {
            LOGF("auto: pass c=%d m=%d", s_regChestCount, s_regMachineCount);
        }

        for (int c = 0; c < s_regChestCount; ++c) {
            if (ChestAlreadyGrouped(c)) {
                continue;
            }
            BuildGroup(c, tfDict);
            if (s_grpMachineCount == 0 || s_grpChestCount == 0) {
                continue;
            }
            s_dbgGrpChests = static_cast<unsigned>(s_grpChestCount);
            s_dbgGrpMachines = static_cast<unsigned>(s_grpMachineCount);
            s_dbgGrpNodes = s_grpNodes;

            /* Output first, so a machine that finished this tick is emptied
             * before anything new is pushed into it. */
            for (int m = 0; kAutomateOutput && m < s_grpMachineCount; ++m) {
                void* machine = s_grpMachines[m];
                void* held = NetObjValue(machine, kObjHeldOff);
                if (held == nullptr || !NetBoolValue(machine, kObjReadyOff)) {
                    continue;
                }
                /* v358: tappers are collected now.  They need
                 * Tree.UpdateTapperProduct afterwards to be re-primed, which
                 * happens below once the machine has been reset -- same order
                 * as vanilla's checkForAction. */
                const bool isTapper = IsTapperBySprite(machine);
                TraceStep(0x21u, machine);
                if (kAutomateLog) {
                    LOGF("auto: out try psi=%d grp=%d/%d",
                         ChestParentSheetIndex(machine), s_grpChestCount,
                         s_grpMachineCount);
                }
                if (!StoreInGroup(held, s_grpChestCount)) {
                    TraceStep(0x24u, machine);
                    if (kAutomateLog) {
                        LOGF("auto: out full psi=%d",
                             ChestParentSheetIndex(machine));
                    }
                    continue;       /* every chest full: leave it in the machine */
                }
                TraceStep(0x22u, machine);
                if (kAutomateLog) {
                    LOGF("auto: out stored psi=%d",
                         ChestParentSheetIndex(machine));
                }
                NetSetObj(machine, kObjHeldOff, nullptr);
                /* v357 FIX -- this is the crash.
                 *
                 * Writing the NetRef value slot directly bypasses
                 * heldObject's fieldChangeVisibleEvent, whose only job is
                 *
                 *     _hasHeldObject = heldObject.Value != null;   (Object.cs:823)
                 *
                 * and Object.updateWhenCurrentLocation -- the very function our
                 * harvest hook rides on -- then does
                 *
                 *     if (_hasHeldObject) {
                 *         Object obj = heldObject.Get();            (Object.cs:2679)
                 *         if (obj.QualifiedItemId == "(O)913" ...)
                 *
                 * With the flag stale at true and the value null, that is an
                 * immediate NullReferenceException, i.e. an unhandled managed
                 * exception, i.e. the "software was closed" abort.  It fires on
                 * the machine's next update, which is why entering a location
                 * full of freshly collected machines killed it instantly.
                 *
                 * _hasHeldObject is a plain bool (not a NetBool), so it has to
                 * be written as a raw byte at +0x208 -- NetSetBool would treat
                 * the bool's value as a pointer to a wrapper. */
                if (RangeReadable(
                        reinterpret_cast<unsigned char*>(machine) + kHeldCacheOff,
                        1u)) {
                    *reinterpret_cast<unsigned char*>(
                        reinterpret_cast<unsigned char*>(machine) + kHeldCacheOff) = 0;
                }
                NetSetBool(machine, kObjReadyOff, false);
                NetSetBool(machine, kObjShowNextOff, false);
                if (kAutomateResetPsi) {
                    resetPsi(machine);
                }
                if (isTapper) {
                    /* Same slot as vanilla: after the reset, so the new product
                     * is not immediately wiped by the reset above. */
                    ReprimeTapper(machine);
                }
                TraceStep(0x23u, machine);
                if (kAutomateLog) {
                    LOGF("auto: out reset ok");
                }
                if (s_autoOutput < 0xFu) {
                    ++s_autoOutput;
                }
            }

            /* Then feed whatever is empty again. */
            for (int m = 0; kAutomateInput && m < s_grpMachineCount; ++m) {
                if (s_grpIsMachine[m] == 0) {
                    continue;       /* not a machine: no point offering items */
                }
                void* machine = s_grpMachines[m];
                if (NetObjValue(machine, kObjHeldOff) != nullptr) {
                    continue;
                }
                if (IsTapperBySprite(machine)) {
                    continue;       /* tappers take no input; their product is
                                     * produced by the tree, not by a recipe */
                }
                /* v351 skipped the charcoal kiln here.  That test is over (it
                 * still crashed), so the skip is gone -- leaving it in would
                 * hide the kiln from the input path and make this bisect lie.
                 * It also explained the "both traces are kilns" red herring:
                 * 9 of the 11 machines on that farm are kilns. */
                FeedFromGroup(machine, pl, s_grpChestCount);
            }

            if (s_autoPasses < 0xFu) {
                ++s_autoPasses;
            }
            if (s_autoGroups < 0xFu) {
                ++s_autoGroups;
            }
        }
        /* v371: ponds are not part of the machine groups -- they are buildings,
         * collected straight into a touching chest. */
        CollectPonds();

        if (kAutomateLog) {
            LOGF("auto: pass end");
        }
    }

    /* Called from HookGameUpdate AFTER Game1.Update returned, i.e. after the
     * location finished walking its objects.  The frame that follows a
     * rollover is the one we harvest; this frame is the one we consume. */
    void AutomateTick() {
        if (!AutomateOn()) {
            /* Switch off: drop any half-built registry so turning it back on
             * starts from a clean pass. */
            s_collecting = false;
            return;
        }
        if (s_collecting) {
            s_collecting = false;
            TraceIdle(0x11u);
            if (kAutomateLog) {
                LOGF("auto: run c=%d m=%d", s_regChestCount, s_regMachineCount);
            }
            /* v352 bisect: the crash now reproduces within seconds of entering
             * the cellar, so the split can be cheap.  This build still harvests
             * (the Object/Chest hooks run, the registry fills) but never calls
             * RunAutomation.  If the cellar still kills it, the automation
             * logic is innocent and the harvest hook -- or something else
             * entirely -- is responsible. */
            if (kAutomateRun) {
                RunAutomation();
            }
        }
        if (++s_automateTick >= static_cast<unsigned>(kAutomateInterval)) {
            s_automateTick = 0;
            s_regChestCount = 0;
            s_regMachineCount = 0;
            s_regPondCount = 0;
            s_collecting = true;
            /* Written BEFORE the object walk of the next frame.  If the log
             * ends on this line the crash was inside the walk (our harvest
             * hook); if it ends on "pass end" it was in the game's own update
             * after us; if it ends on "run"/"out"/"in" it was in RunAutomation. */
            if (kAutomateLog) {
                LOGF("auto: hb %u", ++s_autoHb);
            }
        }
    }

    /* ---- Chest.updateWhenCurrentLocation(this, gameTime) ---- */
    void HookChestUpdate(void* self, void* gameTime) {
        if (AutomateOn() && s_collecting && self != nullptr && IsPlainChest(self)) {
            RegAddChest(self);
        }
        if (s_origChestUpdate != nullptr) {
            s_origChestUpdate(self, gameTime);
        }
    }

    /* ---- v371: fish ponds ---- */
    void RegAddPond(void* pond) {
        if (s_regPondCount >= kRegMaxPonds) {
            return;
        }
        for (int i = 0; i < s_regPondCount; ++i) {
            if (s_regPonds[i] == pond) {
                return;
            }
        }
        void* tx = NetValueSlot(pond, kBuildingTileXOff, 4u);
        void* ty = NetValueSlot(pond, kBuildingTileYOff, 4u);
        void* tw = NetValueSlot(pond, kBuildingWideOff, 4u);
        void* th = NetValueSlot(pond, kBuildingHighOff, 4u);
        if (tx == nullptr || ty == nullptr || tw == nullptr || th == nullptr) {
            return;
        }
        s_regPonds[s_regPondCount] = pond;
        s_regPondX[s_regPondCount] = *reinterpret_cast<int*>(tx);
        s_regPondY[s_regPondCount] = *reinterpret_cast<int*>(ty);
        s_regPondW[s_regPondCount] = *reinterpret_cast<int*>(tw);
        s_regPondH[s_regPondCount] = *reinterpret_cast<int*>(th);
        ++s_regPondCount;
    }

    /* ---- FishPond.Update(this, gameTime) ---- */
    void HookFishPondUpdate(void* self, void* gameTime) {
        if (AutomateOn() && s_collecting && self != nullptr) {
            RegAddPond(self);
        }
        if (s_origPondUpdate != nullptr) {
            s_origPondUpdate(self, gameTime);
        }
    }

    /* Vanilla (FishPond.doAction) does exactly this, with the player instead of
     * a chest:
     *     Item value = output.Value;  output.Value = null;
     *     if (who.addItemToInventoryBool(value)) ... else output.Value = value;
     * so a chest that refuses the item leaves the pond output in place. */
    void CollectPonds() {
        if (s_regPondCount == 0) {
            return;
        }
        auto addItem = reinterpret_cast<void* (*)(void*, void*)>(g_base + kChestAddItemOff);
        for (int p = 0; p < s_regPondCount; ++p) {
            void* slot = NetValueSlot(s_regPonds[p], kPondOutputOff, 8u);
            if (slot == nullptr) {
                continue;
            }
            void* item = *reinterpret_cast<void**>(slot);
            if (item == nullptr) {
                continue;               /* nothing ready */
            }
            /* A chest touching the pond's footprint, not its corner. */
            void* chest = nullptr;
            const int x0 = s_regPondX[p];
            const int y0 = s_regPondY[p];
            const int w = s_regPondW[p];
            const int h = s_regPondH[p];
            for (int c = 0; c < s_regChestCount; ++c) {
                const int cx = static_cast<int>(s_regChestX[c]);
                const int cy = static_cast<int>(s_regChestY[c]);
                const bool side = (cy >= y0 && cy < y0 + h) &&
                                  (cx == x0 - 1 || cx == x0 + w);
                const bool above = (cx >= x0 && cx < x0 + w) &&
                                   (cy == y0 - 1 || cy == y0 + h);
                if (side || above) {
                    chest = s_regChests[c];
                    break;
                }
            }
            if (chest == nullptr) {
                continue;               /* no chest against this pond */
            }
            if (addItem(chest, item) != nullptr) {
                continue;               /* chest full: output stays in the pond */
            }
            *reinterpret_cast<void**>(slot) = nullptr;
            if (s_autoOutput < 0xFu) {
                ++s_autoOutput;
            }
        }
    }

    /* ---- Object.updateWhenCurrentLocation(this, gameTime) ----
     * Every machine is a plain Object, so this is where they all pass. */
    void HookObjectUpdate(void* self, void* gameTime) {
        if (AutomateOn() && s_collecting && self != nullptr) {
            RegHarvest(self);
        }
        if (s_origObjectUpdate != nullptr) {
            s_origObjectUpdate(self, gameTime);
        }
    }

    /* ================= v248: tractor, stage 1 =========================
     *
     * Goal of stage 1 is only "summon something rideable and ride it".  No
     * tilling, no garage, no custom art -- the horse sprite is fine for now.
     *
     * The key fact is that the PC mod's tractor is not a custom class at all:
     *
     *     TractorMod/ModEntry.cs:180
     *     val = new Horse(item.HorseId, (int)tile.X, (int)tile.Y);
     *
     * It is the game's own StardewValley.Characters.Horse.  Rideable entity,
     * mount animation, movement, collision and save serialisation all already
     * exist.  Everything below is verified against E7F84509:
     *
     *   Horse(Guid, int xTile, int yTile)   0x12317F0
     *       prologue: w22=w4 w23=w3 x19=x2 x21=x0 x20=x1
     *       0x12318D0 ldr x8,[x21,#1176]   ; horseId (NetGuid at +0x498)
     *       0x12318D4 mov x1,x20           ; Guid low 64
     *       0x12318D8 mov x2,x19           ; Guid high 64
     *       0x12318F0 blr x10              ; NetGuid.set_Value
     *     so: x0=this (pass 0 -> the runtime allocates, same trick as
     *     ClickableComponent..ctor 0x175A0D0), x1:x2 = Guid, w3/w4 = tiles.
     *
     *   Horse fields (from .tmp/meta_horse.json, docs/offsets/meta_horse.json)
     *       horseId +0x498, netRider +0x4A0, ownerId +0x4A8,
     *       mounting +0x4B0, dismounting +0x4B8
     *     every Net* keeps its value at +0x68 (re-confirmed by get_HorseId's
     *     `ldp x0, x1, [x8, #104]`, 104 = 0x68).
     *
     *   Mounting does NOT need checkAction 0x1234EB0 -- that one runs a
     *   mutex.RequestLock delegate and can pop a NamingMenu.  Horse.update
     *   0x1232010 already does everything when `mounting` is set and `rider`
     *   is filled in:
     *       if (mounting.Value) { if (rider == null || !rider.IsLocalPlayer) return;
     *           ... pull the rider onto the horse ...
     *           currentLocation.characters.Remove(this);
     *           rider.mount = this; mounting.Value = false; ... }
     *
     *   Game1.getFarm          0x1464FF0   static, no args
     *   Game1.get_currentLocation 0x14407B0 static, no args
     *   NetRef<T>.set_Value    0x198A050   (netRef, value) -- set_rider 0x1231990
     *                                      is literally `ldr x0,[x0,#1184]` + tail
     *                                      call to this, so the signature is exact
     *   Farmer.netMount        0x740
     *   Game1.pressUseToolButton 0x147BDE0  `bool ()` -- zero args, and its first
     *                                      instruction is `sub sp,sp,#0x190`, i.e.
     *                                      safe to hook (unlike GameMenu.draw's
     *                                      pre-indexed store).
     *
     * TRIGGER (temporary, stage 1): on the farm, not already riding, and the
     * current tool is null (empty hand / holding a non-tool), then pressing the
     * use-tool button spawns one tractor and mounts it.  Once per session, so a
     * misfire costs one restart and never a pile of horses.
     *
     * WHY "current tool is null": the ideal gate is "holding the scythe", but
     * that needs the tool's identity (a MethodTable compare or a string
     * compare), and neither is verified yet.  An empty hand on the farm is the
     * cheapest gate that a normal player will not hit by accident.
     */
    constexpr bool kTractor = true;
    /* v256/v257 bisect switches for the overnight-save hang.
     *
     * Deliberately `static const` and not `constexpr`: a constexpr false would
     * fold the install branches away in the front end and leave
     * HookPressUseTool / HookTilesAffected / HookGameUpdate unreferenced, which
     * trips -Werror=unused-function.  `static const` keeps the reference
     * visible to the front end and still lets the optimiser drop the dead
     * branches, so a false value means the hooks are genuinely not installed.
     *
     *   kTractorActive = false -> none of the three tool hooks exist at all:
     *                             no spawn, no 3x3, no per-frame auto use.
     *                             Stage 1 (the horse already in the save) is
     *                             untouched.
     *   kTractorAutoUse = false -> keep the manual Y-button 3x3, drop only the
     *                             per-frame Game1.Update hook. */
    static const bool kTractorActive = true;
    static const bool kTractorAutoUse = true;
    /* v259 DIAGNOSTIC.  "Press the use-tool key on a new save and nothing
     * appears" -- this drops the two spawn gates that a fresh save can fail
     * (empty hand, standing on the farm) so one key press anywhere answers a
     * single question:
     *
     *   a horse appears  -> the hook fires and one of those two gates was the
     *                       blocker; put them back and narrow which one.
     *   nothing appears  -> the hook is not running at all (mod not installed /
     *                       fingerprint guard refused), a completely different
     *                       investigation.
     *
     * It also tolerates a null Farmer.netMount wrapper, which is the third
     * candidate gate.  TURN THIS OFF for any normal build. */
    constexpr bool kTractorDiagSpawn = false;
    constexpr uintptr_t kHorseCtorOff    = SDV_OFF(kHorseCtorOff, 0x12317F0u);
    constexpr uintptr_t kGetFarmOff      = SDV_OFF(kGetFarmOff, 0x1464FF0u);
    constexpr uintptr_t kGetLocNowOff    = SDV_OFF(kGetLocNowOff, 0x14407B0u);
    constexpr uintptr_t kNetRefSetOff    = SDV_OFF(kNetRefSetOff, 0x198A050u);
    constexpr uintptr_t kPressUseToolOff = SDV_OFF(kPressUseToolOff, 0x147BDE0u);
    constexpr uintptr_t kCurrentToolOff  = SDV_OFF(kCurrentToolOff, 0x13B8E20u);
    constexpr uintptr_t kTilesAffectedOff = SDV_OFF(kTilesAffectedOff, 0x1B3CF40u);
    constexpr uintptr_t kGameUpdateOff     = SDV_OFF(kGameUpdateOff, 0x14500A0u);
    constexpr unsigned  kFarmerUsingToolOff = SDV_OFF(kFarmerUsingToolOff, 0x648u);
    /* Tool.DoFunction bodies.  DoFunction is virtual and its AOT invoker takes
     * packed arguments, so it cannot be called directly -- but the vtable slot
     * is the same for every Tool subclass, so we can find that slot once at
     * runtime by looking for one of these known bodies and then use the slot
     * for any tool (including modded ones). */
    constexpr uintptr_t kDoFuncToolOff         = SDV_OFF(kDoFuncToolOff, 0x1B3E790u);
    constexpr uintptr_t kDoFuncHoeOff          = SDV_OFF(kDoFuncHoeOff, 0x1B54870u);
    constexpr uintptr_t kDoFuncAxeOff          = SDV_OFF(kDoFuncAxeOff, 0x1B44D30u);
    constexpr uintptr_t kDoFuncPickaxeOff      = SDV_OFF(kDoFuncPickaxeOff, 0x1B62620u);
    constexpr uintptr_t kDoFuncWateringCanOff  = SDV_OFF(kDoFuncWateringCanOff, 0x1B67AB0u);
    /* v290: GameLocation.damageMonster -- the 6-parameter overload.
     *
     * Tool.DoFunction calls it at 0x1B3E964 for the melee swing, with exactly
     * this register setup:
     *
     *     x0        = this (GameLocation)
     *     x1,x2     = Rectangle areaOfEffect   (X|Y<<32, W|H<<32)
     *     w3        = minDamage   = weapon.minDamage + 1
     *     w4        = maxDamage   = minDamage*3 + 3
     *     w5        = isBomb      = 0
     *     x6        = who (Farmer)
     *     w7        = triggerMonsterInvincibleTimer = 0
     *
     * so a plain 6-argument call matches the ABI with no packing tricks.  The
     * 0x14EA470 entry is a stub that fills in the remaining float arguments and
     * tail-calls the real body at 0x14EA4B0 -- call the stub, like the game.
     *
     * The damage numbers do not matter here: the boot patch at 0x14EA504 /
     * 0x14EA508 already forces min = max = 9999 in the body (the one-hit-kill
     * feature), so every hit is lethal either way. */
    constexpr uintptr_t kDamageMonsterOff = SDV_OFF(kDamageMonsterOff, 0x14EA470u);
    /* v291: MeleeWeapon.isScythe() -- the instance method, 0x1B579A0.
     *
     * There are TWO of them in the metadata and they are not interchangeable:
     *
     *   isScythe()       0x1B579A0  flags 0xC6  takes `this`
     *   IsScythe(...)    0x1B57040  flags 0x96  takes the itemId STRING
     *
     * The body proves it: isScythe does `ldr x0,[this+0x78]` (the itemId, with
     * a lazy virtual fallback when it is null) and then tail-calls IsScythe,
     * which is nothing but four `String.Equals` against static scythe ids
     * ("47" and friends).  Calling 0x1B57040 with a tool would hand it an
     * object where it expects a string -- so call the instance method.
     *
     * The +0x78 guard below is not cosmetic: it is what keeps the lazy
     * fallback at 0x15AE924 (a virtual dispatch we have no business invoking)
     * out of the picture.  A tool always has its itemId by the time it is in
     * someone's hands, so the guard never fires in practice. */
    constexpr uintptr_t kIsScytheOff = SDV_OFF(kIsScytheOff, 0x1B579A0u);
    constexpr unsigned  kItemIdOff   = SDV_OFF(kItemIdOff, 0x78u);
    constexpr unsigned kFarmerNetMountOff = SDV_OFF(kFarmerNetMountOff, 0x740u);
    constexpr unsigned kHorseNetRiderOff  = SDV_OFF(kHorseNetRiderOff, 0x4A0u);
    constexpr unsigned kHorseMountingOff  = SDV_OFF(kHorseMountingOff, 0x4B0u);
    constexpr unsigned kHorseIdOff        = SDV_OFF(kHorseIdOff, 0x498u);
    /* v275 -- appearance, per instance.
     *
     * Swapping Animals/horse.xnb outright turns EVERY horse into a tractor,
     * including the one bought from a stable.  So the sheet is 224x256 now:
     * rows 0-3 are the untouched vanilla horse, rows 4-7 are the tractor.
     * Vanilla horses use frames 0-27 and never look below row 3; only our own
     * horse gets its sourceRect pushed down four rows.
     *
     * AnimatedSprite.sourceRect is a plain field and draw 0x1194D00 reads it
     * straight out of the object (`ldur x8,[x20,#84]` / `ldur x9,[x20,#92]`),
     * so writing it is enough.  The game normally recomputes it in
     * UpdateSourceRect 0x11949D0, but that returns immediately when
     * `ignoreSourceRectUpdates` (+0x98) is set -- its first two instructions
     * are `ldrb w8,[x19,#152] / cbz w8, ...`.
     *
     * Offsets from the AOT metadata (meta_animatedsprite.json):
     *     currentFrame +0x3C, sourceRect +0x54 (X,Y,W,H), ignore +0x98
     * and Character.sprite is a NetRef: get_Sprite 0x1220590 is
     *     ldr x8,[x0,#0x50]; ldr x0,[x8,#0x68]; ret
     *
     * The romfs file may be missing (exefs installed without it), so
     * get_textureHeight 0x1192D50 is called once to confirm the tractor rows
     * exist; if they do not, nothing is touched and the tractor just looks
     * like a horse instead of sampling past the end of the sheet. */
    constexpr unsigned kCharSpriteRefOff    = SDV_OFF(kCharSpriteRefOff, 0x50u);
    constexpr unsigned kSpriteFrameOff      = SDV_OFF(kSpriteFrameOff, 0x3Cu);
    constexpr unsigned kSpriteSrcRectOff    = SDV_OFF(kSpriteSrcRectOff, 0x54u);
    constexpr unsigned kSpriteIgnoreRectOff = SDV_OFF(kSpriteIgnoreRectOff, 0x98u);
    constexpr uintptr_t kTextureHeightOff   = SDV_OFF(kTextureHeightOff, 0x1192D50u);
    constexpr int kTractorRowShift = 4;   /* rows 0-3 horse, rows 4-7 tractor */
    constexpr unsigned kNetValueOff2      = SDV_OFF(kNetValueOff2, 0x68u);
    constexpr unsigned kTrkCharsOff       = SDV_OFF(kTrkCharsOff, 0xB0u);
    constexpr unsigned kTrkCollListOff       = SDV_OFF(kTrkCollListOff, 0x58u);
    constexpr unsigned kTrkListSizeOff       = SDV_OFF(kTrkListSizeOff, 0x18u);
    constexpr unsigned kTrkListStoreOff      = SDV_OFF(kTrkListStoreOff, 0x10u);
    constexpr unsigned kTrkStoreCapOff       = SDV_OFF(kTrkStoreCapOff, 0x20u);
    constexpr unsigned kTrkStoreBufOff       = SDV_OFF(kTrkStoreBufOff, 0x10u);
    /* List<T>.Add -- only used when the characters list has no spare capacity. */
    constexpr uintptr_t kListAddOff = SDV_OFF(kListAddOff, 0x1709C90u);
    /* v261: summon the existing tractor to the player instead of making them
     * walk to it.  Character.position (+0x58) is a NetPosition OBJECT, not a
     * plain Vector2 -- get_Position 0x12202a0 is `ldr x8,[x0,#0x58]` then a
     * virtual call on it -- so the value has to go through the accessors.
     * set_Position 0x121ff20 takes (Character* this, Vector2 value) with the
     * Vector2 in s0/s1, i.e. exactly what a C++ `(void*, Vec2)` produces, and
     * it clears the cached tile/pixel fields on the way. */
    constexpr uintptr_t kGetPositionOff = SDV_OFF(kGetPositionOff, 0x12202A0u);
    constexpr uintptr_t kSetPositionOff = SDV_OFF(kSetPositionOff, 0x121FF20u);
    /* GameLocation.performToolAction(Tool, int tileX, int tileY, Farmer) --
     * the per-tile "apply this tool here" entry point.  Needed because the
     * scythe is a MeleeWeapon and has no DoFunction of its own. */
    constexpr uintptr_t kPerformToolActionOff =
        SDV_OFF(kPerformToolActionOff, 0x1535930u);
    /* v262: Character.currentLocation.  Field `currentLocationRef` at +0x180 is
     * a NetRef, and Character has a real accessor pair
     * (get_currentLocation 0x1220660 / set_currentLocation 0x1220670), so the
     * location is set through it rather than by poking +0x180.
     *
     * We never set this on a horse we create, which is why a dismount could
     * drop it out of the world: Horse.dismount 0x1233ED0 calls
     * get_currentLocation (0x1233f24) and then puts the horse back -- with a
     * stale/null location that write goes nowhere, the horse stays removed
     * from characters, and the player sees it vanish. */
    constexpr uintptr_t kSetCurrentLocationOff =
        SDV_OFF(kSetCurrentLocationOff, 0x1220670u);
    /* Character.currentLocation is a NetRef; the value sits at +0x68 like every
     * other NetRef.  We only ever READ it here -- writes go through
     * set_currentLocation 0x1220670. */
    constexpr unsigned kCharCurrentLocOff = SDV_OFF(kCharCurrentLocOff, 0x180u);
    /* GameLocation.isOutdoors, a NetBool at +0x1E0 (AOT metadata, type 0x476DF0). */
    constexpr unsigned kLocIsOutdoorsOff = SDV_OFF(kLocIsOutdoorsOff, 0x1E0u);
    /* Game1.get_locations -- the List<GameLocation>, used to sweep stale copies
     * of our horse out of every location. */
    constexpr uintptr_t kLocationsListOff = SDV_OFF(kLocationsListOff, 0x1440790u);
    /* v286 -- the tractor's icon on the animals page.
     *
     * AnimalPage.FindAnimals builds an AnimalEntry per animal, and the entry's
     * icon is a slice of the horse's own texture:
     *
     *     Texture           = horse.Sprite.Texture;
     *     TextureSourceRect = new Rectangle(0, horse.Sprite.SourceRect.Height * 2 - 26,
     *                                       horse.Sprite.SourceRect.Width, 24);
     *
     * With a 32-tall frame that is y = 38, which lands in the vanilla-horse half
     * of our 224x256 sheet.  Shifting Y by 128 moves the same slice into the
     * tractor half, so the entry shows a tractor instead of a horse.
     *
     * Offsets from the AOT metadata for StardewValley.Menus.AnimalPage+AnimalEntry
     * (aot_type.py cannot resolve the nested type -- its name-matching step gives
     * up -- but aot_meta.py --factory 0x71CC9C8 produces the table directly):
     *     Animal            +0x10
     *     TextureSourceRect +0x50   (Rectangle: X +0x50, Y +0x54, W +0x58, H +0x5C)
     *
     * v287 hooks AnimalPage.FindAnimals 0x1712830 instead of AnimalEntry..ctor
     * 0x1718B80.  v286 hooked the constructor and the console refused to boot;
     * patching the finished list is both safer (a plain non-virtual method, first
     * instruction `sub sp, sp, #0x90`) and more complete, because three of the
     * four `new AnimalEntry(...)` sites in FindAnimals are inlined and a
     * constructor hook would only ever have seen the fourth. */
    constexpr unsigned  kAnimalEntryAnimalOff = SDV_OFF(kAnimalEntryAnimalOff, 0x10u);
    constexpr unsigned  kAnimalEntryRectOff   = SDV_OFF(kAnimalEntryRectOff, 0x50u);
    constexpr uintptr_t kFindAnimalsOff       = SDV_OFF(kFindAnimalsOff, 0x1712830u);
    constexpr unsigned kToolLastUserOff = SDV_OFF(kToolLastUserOff, 0xF0u);
    /* v263: the movement lock the swing animation sets.  Farmer.canMove is a
     * PLAIN bool byte at +0x640 (forceCanMove 0x13db1e0 does
     * `strb w8,#1,[x19,#0x640]`), freezePause is an int at +0x704 (same
     * function does `str wzr,[x19,#0x704]`).  Clearing both right after
     * pressUseToolButton lets the tractor keep driving while the scythe
     * swings -- that is the "animation without the stutter" the user asked
     * for back at v255, and for the scythe the swing is unavoidable because
     * it IS the grass-cutting mechanism. */
    /* v264: OFF.  v263 cleared these two right after pressUseToolButton to stop
     * the swing from freezing the drive, and that broke the scythe: the swing
     * never reached its end, so Farmer.usingTool stayed true, and
     * AutoTractorUse bails out while usingTool is set -- one swing and then
     * nothing, which is exactly what the user reported ("no auto use, have to
     * press it by hand").  A manual press does not look at usingTool, which is
     * why that still worked. */
    constexpr bool kMeleeUnlockMove = false;
    /* v265: how to reach a tile's TerrainFeature, copied verbatim from
     * Hoe.DoFunction 0x1B54870 (it does the same lookup itself):
     *
     *     1b549fc  ldr  x24,[x20,#0x148]   ; location.terrainFeatures (NetDictionary)
     *     1b54a0c  ldr  x0, [x24,#0x58]    ; -> inner Dictionary<Vector2,TerrainFeature>
     *     1b54a14  fmov s0/s1 = tile       ; Vector2 is an HFA -> FP registers
     *     1b54a1c  sub  x1, x29,#0x50      ; &out
     *     1b54a20  bl   0xdd0d70           ; TryGetValue(dict, key, out value)
     *
     * so the inner Dictionary is a plain field at terrainFeatures+0x58 and the
     * lookup is one fixed address.  Nothing generic has to be resolved. */
    constexpr unsigned kTerrainFeaturesOff = SDV_OFF(kTerrainFeaturesOff, 0x148u);
    constexpr unsigned kNetDictInnerOff = SDV_OFF(kNetDictInnerOff, 0x58u);
    constexpr uintptr_t kTryGetValueOff = SDV_OFF(kTryGetValueOff, 0xDD0D70u);
    /* Grass.performToolAction -- virtual, no direct callers; used as the vtable fingerprint. */
    constexpr uintptr_t kGrassPTAOff = SDV_OFF(kGrassPTAOff, 0x1B16390u);
    /* v296: Grass.TryDropItemsOnCut(tool, addAnimation) = 0x1B16670.
     *
     * performToolAction tail-branches to it (`b 0x1b165d4` with x0 = grass,
     * x1 = tool, w2 = 1), so the devirtualised ABI is (this, tool, bool).
     * It is the half that actually decides hay vs fiber, and it only needs
     * numberOfWeeds to be 0 -- which is why v296 zeroes the count itself and
     * then calls this, instead of waiting for performToolAction to agree. */
    constexpr uintptr_t kGrassDropOff = SDV_OFF(kGrassDropOff, 0x1B16670u);
    /* v297: TerrainFeature.performToolAction is virtual slot 0x120 of the
     * vtable (MeleeWeapon.DoDamage reads it at 0x1b5b510 and adds the adjustor
     * byte at 0x1b5b514).  Objects use 0x540 instead -- the two hierarchies
     * have different tables, so the slot is per-hierarchy, not per-class. */
    constexpr unsigned kTfToolActionSlotOff = SDV_OFF(kTfToolActionSlotOff, 0x120u);
    /* v298: NetDictionary's "unwrap the stored value" virtual slot.
     *
     * Hoe.DoFunction and MeleeWeapon.DoDamage both call it right after
     * inner.TryGetValue: `ldr x9,[vtable,#0x210]` + adjustor byte at
     * `[vtable,#0x218]`, with x0 = dictionary+adjustor and x1 = the raw value.
     * The result is the TerrainFeature.  Skipping it hands you a wrapper. */
    constexpr unsigned kNetDictUnwrapSlotOff = SDV_OFF(kNetDictUnwrapSlotOff, 0x210u);
    /* v299: the two harvest tools.  Their DoFunction only reads `this.animal`
     * (+0x130, AOT metadata); the lookup lives in beginUsing, which takes
     * (this, GameLocation, int x, int y, Farmer) and ignores x/y.  Addresses
     * are the method bodies from the AOT metadata for each type. */
    constexpr uintptr_t kMilkPailBeginOff  = SDV_OFF(kMilkPailBeginOff, 0x1B60510u);
    constexpr uintptr_t kMilkPailDoFuncOff = SDV_OFF(kMilkPailDoFuncOff, 0x1B609E0u);
    constexpr uintptr_t kShearsBeginOff    = SDV_OFF(kShearsBeginOff, 0x1B63F50u);
    constexpr uintptr_t kShearsDoFuncOff   = SDV_OFF(kShearsDoFuncOff, 0x1B64160u);
    /* Tool.animal -- the farm animal the pail/shears are pointed at. */
    constexpr unsigned kToolAnimalOff = SDV_OFF(kToolAnimalOff, 0x130u);
    /* v271: the weeds route -- the one the PC mod actually uses.
     *
     * The mod's ScytheAttachment.TryHarvestWeeds does exactly three things:
     *     UseToolOnTile(tool, tile, player, location);   // sets tool.lastUser
     *     weeds.performToolAction(tool);                 // Object.performToolAction
     *     location.removeObject(tile, false);
     * Weeds are Objects in GameLocation.objects, NOT TerrainFeatures -- so the
     * terrainFeatures lookup we had could never see them.
     *
     * Offsets from the AOT metadata (docs/offsets/meta_gamelocation.json):
     *     GameLocation.objects        +0xC8   (NetDictionary<Vector2,Object>)
     *     GameLocation.terrainFeatures +0x148
     *     GameLocation.largeTerrainFeatures +0x130  (NOT objects -- the earlier
     *                          recon note had this one wrong)
     *     Object.performToolAction    0x19D6860 (virtual -> vtable slot)
     *     GameLocation.removeObject   0x14F10C0 (non-virtual, callable directly)
     *     Object.tileLocation        +0xA0    (NetVector2, value at +0x68) */
    constexpr unsigned kObjectsOff        = SDV_OFF(kObjectsOff, 0xC8u);
    /* v272 -- CRASH FIX.  NetDictionary<Vector2,Object> and
     * NetDictionary<Vector2,TerrainFeature> are DIFFERENT generic
     * instantiations (MT 0x51b1d0 vs 0x51acd0) and their field layouts differ:
     *
     *   terrainFeatures : inner Dictionary at +0x58, TryGetValue = 0xDD0D70
     *                     (Hoe.DoFunction 0x1B54870: 1b549fc ldr x24,[x20,#0x148]
     *                      -> 1b54a0c ldr x0,[x24,#0x58] -> bl 0xdd0d70)
     *   objects         : inner Dictionary at **+0x20**, and the game reaches it
     *                     through the thunk 0x19A1710:
     *                         19a1710  ldr x0,[x0,#32]   ; +0x20
     *                         19a1714  b   0x1557770     ; Dictionary.TryGetValue
     *                     GameLocation.removeObject 0x14F10C0 calls exactly that
     *                     (14f1100 ldr x0,[x19,#200] -> bl 0x19a1710).
     *
     * v271 reused +0x58 for objects, read a wild pointer and crashed with
     * "software closed because an error occurred".  Use the thunk instead --
     * same ABI: x0 = the NetDictionary, s0/s1 = key, x1 = &out. */
    constexpr uintptr_t kObjectsTryGetOff = SDV_OFF(kObjectsTryGetOff, 0x19A1710u);
    /* Object.IsWeeds -- `public bool IsWeeds()`, NON-virtual, so the base body
     * is the right thing to call.  The PC mod guards with weeds.IsWeeds()
     * before performToolAction; without it we would poke chests and machines. */
    constexpr uintptr_t kObjectIsWeedsOff = SDV_OFF(kObjectIsWeedsOff, 0x19D49D0u);
    /* v281 -- seeds and fertilizer, the one attachment family the tractor was
     * still missing.
     *
     * The whole thing already exists in the game: Object.placementAction
     * (Object.cs:6939) has a branch for `Category == -74 || Category == -19`
     * that resolves the seed id, validates the soil, calls HoeDirt.plant and
     * handles the sprinkler enricher.  So we do not reimplement any of it --
     * we call placementAction per tile and let the game decide.
     *
     * Addresses, all read out of the game's own seeding code at 0x19F817C:
     *
     *   19f8140  ldr x0,[x8,#24] ; bl 0x13B8C10   Farmer.get_ActiveObject
     *   19f8148  bl 0x15AE860                     Item.get_ItemId
     *   19f8168  bl 0x15AE7E0                     Item.get_Category
     *   19f816c  cmn w0, #0x13                    Category == -19 (fertilizer)
     *   19f817c  bl 0x1B1BD90                     HoeDirt.canPlantThisSeedHere
     *
     * get_Category / get_ItemId are plain non-virtual getters, so they can be
     * called directly -- which also saves having to know Item's field layout
     * (aot_type.py cannot extract Item at all: 1793 candidate factories, and
     * unicorn runs out of memory before it finds the right one).
     *
     * placementAction is virtual, so it goes through the calibrated vtable slot
     * like DoFunction does.  Its ABI, from the prologue at 0x19F66D0:
     *     19f6704 mov x24, x4    who
     *     19f670c mov w21, w3    y
     *     19f6710 mov w23, w2    x
     *     19f6718 mov x22, x1    location
     *     19f671c mov x20, x0    this
     * and x/y are PIXELS (placementAction divides by 64), so pass tile * 64. */
    constexpr uintptr_t kObjectPTAOff     = SDV_OFF(kObjectPTAOff, 0x19D6860u);
    constexpr uintptr_t kActiveObjectOff       = SDV_OFF(kActiveObjectOff, 0x13B8C10u);
    constexpr uintptr_t kItemCategoryOff       = SDV_OFF(kItemCategoryOff, 0x15AE7E0u);
    constexpr uintptr_t kReduceActiveItemOff   = SDV_OFF(kReduceActiveItemOff, 0x13D0710u);
    constexpr uintptr_t kObjectPlacementActOff = SDV_OFF(kObjectPlacementActOff, 0x19F66D0u);
    /* Item categories, straight from the game's own compare (cmn w0, #0x13). */
    constexpr int kCategorySeeds      = -74;
    constexpr int kCategoryFertilizer = -19;
    /* v273: the two Remove thunks, straight out of MeleeWeapon.DoDamage
     * 0x1B5B1A0 -- this is the vanilla per-tile sequence verbatim:
     *
     *   1b5b534  ldr x8, [x21,#32]
     *   1b5b538  ldr x0, [x8, #328]   ; 0x148 -> terrainFeatures (the wrapper)
     *   1b5b544  bl  0xdd0950         ; terrainFeatures.Remove(key)
     *   1b5b54c  ldr x0, [x8, #200]   ; 0xC8  -> objects (the wrapper)
     *   1b5b55c  bl  0x19a1710        ; objects.TryGetValue(key, &out)
     *   1b5b580  blr x10              ; object.performToolAction(tool)
     *   1b5b598  bl  0x19a15d0        ; objects.Remove(key)
     *
     * Note both Removes take the NetDictionary WRAPPER (x0) with the key in
     * s0/s1 -- not the inner Dictionary that TryGetValue needs. */
    constexpr uintptr_t kTerrainRemoveOff = SDV_OFF(kTerrainRemoveOff, 0xDD0950u);
    constexpr uintptr_t kObjectsRemoveOff = SDV_OFF(kObjectsRemoveOff, 0x19A15D0u);
    /* v274 / v396: one swing should cost nothing.
     *
     * Axe and Pickaxe do not build their own area, so they take the per-tile
     * loop -- nine calls.  Each of those plays a sound and deducts stamina
     * inside its own DoFunction (`who.Stamina -= 2*power - FarmingLevel*0.1`),
     * so a single 3x3 used to make nine noises and cost nine swings' worth.
     * The PC mod silences the tail (TractorManager sets
     * `tool.PlayUseSounds = false` once an attachment has applied) but leaves
     * stamina alone.
     *
     * v274 charged only the first call's cost.  v396 charges none: the balance
     * is read before the swing and written back after it.  The hoe and the
     * watering can take the other branch -- one DoFunction at forced power 3 --
     * and are restored the same way.
     *
     *   Tool.PlayUseSounds   +0x120  plain bool (not a NetBool)
     *   Farmer.get_Stamina   0x13B9270  ldr x8,[x0,#1416]; ldr s0,[x8,#104]
     *   Farmer.set_Stamina   0x13B9280  (this, float in s0) */
    constexpr unsigned kToolPlaySoundsOff = SDV_OFF(kToolPlaySoundsOff, 0x120u);
    constexpr uintptr_t kGetStaminaOff    = SDV_OFF(kGetStaminaOff, 0x13B9270u);
    constexpr uintptr_t kSetStaminaOff    = SDV_OFF(kSetStaminaOff, 0x13B9280u);
    constexpr unsigned kObjectTileLocOff  = SDV_OFF(kObjectTileLocOff, 0xA0u);
    constexpr unsigned kFarmerCanMoveOff = SDV_OFF(kFarmerCanMoveOff, 0x640u);
    constexpr unsigned kFarmerFreezePauseOff =
        SDV_OFF(kFarmerFreezePauseOff, 0x704u);
    /* Any fixed 128-bit value.  It only has to not collide with a real horse. */
    constexpr u64 kTractorGuidLo = 0x4B1D7E4C5A9F0001ull;
    constexpr u64 kTractorGuidHi = 0x9A3C5E7F2D1B0A09ull;
    /* Tool.tilesAffected does power++ then lays out tiles by facing direction;
     * 3 becomes a 3x3 cross. */
    constexpr int kTractorPower = 3;
    /* Radius used for the tools that do NOT go through tilesAffected (see
     * AutoTractorUse): 1 gives the same 3x3 footprint. */
    constexpr int kTractorRadius = 1;

    static bool s_tractorTried = false;
    static void* s_tractor = nullptr;

    /* Append `npc` to GameLocation.characters.
     *
     * characters is a NetCollection at +0xB0 whose List<NPC> hangs at +0x58.
     * The List layout is the one already established for List<NPC>: size at
     * +0x18, a wrapper at +0x10 holding the capacity (+0x20) and the element
     * buffer (+0x10).
     *
     * If the buffer is full we give up.  List<T>.Add is deliberately not called
     * from native code (v212), and writing past the end would corrupt the
     * location -- which is a save-corrupting bug, not a cosmetic one. */
    bool AppendToCharacters(void* loc, void* npc) {
        void* coll = *reinterpret_cast<void**>(static_cast<char*>(loc) + kTrkCharsOff);
        if (coll == nullptr || !Plausible(coll)) {
            return false;
        }
        void* list = *reinterpret_cast<void**>(static_cast<char*>(coll) + kTrkCollListOff);
        if (list == nullptr || !Plausible(list)) {
            return false;
        }
        void* store = *reinterpret_cast<void**>(static_cast<char*>(list) + kTrkListStoreOff);
        if (store == nullptr || !Plausible(store)) {
            return false;
        }
        const int n = *reinterpret_cast<int*>(static_cast<char*>(list) + kTrkListSizeOff);
        /* Capacity is a DOUBLE dereference -- this is the layout the project
         * already established empirically (List indexer 0x11521E0 / 0x1203A70):
         *     n        = *(int*)(list + 0x18)
         *     wrapper  = *(void**)(list + 0x10)
         *     capacity = *(int*)(*(void**)(wrapper + 0x20))
         *     buffer   = *(void**)(wrapper + 0x10)
         * Reading the int at wrapper+0x20 directly would give a pointer's low
         * half, the "is there room" test would then always pass, and the write
         * below would run past the end of the array. */
        void* capCell = *reinterpret_cast<void**>(static_cast<char*>(store) + kTrkStoreCapOff);
        if (capCell == nullptr || !Plausible(capCell)) {
            return false;
        }
        const int cap = *reinterpret_cast<int*>(capCell);
        void* buf = *reinterpret_cast<void**>(static_cast<char*>(store) + kTrkStoreBufOff);
        if (buf == nullptr || n < 0) {
            return false;
        }
        if (n < cap) {
            /* Fast path: room already allocated.  No managed call, no GC. */
            reinterpret_cast<void**>(buf)[n] = npc;
            *reinterpret_cast<int*>(static_cast<char*>(list) + kTrkListSizeOff) = n + 1;
            return true;
        }
        /* v260: the list is FULL -- a brand-new save starts with an empty
         * characters list whose capacity is 0, so the old "give up" branch made
         * the tractor impossible to summon on any fresh farm, silently.
         *
         * List<T>.Add (0x1709C90) is exactly this layout and grows the array
         * itself:
         *     1709cb8  ldr x9,[x19,#0x10]   ; wrapper
         *     1709cbc  ldr w8,[x19,#0x18]   ; _size
         *     1709cc0  ldr x10,[x9,#0x20]   ; capacity cell
         *     1709cc8  cmp w8,w10
         *     1709cd0  b.eq -> 0x1710920 EnsureCapacity(1)
         *     1709d00  str x20,[x9,w8,sxtw #3]  ; buffer[_size] = item
         * so calling it is the same two-dereference layout, just with growth.
         *
         * Allocating here is safe: this runs in the Update pass
         * (pressUseToolButton), never in the draw pass -- the v212 rule about
         * not calling List.Add was about appending from drawHUD. */
        reinterpret_cast<void (*)(void*, void*)>(g_base + kListAddOff)(list, npc);
        return true;
    }

    /* Is this horse the one we made?
     *
     * `s_tractor` alone is not enough: it is a session variable, and the horse
     * survives a reload as a normal saved object with a different address.  The
     * one thing that survives is its HorseId -- we spawn it with a fixed Guid,
     * so reading horseId back identifies it across sessions.
     *
     * Horse.horseId is a NetGuid at +0x498 and its value is two u64 at +0x68
     * (get_HorseId 0x1231970 is literally `ldp x0, x1, [x8, #104]`). */
    bool IsOurTractor(void* horse) {
        if (horse == nullptr || !Plausible(horse)) {
            return false;
        }
        void* idRef = *reinterpret_cast<void**>(static_cast<char*>(horse) + kHorseIdOff);
        if (idRef == nullptr || !Plausible(idRef)) {
            return false;
        }
        const u64* guid = reinterpret_cast<const u64*>(
            static_cast<char*>(idRef) + kNetValueOff2);
        return guid[0] == kTractorGuidLo && guid[1] == kTractorGuidHi;
    }

    /* Walk GameLocation.characters looking for our horse.  Returns it, or null.
     * Same List<NPC> layout as AppendToCharacters, read-only. */
    /* Defined below; AdoptTractor needs it. */
    void* FindTractorIn(void* loc);

    /* Take `npc` out of `loc.characters`, if it is there.  The exact mirror of
     * AppendToCharacters, using the same established List<T> layout:
     *     n      = *(int*)(list + 0x18)
     *     wrapper= *(void**)(list + 0x10)
     *     buffer = *(void**)(wrapper + 0x10)
     * Returns true only when it really found and removed an entry, so the
     * caller can tell "it was in this list" from "it was somewhere else".
     *
     * Why this exists: Horse.dismount only puts a horse back into
     * currentLocation.characters when TryFindStable() finds a stable for it
     * (Horse.cs).  Ours was never bought from one, so the game never re-adds
     * it -- and nothing updates a horse's currentLocation when the player
     * warps, so a horse ridden off the farm still claims to live on the farm.
     * Dismount elsewhere and it sits in a list that is not being drawn: the
     * "sometimes the horse disappears" report. */
    bool RemoveFromCharacters(void* loc, void* npc) {
        void* coll = *reinterpret_cast<void**>(static_cast<char*>(loc) + kTrkCharsOff);
        if (coll == nullptr || !Plausible(coll)) {
            return false;
        }
        void* list = *reinterpret_cast<void**>(static_cast<char*>(coll) + kTrkCollListOff);
        if (list == nullptr || !Plausible(list)) {
            return false;
        }
        void* store = *reinterpret_cast<void**>(static_cast<char*>(list) + kTrkListStoreOff);
        if (store == nullptr || !Plausible(store)) {
            return false;
        }
        const int n = *reinterpret_cast<int*>(static_cast<char*>(list) + kTrkListSizeOff);
        if (n <= 0 || n > 512) {
            return false;
        }
        void* buf = *reinterpret_cast<void**>(static_cast<char*>(store) + kTrkStoreBufOff);
        if (buf == nullptr || !Plausible(buf)) {
            return false;
        }
        void** elems = reinterpret_cast<void**>(buf);
        for (int i = 0; i < n; i++) {
            if (elems[i] != npc) {
                continue;
            }
            for (int j = i; j + 1 < n; j++) {
                elems[j] = elems[j + 1];
            }
            elems[n - 1] = nullptr;
            *reinterpret_cast<int*>(static_cast<char*>(list) + kTrkListSizeOff) = n - 1;
            return true;
        }
        return false;
    }

    /* Make `horse` live in `here`: pull it out of whatever location it still
     * claims, put it into here exactly once, and keep the link honest so a
     * later dismount has somewhere to go.
     *
     * The removal matters.  Appending without removing would leave the horse in
     * two location lists at once, and the save serialises per location -- it
     * would come back as two horses with the same HorseId. */
    /* Keep the tractor outside.  Summoning it in a shop or in the farmhouse
     * would park it in a location it can never be ridden out of, because a
     * horse is only carried across a warp while it is being ridden. */
    /* Remove our horse from EVERY location's character list.
     *
     * Needed because the animal page (AnimalPage.FindAnimals) counts a horse
     * twice when it is both in a location's characters AND in Farmer.mount --
     * vanilla avoids that by removing the horse from characters when you mount
     * it (Horse.update, the `characters.Remove(this)` at the end of the mount
     * animation).  Ours stayed in the list, so the page showed the vanilla
     * horse plus two entries for the tractor.
     *
     * Sweeping all locations (Game1.get_locations 0x1440790 -> List<GameLocation>)
     * also cleans up copies left behind by earlier versions, which is why the
     * user's save had accumulated extras. */
    bool LocationIsOutdoors(void* loc);
    bool LocationAllowsTractor(void* loc);

    /* Remove every copy of OUR horse from `loc`, except `keep`.
     *
     * RemoveFromCharacters compares pointers, which only catches the same
     * object appearing twice.  But every horse this mod has ever created shares
     * one fixed Guid (kTractorGuidLo/Hi), so an older build could leave behind
     * a DIFFERENT object carrying the SAME HorseId -- a stale copy that a
     * pointer comparison walks right past.  That is what accumulates in a save
     * across versions, and it is why the animal page kept growing.
     *
     * So sweep on the Guid as well: anything that answers IsOurTractor but is
     * not the instance we are about to keep gets dropped. */
    int RemoveOurCopiesFrom(void* loc, void* keep) {
        void* coll = *reinterpret_cast<void**>(static_cast<char*>(loc) + kTrkCharsOff);
        if (coll == nullptr || !Plausible(coll)) {
            return 0;
        }
        void* list = *reinterpret_cast<void**>(static_cast<char*>(coll) + kTrkCollListOff);
        if (list == nullptr || !Plausible(list)) {
            return 0;
        }
        void* store = *reinterpret_cast<void**>(static_cast<char*>(list) + kTrkListStoreOff);
        if (store == nullptr || !Plausible(store)) {
            return 0;
        }
        int n = *reinterpret_cast<int*>(static_cast<char*>(list) + kTrkListSizeOff);
        if (n <= 0 || n > 512) {
            return 0;
        }
        void* buf = *reinterpret_cast<void**>(static_cast<char*>(store) + kTrkStoreBufOff);
        if (buf == nullptr || !Plausible(buf)) {
            return 0;
        }
        void** elems = reinterpret_cast<void**>(buf);
        int removed = 0;
        /* Walk downwards so a shift never skips an entry. */
        for (int i = n - 1; i >= 0; i--) {
            void* e = elems[i];
            if (e == nullptr || e == keep || !Plausible(e)) {
                continue;
            }
            if (!IsOurTractor(e)) {
                continue;
            }
            for (int j = i; j + 1 < n; j++) {
                elems[j] = elems[j + 1];
            }
            elems[n - 1] = nullptr;
            n--;
            ++removed;
        }
        if (removed > 0) {
            *reinterpret_cast<int*>(static_cast<char*>(list) + kTrkListSizeOff) = n;
        }
        return removed;
    }

    void RemoveTractorEverywhere(void* horse) {
        void* locs = reinterpret_cast<void* (*)(void*)>(
            g_base + kLocationsListOff)(nullptr);
        if (locs == nullptr || !Plausible(locs)) {
            return;
        }
        void* store = *reinterpret_cast<void**>(static_cast<char*>(locs) + kTrkListStoreOff);
        if (store == nullptr || !Plausible(store)) {
            return;
        }
        const int n = *reinterpret_cast<int*>(static_cast<char*>(locs) + kTrkListSizeOff);
        void* buf = *reinterpret_cast<void**>(static_cast<char*>(store) + kTrkStoreBufOff);
        if (buf == nullptr || !Plausible(buf) || n <= 0 || n > 512) {
            return;
        }
        void** elems = reinterpret_cast<void**>(buf);
        int removed = 0;
        for (int i = 0; i < n; i++) {
            void* loc = elems[i];
            if (loc == nullptr || !Plausible(loc)) {
                continue;
            }
            /* Two passes, because there are two kinds of leftover:
             *   - this exact object appended twice (pointer match), and
             *   - a different object with our HorseId from an older build. */
            while (RemoveFromCharacters(loc, horse)) {
                ++removed;
                if (removed > 8) {
                    return;               /* runaway guard */
                }
            }
            removed += RemoveOurCopiesFrom(loc, horse);
        }
        if (removed > 0) {
            LOGF("tractor: removed %d stale copies", removed);
        }
    }

    /* DO NOT take the horse out of `characters` here, and do not do it right
     * after MountTractor either.
     *
     * Horse.update only runs for NPCs that are IN a location's character list,
     * and the mount animation -- pulling the rider in, the hop, and finally
     * `rider.mount = this` -- is driven entirely by Horse.update.  Removing the
     * horse at that moment stops it being updated, so `mounting` never
     * completes, the rider never gets mounted, and the horse is not drawn
     * either: pressing the key appears to do nothing at all.  That was v283's
     * regression.
     *
     * Horse.update removes the horse itself once the animation finishes
     * (`characters.Remove(this)` at the end of the mounting branch), which is
     * what keeps it from being counted twice on the animal page.  Left to the
     * game, it works. */

    /* Dismount is not hooked, so notice the transition once per frame and put
     * the horse back on the ground where the player actually is.
     *
     * This is also the real fix for "the horse disappears": Horse.dismount only
     * re-adds a horse whose TryFindStable() is non-null, and ours has no stable,
     * so the game would never do it. */
    static bool s_wasRiding = false;

    void MaintainTractorPresence() {
        void* horse = s_tractor;
        if (horse == nullptr || !Plausible(horse) || !IsOurTractor(horse)) {
            return;
        }
        void* pl = reinterpret_cast<void* (*)(void*)>(g_base + kGetPlayerOff)(nullptr);
        if (pl == nullptr || !Plausible(pl)) {
            return;
        }
        void* mountRef = *reinterpret_cast<void**>(
            static_cast<char*>(pl) + kFarmerNetMountOff);
        bool riding = false;
        if (mountRef != nullptr && Plausible(mountRef)) {
            riding = *reinterpret_cast<void**>(
                         static_cast<char*>(mountRef) + kNetValueOff2) == horse;
        }
        if (riding == s_wasRiding) {
            return;
        }
        s_wasRiding = riding;
        if (riding) {
            /* Just finished mounting: rider.mount is set, which only happens on
             * the last frame of Horse.update's mounting animation.  Only NOW is
             * it safe to take the horse out of the list -- doing it earlier
             * stops Horse.update running at all and the animation never
             * finishes (that was v283's regression).
             *
             * While mounted the horse is drawn through Farmer.draw, so it does
             * not need a list entry, and AnimalPage.FindAnimals already counts
             * it once through Farmer.mount.  Leaving it in the list as well is
             * what made the animal page show it twice. */
            void* now = reinterpret_cast<void* (*)(void*)>(g_base + kGetLocNowOff)(nullptr);
            if (now != nullptr && Plausible(now)) {
                if (RemoveFromCharacters(now, horse)) {
                    LOGF("tractor: hidden after mount");
                }
            }
            return;
        }
        void* here = reinterpret_cast<void* (*)(void*)>(g_base + kGetLocNowOff)(nullptr);
        if (here == nullptr || !Plausible(here) || !LocationAllowsTractor(here)) {
            return;
        }
        RemoveTractorEverywhere(horse);
        reinterpret_cast<void (*)(void*, void*)>(g_base + kSetCurrentLocationOff)(
            horse, here);
        if (!AppendToCharacters(here, horse)) {
            LOGF("tractor: could not put it back after dismount");
            return;
        }
        LOGF("tractor: placed after dismount");
    }

    bool LocationIsOutdoors(void* loc) {
        void* ref = *reinterpret_cast<void**>(
            static_cast<char*>(loc) + kLocIsOutdoorsOff);
        if (ref == nullptr || !Plausible(ref)) {
            return false;
        }
        return *reinterpret_cast<unsigned char*>(
                   static_cast<char*>(ref) + kNetValueOff2) != 0;
    }

    /* v292: does this location's internal name start with `ascii`?
     *
     * GameLocation.Name is a NetString:  loc+0x1A8 -> wrapper, wrapper+0x68 ->
     * the System.String (get_Name 0x14C6D10 is literally those two loads).  A
     * managed string is the object header followed by the length at +0x10 and
     * UTF-16 characters at +0x14, so the compare below is a plain char loop.
     *
     * Fails CLOSED: any bad pointer, absurd length or non-ASCII mismatch simply
     * answers "no", which leaves the caller on the old, safe behaviour. */
    bool LocationNameStartsWith(void* loc, const char* ascii) {
        if (loc == nullptr || !Plausible(loc) || ascii == nullptr) {
            return false;
        }
        void* ref = *reinterpret_cast<void**>(static_cast<char*>(loc) + kLocNameOff);
        if (ref == nullptr || !Plausible(ref)) {
            return false;
        }
        void* str = *reinterpret_cast<void**>(static_cast<char*>(ref) + kNetValueOff2);
        if (str == nullptr || !Plausible(str)) {
            return false;
        }
        const int len = *reinterpret_cast<int*>(static_cast<char*>(str) + 0x10);
        if (len <= 0 || len > 64) {
            return false;
        }
        const unsigned short* chars = reinterpret_cast<const unsigned short*>(
            static_cast<char*>(str) + 0x14);
        int i = 0;
        for (; ascii[i] != '\0'; i++) {
            if (i >= len || chars[i] != static_cast<unsigned short>(ascii[i])) {
                return false;
            }
        }
        return true;
    }

    /* v292: where may the tractor be?
     *
     * Outdoors has always been fine.  v282 then banned indoor summoning
     * outright ("never park it indoors"), and that turned out to also ban the
     * mines -- exactly the place the auto-attack feature is for.  A mine shaft
     * is a normal GameLocation with isOutdoors = false, so the only practical
     * way to tell it apart from a house or a shop is its name.
     *
     * Houses, shops and festivals stay refused: a horse parked in Pierre's
     * would be written into that location's save and greet you at the counter
     * forever. */
    bool LocationAllowsTractor(void* loc) {
        if (loc == nullptr || !Plausible(loc)) {
            return false;
        }
        if (LocationIsOutdoors(loc)) {
            return true;
        }
        return LocationNameStartsWith(loc, "UndergroundMine") ||
               LocationNameStartsWith(loc, "Volcano");
    }

    bool AdoptTractor(void* here, void* horse) {
        void* ref = *reinterpret_cast<void**>(
            static_cast<char*>(horse) + kCharCurrentLocOff);
        void* old = nullptr;
        if (ref != nullptr && Plausible(ref)) {
            old = *reinterpret_cast<void**>(static_cast<char*>(ref) + kNetValueOff2);
        }
        if (old != nullptr && old != here && Plausible(old)) {
            if (RemoveFromCharacters(old, horse)) {
                LOGF("tractor: moved out of the previous location");
            }
        }
        /* v283: sweep every location, not just the one it claims.  A stale
         * copy in some other list is exactly what made the animal page show
         * the tractor twice. */
        RemoveTractorEverywhere(horse);
        if (!AppendToCharacters(here, horse)) {
            /* Do not mount a horse that is in no location list: it would be
             * drawn while ridden and lost the moment you dismount. */
            LOGF("tractor: adopt could not append");
            return false;
        }
        reinterpret_cast<void (*)(void*, void*)>(g_base + kSetCurrentLocationOff)(
            horse, here);
        return true;
    }

    void* FindTractorIn(void* loc) {
        void* coll = *reinterpret_cast<void**>(static_cast<char*>(loc) + kTrkCharsOff);
        if (coll == nullptr || !Plausible(coll)) {
            return nullptr;
        }
        void* list = *reinterpret_cast<void**>(static_cast<char*>(coll) + kTrkCollListOff);
        if (list == nullptr || !Plausible(list)) {
            return nullptr;
        }
        void* store = *reinterpret_cast<void**>(static_cast<char*>(list) + kTrkListStoreOff);
        if (store == nullptr || !Plausible(store)) {
            return nullptr;
        }
        void* buf = *reinterpret_cast<void**>(static_cast<char*>(store) + kTrkStoreBufOff);
        const int n = *reinterpret_cast<int*>(static_cast<char*>(list) + kTrkListSizeOff);
        if (buf == nullptr || n <= 0 || n > 512) {
            return nullptr;
        }
        void** elems = reinterpret_cast<void**>(buf);
        for (int i = 0; i < n; i++) {
            if (IsOurTractor(elems[i])) {
                return elems[i];
            }
        }
        return nullptr;
    }

    /* v292: find our horse in ANY location.
     *
     * Until now the summon path only knew about the current location plus the
     * cached s_tractor pointer, and s_tractor is only ever filled from the
     * location the player is standing in.  Start a session in the mines and the
     * horse (parked on the farm) is invisible to both -- pressing the key fell
     * through to "create a new one", which is farm-only, so nothing happened.
     *
     * Same list walk as RemoveTractorEverywhere, one FindTractorIn per entry.
     * Only runs on a key press, never per frame. */
    void* FindTractorAnywhere() {
        void* locs = reinterpret_cast<void* (*)(void*)>(
            g_base + kLocationsListOff)(nullptr);
        if (locs == nullptr || !Plausible(locs)) {
            return nullptr;
        }
        void* store = *reinterpret_cast<void**>(static_cast<char*>(locs) + kTrkListStoreOff);
        if (store == nullptr || !Plausible(store)) {
            return nullptr;
        }
        const int n = *reinterpret_cast<int*>(static_cast<char*>(locs) + kTrkListSizeOff);
        void* buf = *reinterpret_cast<void**>(static_cast<char*>(store) + kTrkStoreBufOff);
        if (buf == nullptr || !Plausible(buf) || n <= 0 || n > 512) {
            return nullptr;
        }
        void** elems = reinterpret_cast<void**>(buf);
        for (int i = 0; i < n; i++) {
            void* loc = elems[i];
            if (loc == nullptr || !Plausible(loc)) {
                continue;
            }
            void* found = FindTractorIn(loc);
            if (found != nullptr) {
                return found;
            }
        }
        return nullptr;
    }
    /* v261: set rider first (Horse.update bails out when it is null), then the
     * mounting flag.  Shared by "spawn a new one" and "bring the existing one
     * here" so the two paths cannot drift apart. */
    bool MountTractor(void* pl, void* horse) {
        void* riderRef = *reinterpret_cast<void**>(
            static_cast<char*>(horse) + kHorseNetRiderOff);
        if (riderRef == nullptr || !Plausible(riderRef)) {
            return false;
        }
        reinterpret_cast<void (*)(void*, void*)>(g_base + kNetRefSetOff)(
            riderRef, pl);
        void* mountFlag = *reinterpret_cast<void**>(
            static_cast<char*>(horse) + kHorseMountingOff);
        if (mountFlag != nullptr && Plausible(mountFlag)) {
            *reinterpret_cast<unsigned char*>(
                static_cast<char*>(mountFlag) + kNetValueOff2) = 1;
        }
        return true;
    }

    void TrySpawnTractor() {
        if (!kTractor) {
            return;
        }
        void* pl = reinterpret_cast<void* (*)(void*)>(g_base + kGetPlayerOff)(nullptr);
        if (pl == nullptr || !Plausible(pl)) {
            return;
        }
        /* Already riding something: leave the one shot unused rather than burn
         * it.  netMount is a NetRef, so the wrapper is always there and the
         * value at +0x68 is null when not riding. */
        void* mountRef = *reinterpret_cast<void**>(static_cast<char*>(pl) + kFarmerNetMountOff);
        if (mountRef == nullptr || !Plausible(mountRef)) {
            if (!kTractorDiagSpawn) {
                return;
            }
        }
        else {
            void* riding = *reinterpret_cast<void**>(
                static_cast<char*>(mountRef) + kNetValueOff2);
            if (riding != nullptr) {
                /* If it is our tractor we are done for this session -- remember
                 * it so stage 2 can recognise "on the tractor" without a
                 * re-scan. */
                if (IsOurTractor(riding)) {
                    s_tractor = riding;
                    s_tractorTried = true;
                    LOGF("tractor: already riding it");
                }
                return;
            }
        }
        void* here = reinterpret_cast<void* (*)(void*)>(g_base + kGetLocNowOff)(nullptr);
        if (here == nullptr) {
            return;
        }
        if (!kTractorDiagSpawn) {
            /* Empty hand only.  This one applies EVERYWHERE: pressing the
             * use-tool key while holding a tool must never teleport a horse. */
            if (reinterpret_cast<void* (*)(void*)>(g_base + kCurrentToolOff)(pl) !=
                nullptr) {
                return;
            }
        }
        /* v282: the farm restriction used to sit here and covered both
         * "bring the horse to me" and "make a new one".  Bringing the horse is
         * now allowed anywhere -- riding to town, dismounting and pressing the
         * key used to do nothing, which is exactly what the user reported.
         * Only CREATING a horse stays farm-only, checked further down. */
        /* A tractor from a previous session may already be standing here.  The
         * one-shot flag is per session, so without this check every reload
         * would leave another horse on the farm. */
        void* existing = FindTractorIn(here);
        if (existing == nullptr && s_tractor != nullptr &&
            Plausible(s_tractor) && IsOurTractor(s_tractor)) {
            /* The horse is not in this location's characters but we still hold
             * a live pointer to it: either it vanished (dismount does not
             * re-add a horse with no stable) or it is standing somewhere else.
             * AdoptTractor below takes it out of the old list and puts it here.
             * IsOurTractor re-checks the HorseId Guid, so a recycled pointer
             * fails the test instead of being adopted. */
            existing = s_tractor;
        }
        if (existing == nullptr) {
            /* v292: the horse is parked in a location the player is not in --
             * e.g. on the farm while the player just entered the mines, so this
             * session has never seen it and s_tractor is still null.  Walk the
             * location list once instead of falling through to "create a new
             * one", which is farm-only and would do nothing here. */
            existing = FindTractorAnywhere();
            if (existing != nullptr) {
                s_tractor = existing;
            }
        }
        if (existing != nullptr) {
            /* v261: SUMMON it instead of just remembering it.
             *
             * Before this, pressing the key while the tractor stood anywhere
             * on the farm only set s_tractor and returned -- the player had to
             * walk over and mount it by hand.  After passing out in the far
             * corner of the farm that is exactly the case the user complained
             * about ("if I faint, the tractor is miles from home").
             *
             * Moving it is two accessor calls: read the player's Vector2 and
             * write it into the horse, then run the same rider+mounting
             * sequence a fresh spawn uses.  Horse.update does the rest (it
             * pulls the farmer in and sets rider.mount). */
            /* v282: AdoptTractor moves it out of the location it still claims
             * before putting it here, and sets currentLocation.  That covers
             * both the "vanished" case and the "left it on the farm and walked
             * to town" case. */
            if (!LocationAllowsTractor(here)) {
                return;                  /* never park it indoors */
            }
            if (!AdoptTractor(here, existing)) {
                return;
            }
            const Vec2 p =
                reinterpret_cast<Vec2 (*)(void*)>(g_base + kGetPositionOff)(pl);
            reinterpret_cast<void (*)(void*, Vec2)>(g_base + kSetPositionOff)(
                existing, p);
            MountTractor(pl, existing);
            s_tractor = existing;
            s_tractorTried = true;
            LOGF("tractor: summoned to player");
            return;
        }

        /* v262: nothing to reuse, so a fresh spawn must be allowed even if one
         * happened earlier in this session -- otherwise a horse that got lost
         * made the feature dead until a restart.  Duplicates are still
         * prevented by the FindTractorIn check above. */
        /* Creating a horse stays farm-only.  Spawning one inside a house, a
         * mine or a shop would put a horse somewhere it can never be ridden
         * out of, and the farm is where a tractor belongs. */
        if (!kTractorDiagSpawn) {
            void* farm =
                reinterpret_cast<void* (*)(void*)>(g_base + kGetFarmOff)(nullptr);
            if (farm == nullptr || here != farm) {
                return;
            }
        }
        s_tractorTried = false;
        const Vec2 tile = reinterpret_cast<Vec2 (*)(void*)>(g_base + kGetTileBodyOff)(pl);
        void* horse = reinterpret_cast<void* (*)(void*, u64, u64, int, int)>(
            g_base + kHorseCtorOff)(nullptr, kTractorGuidLo, kTractorGuidHi,
                                    static_cast<int>(tile.x), static_cast<int>(tile.y));
        if (horse == nullptr || !Plausible(horse)) {
            /* v260: do NOT burn the one shot on a failure.  Any silent failure
             * used to disable the whole feature for the rest of the session,
             * so one bad press looked exactly like "the mod does nothing". */
            LOGF("tractor: ctor returned nothing");
            return;
        }
        if (!AppendToCharacters(here, horse)) {
            LOGF("tractor: could not add to characters");
            return;
        }
        /* v262: link the horse to the farm.  Without this a dismount cannot
         * put it back and it disappears. */
        reinterpret_cast<void (*)(void*, void*)>(g_base + kSetCurrentLocationOff)(
            horse, here);
        if (!MountTractor(pl, horse)) {
            LOGF("tractor: horse has no netRider wrapper");
            return;
        }
        s_tractorTried = true;
        s_tractor = horse;
        LOGF("tractor: spawned at %d,%d", static_cast<int>(tile.x), static_cast<int>(tile.y));
    }

    /* Set only for the duration of one pressUseToolButton call (see
     * HookPressUseTool).  It cannot be derived from `who.mount`, because that
     * is deliberately nulled out for exactly that call. */
    static bool s_forceTractorPower = false;

    static bool (*s_origPressUseTool)() = nullptr;

    bool HookPressUseTool() {
        /* v304: master switch off -- behave exactly like vanilla. */
        if (g_tractorOn == 0) {
            return s_origPressUseTool != nullptr ? s_origPressUseTool() : false;
        }
        TrySpawnTractor();
        /* The game refuses to use a tool while mounted.  pressUseToolButton
         * 0x147BDE0 branches on `player.isRidingHorse()` and takes the
         * "interact instead of use the tool" path, so nothing downstream ever
         * runs -- that is why v250's tilesAffected hook never fired.
         *
         * Rather than hook isRidingHorse 0x13B9480 (seven instructions, almost
         * certainly inlined into that branch), hide the mount for exactly the
         * duration of the original call and put it back immediately.  The
         * horse's own `rider` field is left alone, so no dismount logic can
         * start: this is one synchronous call, no frame boundary is crossed. */
        void* pl = reinterpret_cast<void* (*)(void*)>(g_base + kGetPlayerOff)(nullptr);
        if (pl != nullptr && Plausible(pl)) {
            void* mountRef = *reinterpret_cast<void**>(
                static_cast<char*>(pl) + kFarmerNetMountOff);
            if (mountRef != nullptr && Plausible(mountRef)) {
                void** slot = reinterpret_cast<void**>(
                    static_cast<char*>(mountRef) + kNetValueOff2);
                void* horse = *slot;
                if (IsOurTractor(horse)) {
                    *slot = nullptr;
                    s_forceTractorPower = true;
                    const bool used = s_origPressUseTool != nullptr
                                          ? s_origPressUseTool()
                                          : false;
                    s_forceTractorPower = false;
                    *slot = horse;
                    return used;
                }
            }
        }
        return s_origPressUseTool != nullptr ? s_origPressUseTool() : false;
    }

    /* ---- stage 2: area of effect while on the tractor ------------------
     *
     * The first idea was to call Tool.DoFunction once per neighbouring tile.
     * That is not callable: DoFunction is virtual, and the invoker the AOT
     * metadata hands out (0x73A6D04) starts with `ldp x19, x8, [x1]` -- it
     * takes its arguments packed in a struct, not in the normal registers.
     *
     * It is also unnecessary. Hoe.DoFunction already applies the tool to a
     * LIST of tiles:
     *
     *     List<Vector2> list = tilesAffected(vector, power, who);
     *     foreach (Vector2 item in list) { ...makeHoeDirt(item)... }
     *
     * and `Tool.tilesAffected` is a plain non-virtual method (flags 0x84,
     * body 0x1B3CF40) that every farming tool calls.  It starts with
     * `power++` and then lays tiles out by facing direction, so raising the
     * power widens the area using the game's own per-tile validation,
     * stamina handling and sound effects -- no per-tile calls of our own,
     * no extra stamina, no 9 overlapping sounds.
     *
     * Argument registers, read off the prologue:
     *     1b3cf6c  fmov s8, s1     <- Vector2 tileLocation (HFA in s0/s1)
     *     1b3cf70  fmov s9, s0
     *     1b3cf78  mov  x21, x2    <- Farmer who
     *     1b3cf7c  mov  w20, w1    <- int power
     * so a plain C++ `(void*, Vec2, int, void*)` produces exactly that ABI:
     * the HFA takes the FP registers and the integers fill x0/x1/x2 in order.
     *
     * Only raised while `who` is riding OUR horse (identified by HorseId), so
     * the player's own horse behaves like vanilla. */
    static void* (*s_origTilesAffected)(void*, Vec2, int, void*) = nullptr;

    void* HookTilesAffected(void* self, Vec2 tile, int power, void* who) {
        if (kTractor && power < kTractorPower) {
            /* Two ways to be "on the tractor", because the tool does not fire
             * inside pressUseToolButton at all:
             *
             *   pressUseToolButton 0x147BDE0 only STARTS the swing animation.
             *   The real DoFunction -> tilesAffected call happens frames later,
             *   from the animation update.  By then HookPressUseTool has already
             *   put player.netMount back, so s_forceTractorPower is false again
             *   and the flag alone never matches (v251: tool worked, but still
             *   one tile at a time).
             *
             * So: the flag covers any call that happens synchronously inside
             * pressUseToolButton, and who.mount covers the animation-driven one.
             * At the later time the mount is restored, so this reads correctly. */
            bool onTractor = s_forceTractorPower;
            if (!onTractor && who != nullptr && Plausible(who)) {
                void* mountRef = *reinterpret_cast<void**>(
                    static_cast<char*>(who) + kFarmerNetMountOff);
                if (mountRef != nullptr && Plausible(mountRef)) {
                    onTractor = IsOurTractor(*reinterpret_cast<void**>(
                        static_cast<char*>(mountRef) + kNetValueOff2));
                }
            }
            if (onTractor) {
                power = kTractorPower;
            }
        }
        return s_origTilesAffected != nullptr
                   ? s_origTilesAffected(self, tile, power, who)
                   : nullptr;
    }

    /* ---- stage 2c: drive and it works by itself (option C) -------------
     *
     * The user picks a tool and drives; no button press.  Rule:
     *
     *   on our tractor + holding a tool + NOT mid-swing + crossed into a new
     *   tile  ->  run one tool use
     *
     * "Crossed into a new tile" is the gate rather than a timer: it is what
     * "moving" actually means for a tractor, it cannot fire while standing
     * still, and it throttles itself because a tile takes several frames to
     * cross.
     *
     * The mid-swing check is essential: Farmer.usingTool (NetBool @ +0x648) is
     * true while the swing animation runs.  Firing again during it restarts the
     * animation, so it would never reach the frame that actually applies the
     * tool -- the same class of bug as v251.
     *
     * Per-frame entry point: Game1.Update 0x14500A0, whose first instruction is
     * `sub sp, sp, #0x150` (safe to hook).  Its neighbours are not usable:
     * Game1._update 0x1450430 starts with `stp d9, d8, [sp, #-112]!` and
     * Game1.UpdateControlInput 0x145B820 with `str d10, [sp, #-128]!` -- both
     * pre-indexed stores that move SP, the shape that froze the console when
     * GameMenu.draw was hooked.
     *
     * This runs in the UPDATE path, not the draw path. */
    static int s_lastTileX = 0x7FFFFFFF;
    static int s_lastTileY = 0x7FFFFFFF;

    /* Which vtable slot is DoFunction?
     *
     * A virtual call in this AOT build looks like
     *     ldr x9, [x8]        ; x8 = object, x9 = MethodTable
     *     ldr x9, [x9, #8]    ; x9 = vtable
     *     ldr x10, [x9, #N]   ; x10 = method
     * (seen at 0x147C278 in pressUseToolButton).  So the vtable is MethodTable+8
     * and the slot index is fixed by the class hierarchy -- the same index for
     * Hoe, Axe, Pickaxe and everything else.
     *
     * We do not know the index a priori, so: scan the vtable for a pointer that
     * equals one of the DoFunction bodies we do know, and remember that index.
     * After that the index works for every tool, modded ones included. */
    static int s_doFunctionSlot = -1;

    constexpr uintptr_t kDoFunctionBodies[] = {
        kDoFuncToolOff, kDoFuncHoeOff, kDoFuncAxeOff,
        kDoFuncPickaxeOff, kDoFuncWateringCanOff,
        /* v301: the two harvest tools.  Without these, FindDoFunction returns
         * nullptr for a milk pail or shears (their DoFunction is an override we
         * did not know), and AutoTractorUse bailed out before it could ever run
         * beginUsing -- the readout showed begin = 0 with a pail in hand. */
        kMilkPailDoFuncOff, kShearsDoFuncOff,
    };

    /* Find the vtable slot whose entry equals `body`.  The slot index is
     * fixed by the class hierarchy, so one calibration works for every
     * instance of that class (and, for a virtual method, for every subclass
     * that does not override it). */
    void* FindVTableSlot(void* obj, void* body) {
        void* mt = *reinterpret_cast<void**>(obj);
        if (mt == nullptr || !Plausible(mt)) {
            return nullptr;
        }
        void* vt = *reinterpret_cast<void**>(static_cast<char*>(mt) + 8);
        if (vt == nullptr || !Plausible(vt)) {
            return nullptr;
        }
        void** slots = reinterpret_cast<void**>(vt);
        for (int i = 0; i < 256; i++) {
            if (slots[i] == body) {
                return slots[i];
            }
        }
        return nullptr;
    }

    void* FindDoFunction(void* tool) {
        void* mt = *reinterpret_cast<void**>(tool);
        if (mt == nullptr) {
            return nullptr;
        }
        void* vt = *reinterpret_cast<void**>(static_cast<char*>(mt) + 8);
        if (vt == nullptr) {
            return nullptr;
        }
        void** slots = reinterpret_cast<void**>(vt);
        if (s_doFunctionSlot >= 0) {
            return slots[s_doFunctionSlot];
        }
        for (int i = 0; i < 256; i++) {
            void* f = slots[i];
            for (unsigned k = 0;
                 k < sizeof(kDoFunctionBodies) / sizeof(kDoFunctionBodies[0]); k++) {
                if (f == reinterpret_cast<void*>(g_base + kDoFunctionBodies[k])) {
                    s_doFunctionSlot = i;
                    LOGF("tractor: DoFunction slot = %d", i);
                    return f;
                }
            }
        }
        return nullptr;
    }

    /* Place the held item on every tile of the tractor's area, using the
     * game's own placementAction so all of its rules apply (only tilled soil
     * for seeds, only empty/valid soil for fertilizer, sprinkler enrichers,
     * the correct sound, and so on).
     *
     * One item is consumed per successful placement -- placementAction itself
     * does NOT decrement the stack (Object.cs:6993 returns true right after
     * planting), so we call Farmer.reduceActiveItemByOne 0x13D0710, which is
     * the same thing vanilla does after a successful use.
     *
     * When the stack empties, reduceActiveItemByOne removes the item from the
     * inventory, so `item` becomes garbage: the loop re-reads ActiveObject
     * before every tile and stops as soon as it is no longer the same object. */
    void PlaceHeldItemPerTile(void* pl, void* item, int tx, int ty) {
        void* here = reinterpret_cast<void* (*)(void*)>(g_base + kGetLocNowOff)(nullptr);
        if (here == nullptr || !Plausible(here)) {
            return;
        }
        /* v283: call the body DIRECTLY instead of calibrating a vtable slot.
         *
         * v281 went through FindVTableSlot, and "seeds do nothing" is exactly
         * what a silent nullptr from that scan looks like.  Everything that
         * reaches here has Category -74 or -19, i.e. a plain StardewValley.Object,
         * so the virtual dispatch would land on Object.placementAction anyway --
         * going through the slot buys nothing and adds one failure mode. */
        auto place = reinterpret_cast<bool (*)(void*, void*, int, int, void*)>(
            g_base + kObjectPlacementActOff);
        if (s_scytheStage < 0x20u) {
            s_scytheStage = 0x20u;        /* item path reached */
        }
        auto activeObject = reinterpret_cast<void* (*)(void*)>(g_base + kActiveObjectOff);
        auto reduce = reinterpret_cast<void (*)(void*)>(g_base + kReduceActiveItemOff);
        int placed = 0;
        for (int dy = -kTractorRadius; dy <= kTractorRadius; ++dy) {
            for (int dx = -kTractorRadius; dx <= kTractorRadius; ++dx) {
                if (activeObject(pl) != item) {
                    LOGF("tractor: held item exhausted after %d", placed);
                    return;
                }
                if (place(item, here, (tx + dx) * 64, (ty + dy) * 64, pl)) {
                    reduce(pl);
                    ++placed;
                    if (s_scytheStage < 0x21u) {
                        s_scytheStage = 0x21u;   /* at least one tile accepted */
                    }
                } else if (s_scytheStage < 0x22u) {
                    s_scytheStage = 0x22u;       /* called, but refused */
                }
            }
        }
        if (placed > 0) {
            LOGF("tractor: placed %d item(s)", placed);
        }
    }

    /* v290: hit every monster standing in the tractor's 3x3.
     *
     * Weapons are MeleeWeapon, which declares no DoFunction of its own, so the
     * per-tile tool path in AutoTractorUse can never touch a monster -- it only
     * ever reaches Tool.DoFunction, the swing.  The canonical entry is
     * GameLocation.damageMonster, which takes an area and damages every monster
     * whose bounding box intersects it, so ONE call covers the whole ring.
     *
     * Coordinates are PIXELS, like every other damageMonster caller: the game
     * itself builds a 64x64 rectangle around the player.  We build the 3x3 the
     * tractor already uses (kTractorRadius), i.e. 192x192 starting one tile up
     * and to the left of the player's tile.
     *
     * Returns true when at least one monster was in range. */
    struct DamageRect { int x, y, w, h; };

    bool DamageMonstersAround(void* loc, void* who, int tx, int ty) {
        if (loc == nullptr || !Plausible(loc) || who == nullptr || !Plausible(who)) {
            return false;
        }
        using DamageMonsterFn = bool (*)(void*, DamageRect, int, int, int, void*, int);
        auto damage = reinterpret_cast<DamageMonsterFn>(g_base + kDamageMonsterOff);
        const int side = (kTractorRadius * 2 + 1) * 64;
        DamageRect area{
            (tx - kTractorRadius) * 64,
            (ty - kTractorRadius) * 64,
            side,
            side,
        };
        if (s_combatCalls < 0xFFu) {
            ++s_combatCalls;
        }
        const bool hit = damage(loc, area, 1, 3, 0, who, 0);
        if (hit && s_combatHits < 0xFFu) {
            ++s_combatHits;
        }
        return hit;
    }

    /* v291: is this tool a scythe?  See kIsScytheOff for why the instance
     * method is the safe one to call. */
    bool ToolIsScythe(void* tool) {
        if (tool == nullptr || !Plausible(tool)) {
            return false;
        }
        void* id = *reinterpret_cast<void**>(static_cast<char*>(tool) + kItemIdOff);
        if (id == nullptr || !Plausible(id)) {
            return false;
        }
        return reinterpret_cast<int (*)(void*)>(g_base + kIsScytheOff)(tool) != 0;
    }

    bool OnOurTractor(void* pl) {
        void* mountRef = *reinterpret_cast<void**>(
            static_cast<char*>(pl) + kFarmerNetMountOff);
        if (mountRef == nullptr || !Plausible(mountRef)) {
            return false;
        }
        return IsOurTractor(*reinterpret_cast<void**>(
            static_cast<char*>(mountRef) + kNetValueOff2));
    }

    void AutoTractorUse() {
        auto mark = [](unsigned n) {
            if (s_scytheStage < n) {
                s_scytheStage = n;
            }
        };
        void* pl = reinterpret_cast<void* (*)(void*)>(g_base + kGetPlayerOff)(nullptr);
        if (pl == nullptr || !Plausible(pl)) {
            return;
        }
        if (!OnOurTractor(pl)) {
            /* re-arm, so dismounting and mounting again starts clean */
            s_lastTileX = 0x7FFFFFFF;
            s_lastTileY = 0x7FFFFFFF;
            return;
        }
        mark(2);
        void* heldTool = reinterpret_cast<void* (*)(void*)>(g_base + kCurrentToolOff)(pl);
        /* Two modes.  A tool takes the DoFunction path below; a seed or a bag of
         * fertilizer has no Tool at all (CurrentTool is null), so it takes the
         * placement path instead.  Anything else in hand -- a chest, a piece of
         * furniture -- is deliberately ignored, because placementAction would
         * happily put it on the ground on every tile we drive over. */
        void* heldObject = nullptr;
        if (heldTool == nullptr) {
            heldObject = reinterpret_cast<void* (*)(void*)>(g_base + kActiveObjectOff)(pl);
            if (heldObject == nullptr || !Plausible(heldObject)) {
                return;                              /* truly empty handed */
            }
            const int cat = reinterpret_cast<int (*)(void*)>(
                g_base + kItemCategoryOff)(heldObject);
            if (cat != kCategorySeeds && cat != kCategoryFertilizer) {
                return;                              /* not plantable: do nothing */
            }
            if (s_scytheStage < 0x24u) {
                s_scytheStage = 0x24u;   /* held item is a seed / fertilizer */
            }
        }
        mark(3);
        void* usingRef = *reinterpret_cast<void**>(
            static_cast<char*>(pl) + kFarmerUsingToolOff);
        if (usingRef != nullptr && Plausible(usingRef) &&
            *reinterpret_cast<unsigned char*>(
                static_cast<char*>(usingRef) + kNetValueOff2) != 0) {
            return;                                  /* mid-swing, let it finish */
        }
        mark(4);
        const Vec2 tile = reinterpret_cast<Vec2 (*)(void*)>(g_base + kGetTileBodyOff)(pl);
        const int tx = static_cast<int>(tile.x);
        const int ty = static_cast<int>(tile.y);
        if (tx == s_lastTileX && ty == s_lastTileY) {
            return;                                  /* standing still */
        }
        mark(5);
        const bool armed = (s_lastTileX != 0x7FFFFFFF);
        s_lastTileX = tx;
        s_lastTileY = ty;
        if (!armed) {
            return;                                  /* first sample after mounting */
        }
        mark(6);
        if (heldTool == nullptr) {
            /* v312: the 播种施肥 row.  Off means the seed stays in hand. */
            if (g_rowOn[kRowPlant] != 0) {
                PlaceHeldItemPerTile(pl, heldObject, tx, ty);
            }
            return;
        }
        /* Apply the tool DIRECTLY, the way the PC Tractor Mod does:
         *
         *     tool.DoFunction(location, player.lastClick.X, player.lastClick.Y, 0, player);
         *
         * Going through pressUseToolButton instead starts a swing animation,
         * which locks the player for ~20 frames -- that is the stutter the user
         * reported.  DoFunction is the same entry point the animation would
         * eventually reach, minus the animation.
         *
         * `who.mount` is left alone here, so the tilesAffected hook still sees
         * the tractor and widens the area to 3x3. */
        void* tool = reinterpret_cast<void* (*)(void*)>(g_base + kCurrentToolOff)(pl);
        if (tool == nullptr) {
            return;
        }
        mark(7);
        void* doFunction = FindDoFunction(tool);
        if (doFunction == nullptr) {
            return;                       /* unknown tool class: do nothing */
        }
        mark(8);
        void* here = reinterpret_cast<void* (*)(void*)>(g_base + kGetLocNowOff)(nullptr);
        if (here == nullptr) {
            return;
        }
        mark(9);
        /* Tile centre in pixels: DoFunction divides x and y by 64. */
        const int px = tx * 64 + 32;
        const int py = ty * 64 + 32;
        auto callTool =
            reinterpret_cast<void (*)(void*, void*, int, int, int, void*)>(
                doFunction);

        /* Two families of tools, two different ways to reach 3x3.
         *
         * Hoe and WateringCan build their work list with tilesAffected(), so
         * ONE DoFunction already covers the whole area once the power hook
         * raises power to 3.  Calling them nine times would apply 3x3 nine
         * times over (81 tiles, 81 sounds).
         *
         * Axe, Pickaxe and the base Tool.DoFunction (which the scythe uses --
         * MeleeWeapon declares no DoFunction of its own) never call
         * tilesAffected.  They resolve x/64, y/64 to ONE tile and act on
         * whatever sits there: a tree, a stone, a patch of grass.  power does
         * not widen them, so the only way to get an area is to call DoFunction
         * once per tile.
         *
         * That is vanilla behaviour applied nine times, which is the intent:
         *   - a tree only matches the terrainFeatures entry at its own base
         *     tile, so it still takes exactly one hit, not nine;
         *   - a multi-tile resource clump (boulder, large log) is matched from
         *     every tile it covers, so it takes several hits at once -- that is
         *     precisely "one swing clears the ring of rocks".
         *
         * power=1 for the per-tile calls, matching what Farmer.useTool 0x13d90c0
         * passes (`mov w4, #1`), so stamina and damage behave like a normal
         * swing.  The area path keeps 0, which is where tilesAffected's own
         * power++ starts from. */
        const bool buildsOwnArea =
            doFunction == reinterpret_cast<void*>(g_base + kDoFuncHoeOff) ||
            doFunction ==
                reinterpret_cast<void*>(g_base + kDoFuncWateringCanOff);
        /* v261: the scythe is a MeleeWeapon, and MeleeWeapon declares NO
         * DoFunction of its own -- it inherits Tool.DoFunction 0x1B3E790,
         * which is the melee-swing path (doSwipe / damageMonster), not the
         * "act on the tile at x,y" path.  Calling it per tile therefore does
         * nothing visible: the user reported exactly that.
         *
         * Grass and crops are cut through GameLocation.performToolAction
         * 0x1535930 instead -- that is the canonical "apply this tool to this
         * one tile" entry point (Grass.performToolAction tests
         * `t is MeleeWeapon && isScythe()`), and its ABI is:
         *
         *     1535958  mov w21, w3   ; tileY
         *     1535960  mov w22, w2   ; tileX
         *     1535964  mov x19, x1   ; Tool
         *     153596c  mov x20, x0   ; this (GameLocation)
         *     (who stays in x4)
         *
         * so `(void* loc, void* tool, int tx, int ty, void* who)` matches. */
        const bool meleeTool =
            doFunction == reinterpret_cast<void*>(g_base + kDoFuncToolOff);
        /* v312: ONE GATE PER TOOL FAMILY.
         *
         * The Mod page has a row for each of these, and a row that is off has
         * to mean "this tool does nothing while driving" -- not "the tool
         * behaves a bit differently".  Deciding that here, once, before any
         * branch runs, is what makes the eight switches honest: v305 gated
         * three of them from inside the branches, which left hoe, watering can,
         * axe, pickaxe and the seed path with no switch at all.
         *
         * -1 means no row governs this tool (a fishing rod, a wand, anything we
         * have never handled); those keep whatever they did before. */
        const bool isScythe = meleeTool && ToolIsScythe(tool);
        int toolRow = -1;
        if (doFunction == reinterpret_cast<void*>(g_base + kDoFuncHoeOff)) {
            toolRow = kRowHoe;
        }
        else if (doFunction ==
                 reinterpret_cast<void*>(g_base + kDoFuncWateringCanOff)) {
            toolRow = kRowWater;
        }
        else if (doFunction == reinterpret_cast<void*>(g_base + kDoFuncAxeOff)) {
            toolRow = kRowAxe;
        }
        else if (doFunction ==
                 reinterpret_cast<void*>(g_base + kDoFuncPickaxeOff)) {
            toolRow = kRowPick;
        }
        else if (doFunction ==
                     reinterpret_cast<void*>(g_base + kMilkPailDoFuncOff) ||
                 doFunction ==
                     reinterpret_cast<void*>(g_base + kShearsDoFuncOff)) {
            toolRow = kRowMilk;
        }
        else if (meleeTool) {
            toolRow = isScythe ? kRowScythe : kRowCombat;
        }
        if (toolRow >= 0 && g_rowOn[toolRow] == 0) {
            return;                       /* that row is off: do nothing */
        }
        /* v290: auto-attack.  `meleeTool` means "no DoFunction of its own",
         * which for a Tool means MeleeWeapon -- sword, club, dagger or scythe.
         * The tile path below cannot hurt a monster (it ends up in
         * Tool.DoFunction, i.e. the swing, which acts on the player's facing,
         * not on the tile we pass), so monsters are handled separately, once
         * per tile the tractor moves onto, with the whole 3x3 as the area.
         *
         * One call per tile, not nine: damageMonster already sweeps the area
         * itself, and nine overlapping calls would hit the same monster nine
         * times.
         *
         * v291: the scythe is a MeleeWeapon too, so `meleeTool` is true for it
         * as well -- but vanilla scythes cannot damage monsters, and the user
         * asked for that to stay true.  Ask the game. */
        if (isScythe) {
            if (s_combatScythe < 0xFFu) {
                ++s_combatScythe;
            }
        }
        else if (meleeTool) {
            /* v312: the 打怪 row was checked above. */
            DamageMonstersAround(here, pl, tx, ty);
        }
        /* v299: MILK PAIL / SHEARS need beginUsing() first.
         *
         * Decompiled from StardewValley.Tools.MilkPail:
         *
         *     public override bool beginUsing(GameLocation location, int x, int y, Farmer who)
         *     {
         *         x = (int)who.GetToolLocation().X;
         *         y = (int)who.GetToolLocation().Y;
         *         animal = Utility.GetBestHarvestableFarmAnimal(
         *             toolRect: new Rectangle(x - 32, y - 32, 64, 64),
         *             animals: location.animals.Values, tool: this);
         *         ...
         *     }
         *
         * and DoFunction only harvests `animal.currentProduce` -- it never looks
         * an animal up itself.  We only ever called DoFunction, so `animal`
         * (+0x130) stayed null and the pail did nothing at all.  That is the
         * whole reason milking and shearing never worked.
         *
         * beginUsing ignores the x/y arguments it is given (it overwrites them
         * from the player), so passing 0 is fine.  Its rect is one tile around
         * the player, which is exactly where the tractor is. */
        {
            void* begin = nullptr;
            if (doFunction == reinterpret_cast<void*>(g_base + kMilkPailDoFuncOff)) {
                begin = reinterpret_cast<void*>(g_base + kMilkPailBeginOff);
            }
            else if (doFunction == reinterpret_cast<void*>(g_base + kShearsDoFuncOff)) {
                begin = reinterpret_cast<void*>(g_base + kShearsBeginOff);
            }
            if (begin != nullptr) {
                /* v312: the 挤奶剪毛 row was checked above. */
                reinterpret_cast<bool (*)(void*, void*, int, int, void*)>(begin)(
                    tool, here, 0, 0, pl);
                if (s_milkBegin < 255u) {
                    ++s_milkBegin;
                }
                /* Tool.animal sits at +0x130 in both MilkPail and Shears (same
                 * declaration order, same metadata offsets).  Non-null here
                 * means beginUsing found a harvestable animal; whether anything
                 * lands in the bag then depends on it actually having produce. */
                void* animal = *reinterpret_cast<void**>(
                    static_cast<char*>(tool) + kToolAnimalOff);
                if (animal != nullptr && Plausible(animal) && s_milkFound < 255u) {
                    ++s_milkFound;
                }
            }
        }
        if (buildsOwnArea) {
            mark(10);
            /* v396: the hoe and watering can build their own work list, so this
             * is ONE DoFunction -- but it runs at forced power 3, and the
             * stamina formula inside it scales with power.  Snapshot and put it
             * back: driving the tractor does not tire you out.
             *
             * v397: only write back if the value actually dropped.  Right after
             * a save is loaded the net field can still read 0 for a frame or
             * two, and refunding that snapshot would paint 0 over the real
             * stamina -- which is how one console came up exhausted on entry. */
            auto getStamina = reinterpret_cast<float (*)(void*)>(
                g_base + kGetStaminaOff);
            auto setStamina = reinterpret_cast<void (*)(void*, float)>(
                g_base + kSetStaminaOff);
            const float staminaBefore = getStamina(pl);
            s_forceTractorPower = true;
            callTool(tool, here, px, py, 0, pl);
            s_forceTractorPower = false;
            if (getStamina(pl) < staminaBefore) {
                setStamina(pl, staminaBefore);
            }
        } else if (meleeTool) {
            /* v312: the 割草 row was checked above. */
            /* v265: the scythe.  Nothing on the TOOL can do this -- see
             * docs/SCYTHE_ANALYSIS.md:
             *
             *   - MeleeWeapon has no DoFunction of its own; the inherited
             *     Tool.DoFunction is the melee-swing path (v255: cut nothing).
             *   - GameLocation.performToolAction only touches +0x148 once, as
             *     an Any(...) CHECK; it applies tools to objects/resourceClumps
             *     only (v261: cut nothing).
             *   - pressUseToolButton cuts, but the swing locks movement, and
             *     clearing that lock wedges usingTool (v263: stopped auto-using).
             *
             * The effect lives on the GRASS, not on the scythe.  So: look the
             * tile up in terrainFeatures and let the Grass do the work.  No
             * animation, no usingTool, no canMove.
             *
             * Grass.performToolAction is virtual with ZERO direct callers in
             * the whole text segment, so its slot is calibrated at runtime the
             * same way FindDoFunction does it: scan the vtable for a pointer
             * equal to the known Grass body.  That scan doubles as the type
             * check -- a Tree's vtable will not contain it. */
            int applied = 0;
            mark(11);
            /* v271: THE WEEDS ROUTE -- the one the PC mod actually uses.
             *
             * ScytheAttachment.TryHarvestWeeds does exactly three things:
             *     UseToolOnTile(tool, tile, player, location);  // sets lastUser
             *     weeds.performToolAction(tool);                // Object.performToolAction
             *     location.removeObject(tile, false);
             *
             * Weeds are Objects in GameLocation.objects (+0xC8), NOT
             * TerrainFeatures -- so the terrainFeatures lookup that v265 used
             * could never have seen a weed.  That is the whole reason v255-v270
             * cut nothing.
             *
             * Object.performToolAction ends in cutWeed(t.getLastFarmerToUse()),
             * which dereferences `who` -- so tool.lastUser MUST be set first or
             * it throws inside our own call.  (Tool.lastUser is a plain field at
             * +0xF0, not a NetRef.)
             *
             * ABI notes:
             *   Object.performToolAction(Tool t)          x0=obj, x1=tool
             *   GameLocation.removeObject(Vector2, bool)   x0=loc, s0/s1=tile, w1=bool
             *     (Vector2 is an HFA -> FP registers; the bool continues in the
             *      integer sequence at x1, not x2)
             */
            *reinterpret_cast<void**>(
                static_cast<char*>(tool) + kToolLastUserOff) = pl;
            {
                /* NOTE: pass the NetDictionary itself to the thunk -- do NOT
                 * read an inner dictionary field.  objects' inner Dictionary
                 * lives at +0x20, not at terrainFeatures' +0x58.  See
                 * kObjectsTryGetOff. */
                void* odict = *reinterpret_cast<void**>(
                    static_cast<char*>(here) + kObjectsOff);
                if (odict != nullptr && Plausible(odict)) {
                    mark(0x10);
                    auto tryGet = reinterpret_cast<bool (*)(void*, Vec2, void**)>(
                        g_base + kObjectsTryGetOff);
                    auto isWeeds = reinterpret_cast<bool (*)(void*)>(
                        g_base + kObjectIsWeedsOff);
                    void* objectPta =
                        reinterpret_cast<void*>(g_base + kObjectPTAOff);
                    for (int dy = -kTractorRadius; dy <= kTractorRadius; ++dy) {
                        for (int dx = -kTractorRadius; dx <= kTractorRadius; ++dx) {
                            const Vec2 key{static_cast<float>(tx + dx),
                                           static_cast<float>(ty + dy)};
                            void* obj = nullptr;
                            if (!tryGet(odict, key, &obj)) {
                                continue;
                            }
                            if (obj == nullptr || !Plausible(obj)) {
                                continue;
                            }
                            mark(0x11);
                            if (!isWeeds(obj)) {
                                continue;      /* not a weed: leave it alone */
                            }
                            /* Self-check: the object must really sit on this
                             * tile.  Object.tileLocation is a NetVector2 at
                             * +0xA0 with its value at +0x68.  If the dictionary
                             * lookup were wrong we would otherwise cut
                             * something at random. */
                            void* tlRef = *reinterpret_cast<void**>(
                                static_cast<char*>(obj) + kObjectTileLocOff);
                            if (tlRef == nullptr || !Plausible(tlRef)) {
                                continue;
                            }
                            const float* tl = reinterpret_cast<const float*>(
                                static_cast<char*>(tlRef) + kNetValueOff2);
                            if (tl[0] != key.x || tl[1] != key.y) {
                                continue;
                            }
                            void* fn = FindVTableSlot(obj, objectPta);
                            if (fn == nullptr) {
                                continue;
                            }
                            mark(0x12);
                            if (reinterpret_cast<bool (*)(void*, void*)>(fn)(obj, tool)) {
                                mark(0x13);
                                applied++;
                                if (s_scytheCalls < 255u) {
                                    s_scytheCalls++;
                                }
                                /* vanilla: 1b5b598 bl 0x19a15d0 on the objects
                                 * wrapper.  (GameLocation.removeObject would
                                 * also work but does extra bookkeeping.) */
                                reinterpret_cast<bool (*)(void*, Vec2)>(
                                    g_base + kObjectsRemoveOff)(odict, key);
                            }
                        }
                    }
                }
            }
            void* nd = *reinterpret_cast<void**>(
                static_cast<char*>(here) + kTerrainFeaturesOff);
            void* dict = nullptr;
            if (nd != nullptr && Plausible(nd)) {
                void* inner = *reinterpret_cast<void**>(
                    static_cast<char*>(nd) + kNetDictInnerOff);
                if (inner != nullptr && Plausible(inner)) {
                    dict = inner;
                }
            }
            if (dict != nullptr) {
                mark(13);
                auto tryGet = reinterpret_cast<bool (*)(void*, Vec2, void**)>(
                    g_base + kTryGetValueOff);
                void* grassBody =
                    reinterpret_cast<void*>(g_base + kGrassPTAOff);
                /* v298: THE VALUE IN THE DICTIONARY IS NOT THE FEATURE.
                 *
                 * Hoe.DoFunction does two steps, and we were only doing the
                 * first:
                 *
                 *     1b54a20  bl 0xdd0d70          ; inner.TryGetValue(key,&raw)
                 *     1b54a2c  ldur x1,[x29,#-0x50] ; x1 = raw
                 *     1b54a28  ldr x8,[x24]         ; x24 = the NetDictionary
                 *     1b54a30  ldr x8,[x8,#8]       ; its vtable
                 *     1b54a34  ldr x9,[x8,#0x210]   ; slot 0x210
                 *     1b54a40  blr x9               ; -> the TerrainFeature
                 *     1b54a4c  ldr x9,[x8,#0x120]   ; then performToolAction
                 *
                 * i.e. the dictionary hands back a wrapper and the NetDictionary
                 * itself unwraps it (slot 0x210, adjustor byte at 0x218).  We
                 * used the raw value as if it were the feature, so every read
                 * after that was the wrapper's: its MethodTable has no Grass
                 * body at slot 0x120, +0x30/+0x48 are not grassType /
                 * numberOfWeeds.  That is why nothing ever matched -- not the
                 * slot test, not the field test -- while `found` kept counting. */
                void* unwrapFn = nullptr;
                void* ndBase = nullptr;
                {
                    void* ndMt = *reinterpret_cast<void**>(nd);
                    if (ndMt != nullptr && Plausible(ndMt)) {
                        void* ndVt = *reinterpret_cast<void**>(
                            static_cast<char*>(ndMt) + 8);
                        if (ndVt != nullptr && Plausible(ndVt)) {
                            unwrapFn = *reinterpret_cast<void**>(
                                static_cast<char*>(ndVt) + kNetDictUnwrapSlotOff);
                            const unsigned char adj =
                                *reinterpret_cast<unsigned char*>(
                                    static_cast<char*>(ndVt) +
                                    kNetDictUnwrapSlotOff + 8);
                            ndBase = static_cast<char*>(nd) + adj;
                        }
                    }
                }
                for (int dy = -kTractorRadius; dy <= kTractorRadius; ++dy) {
                    for (int dx = -kTractorRadius; dx <= kTractorRadius; ++dx) {
                        const Vec2 key{static_cast<float>(tx + dx),
                                       static_cast<float>(ty + dy)};
                        void* raw = nullptr;
                        if (!tryGet(dict, key, &raw)) {
                            continue;              /* nothing on this tile */
                        }
                        if (raw == nullptr || !Plausible(raw)) {
                            continue;
                        }
                        void* tf = raw;
                        if (unwrapFn != nullptr) {
                            tf = reinterpret_cast<void* (*)(void*, void*)>(
                                unwrapFn)(ndBase, raw);
                        }
                        if (tf == nullptr || !Plausible(tf)) {
                            continue;
                        }
                        mark(14);
                        if (s_grassFound < 255u) {
                            s_grassFound++;
                        }
                        void* mt = *reinterpret_cast<void**>(tf);
                        if (mt == nullptr || !Plausible(mt)) {
                            continue;
                        }
                        void* vt = *reinterpret_cast<void**>(
                            static_cast<char*>(mt) + 8);
                        if (vt == nullptr || !Plausible(vt)) {
                            continue;
                        }
                        void** slots = reinterpret_cast<void**>(vt);
                        (void)slots;
                        /* v297: USE THE SLOT, NOT A BODY SCAN.
                         *
                         * The 256-slot scan for the Grass body was a type test
                         * that cannot work: vtables sit next to each other in
                         * .data, so scanning past the end of a Tree's or a
                         * HoeDirt's table walks straight into Grass's and finds
                         * the body there.  Every terrain feature "matched", we
                         * called Grass code on objects that are not Grass, and
                         * `tf+0x48` -- numberOfWeeds only for a real Grass --
                         * came back as garbage, which is why the cut never
                         * happened and `cut` stayed 00.
                         *
                         * performToolAction is a fixed virtual slot.  Vanilla
                         * MeleeWeapon.DoDamage calls it as
                         *     0x1b5b510  ldr x9,[x8,#0x120]
                         *     0x1b5b514  ldrb w8,[x8,#0x128]   (adjustor)
                         * so slot 0x120 of the vtable IS performToolAction for
                         * every TerrainFeature.  Reading that one pointer is an
                         * exact type check: it equals the Grass body only for a
                         * Grass. */
                        void* fn = *reinterpret_cast<void**>(
                            static_cast<char*>(vt) + kTfToolActionSlotOff);
                        bool isGrass = (fn == grassBody);
                        if (isGrass) {
                            if (s_grassSlot < 255u) {
                                s_grassSlot++;
                            }
                        }
                        else {
                            /* Slot says "not Grass", but trust nothing blindly:
                             * a Grass has grassType 1..7 at +0x30 and a
                             * numberOfWeeds NetInt at +0x48.  Both are metadata
                             * offsets, and both must look sane before we touch
                             * anything. */
                            void* gtRef = *reinterpret_cast<void**>(
                                static_cast<char*>(tf) + 0x30);
                            void* wRef = *reinterpret_cast<void**>(
                                static_cast<char*>(tf) + 0x48);
                            if (gtRef != nullptr && Plausible(gtRef) &&
                                wRef != nullptr && Plausible(wRef)) {
                                const int gt = *reinterpret_cast<unsigned char*>(
                                    static_cast<char*>(gtRef) + kNetValueOff2);
                                const int w = *reinterpret_cast<int*>(
                                    static_cast<char*>(wRef) + kNetValueOff2);
                                if (gt >= 1 && gt <= 7 && w >= 0 && w <= 200) {
                                    isGrass = true;
                                    if (s_grassField < 255u) {
                                        s_grassField++;
                                    }
                                }
                            }
                        }
                        if (!isGrass) {
                            continue;
                        }
                        fn = grassBody;      /* always the known body */
                        if (fn == nullptr) {
                            continue;              /* not a Grass */
                        }
                        mark(15);
                        /* v267: damage must be > 0.
                         *
                         * Grass.performToolAction 0x1b16390 branches on it
                         * immediately:
                         *     1b1642c  cmp  w22, #0      ; w22 = damage
                         *     1b16434  b.gt 1b16440      ; damage > 0 -> permissive path
                         *     1b16438  cmp  w8, #2
                         *     1b1643c  b.eq 1b165d8      ; damage <= 0 && w8 == 2 -> SKIP
                         * v265/v266 passed 0 (copied from Hoe.DoFunction, but that
                         * call was to a different slot) and got the skipping path.
                         *
                         * Also point Farmer.lastClick (+0xFC, a Vector2 of PIXELS)
                         * at this tile first: while riding it still holds whatever
                         * it held before, which is why a manual press only cuts
                         * some of the time. */
                        *reinterpret_cast<float*>(static_cast<char*>(pl) + 0xFC) =
                            key.x * 64.0f + 32.0f;
                        *reinterpret_cast<float*>(static_cast<char*>(pl) + 0x100) =
                            key.y * 64.0f + 32.0f;
                        /* explosion = 0, like vanilla (MeleeWeapon.DoDamage
                         * passes 0).  The C# condition is
                         *     (meleeWeapon != null && type != 2) || explosion > 0
                         * so a scythe (type != 2) takes the branch even with 0;
                         * passing 1 only changes how many weeds come off (2-3
                         * instead of 1) and touches Game1.recentMultiplayerRandom
                         * for nothing.  v267 read the `cmp w22,#0 / b.gt` as
                         * "damage must be > 0" and missed the other entry. */
                        auto callCut =
                            reinterpret_cast<bool (*)(void*, void*, int, Vec2, void*)>(fn);
                        /* v294: KEEP CUTTING UNTIL THE GRASS IS GONE.
                         *
                         * Grass.performToolAction does `numberOfWeeds -= 1`
                         * (2 for the golden scythe "53", 4 for the iridium one
                         * "66") and then `return TryDropItemsOnCut(t)`, which
                         * is where hay comes from:
                         *
                         *     if (numberOfWeeds.Value <= 0) {
                         *         if (grassType != 1 && grassType != 7)  ... random drops
                         *         else if (tool.isScythe())              ... hay
                         *     }
                         *     return false;      // <-- still weeds left: NO hay
                         *
                         * A vanilla scythe swing calls this many times per frame
                         * for as long as the animation lasts, so a 3-weed tuft
                         * always reaches 0 within one swing.  We call it once
                         * per tile the tractor moves onto, so anything above
                         * one weed never reached 0 -- and that is why the silo
                         * stayed empty.
                         *
                         * numberOfWeeds is the field performToolAction decrements
                         * at 0x1b16578 and TryDropItemsOnCut tests at 0x1b166f4.
                         *
                         * v296: STOP ASKING, START DOING.
                         *
                         * Three builds in a row (294, 295) relied on
                         * performToolAction eventually answering true, and the
                         * hardware readouts say it never does: `cut` stays 00
                         * while `calls` barely moves.  Whatever the reason, the
                         * return value is not something we can steer.
                         *
                         * So do exactly what the decompiled C# does, without
                         * asking it to agree:
                         *
                         *   numberOfWeeds -= 1;        // +0x48, a NetInt, value
                         *                              // at +0x68 (metadata, and
                         *                              // both vanilla functions
                         *                              // read it there)
                         *   TryDropItemsOnCut(tool);   // 0x1b16670, the drop
                         *   terrainFeatures.Remove     // 0xdd0950
                         *
                         * One real call still goes in first, purely for the
                         * swipe sound and the destroy sprites; if it happens to
                         * cut, its own drop already ran and we leave it alone --
                         * that is the guard against dropping hay twice. */
                        bool cut = callCut(tf, tool, 0, key, here);
                        if (s_scytheCalls < 255u) {
                            s_scytheCalls++;
                        }
                        if (!cut) {
                            void* wref = *reinterpret_cast<void**>(
                                static_cast<char*>(tf) + 0x48);
                            if (wref != nullptr && Plausible(wref)) {
                                int* weeds = reinterpret_cast<int*>(
                                    static_cast<char*>(wref) + kNetValueOff2);
                                if (*weeds > 0) {
                                    *weeds = 0;
                                    reinterpret_cast<bool (*)(void*, void*, int)>(
                                        g_base + kGrassDropOff)(tf, tool, 1);
                                }
                                cut = true;
                            }
                        }
                        if (cut) {
                            /* v273 -- the step we were missing.  Cutting a
                             * TerrainFeature only decrements its internals; the
                             * grass disappears because vanilla then removes it
                             * from the dictionary (1b5b544 bl 0xdd0950).
                             * Without this the tile stayed covered. */
                            reinterpret_cast<bool (*)(void*, Vec2)>(
                                g_base + kTerrainRemoveOff)(nd, key);
                            if (s_grassCut < 255u) {
                                s_grassCut++;
                            }
                            applied++;
                        }
                    }
                }
            }
            if (applied == 0) {
                /* v266 FALLBACK + DIAGNOSTIC.
                 *
                 * If the terrainFeatures route cut nothing, fall back to the
                 * path that is KNOWN to cut grass: press the button by hand
                 * (hide the mount, run the real pressUseToolButton, put the
                 * mount back -- exactly what HookPressUseTool does).  That
                 * makes this build self-reporting:
                 *
                 *   no swing, grass cut      -> the terrainFeatures route works
                 *   swing, grass cut         -> the route failed, this is the
                 *                               fallback; the dictionary side is
                 *                               the broken half
                 *   swing, grass NOT cut     -> the branch is not running at
                 *                               all (meleeTool is false, or
                 *                               AutoTractorUse bailed earlier)
                 *
                 * Deliberately NO movement unlock: v263 proved that wedges
                 * usingTool and kills auto-use. */
                void* mountRef = *reinterpret_cast<void**>(
                    static_cast<char*>(pl) + kFarmerNetMountOff);
                if (mountRef != nullptr && Plausible(mountRef)) {
                    void** slot = reinterpret_cast<void**>(
                        static_cast<char*>(mountRef) + kNetValueOff2);
                    void* horse = *slot;
                    if (IsOurTractor(horse)) {
                        *slot = nullptr;
                        s_forceTractorPower = true;
                        if (s_origPressUseTool != nullptr) {
                            s_origPressUseTool();
                        }
                        s_forceTractorPower = false;
                        *slot = horse;
                    }
                }
            }
        } else {
            mark(12);
            {
                unsigned char* sounds = reinterpret_cast<unsigned char*>(
                    static_cast<char*>(tool) + kToolPlaySoundsOff);
                const unsigned char savedSounds = *sounds;
                auto getStamina = reinterpret_cast<float (*)(void*)>(
                    g_base + kGetStaminaOff);
                auto setStamina = reinterpret_cast<void (*)(void*, float)>(
                    g_base + kSetStaminaOff);
                const float staminaBefore = getStamina(pl);
                bool first = true;
                for (int dy = -kTractorRadius; dy <= kTractorRadius; ++dy) {
                    for (int dx = -kTractorRadius; dx <= kTractorRadius; ++dx) {
                        /* silence every call after the first, like the PC mod */
                        if (!first) {
                            *sounds = 0;
                        }
                        callTool(tool, here, (tx + dx) * 64 + 32,
                                 (ty + dy) * 64 + 32, 1, pl);
                        first = false;
                    }
                }
                *sounds = savedSounds;
                /* v396: no stamina cost at all.  Each of the nine DoFunctions
                 * deducts its own (2*power - FarmingLevel*0.1), so the balance
                 * is simply put back.  v274 charged one swing instead of nine;
                 * this charges none.
                 *
                 * v397: and only put it back when it actually went down.  A
                 * snapshot taken before the net field has synced reads 0, and
                 * writing that back would empty a full bar. */
                if (!first && getStamina(pl) < staminaBefore) {
                    setStamina(pl, staminaBefore);
                }
            }
        }
    }

    /* -1 = not probed yet, 0 = vanilla 224x128 sheet, 1 = tractor rows present */
    static int s_skinRows = -1;

    /* v276 -- keep the per-frame cost near zero.
     *
     * v275 looked the horse up by walking GameLocation.characters on EVERY
     * frame: up to 512 elements, each one through IsOurTractor, which is two
     * Plausible() calls, and Plausible is an svcQueryMemory syscall.  That is
     * thousands of syscalls a second for nothing, and it runs while standing
     * still in a menu just as much as while driving.
     *
     * Now the walk only happens when the location pointer changes (plus a
     * 300-frame safety net), the sprite is resolved once per horse, and the
     * rect is rewritten only when the animation frame actually moved. */
    static void* s_scanLoc = nullptr;
    static int s_scanCountdown = 0;
    static void* s_fixHorse = nullptr;
    static void* s_fixSprite = nullptr;
    static int s_fixFrame = -1;

    bool SkinHasTractorRows(void* sprite) {
        if (s_skinRows >= 0) {
            return s_skinRows != 0;
        }
        const int h = reinterpret_cast<int (*)(void*)>(g_base + kTextureHeightOff)(sprite);
        s_skinRows = (h >= 256) ? 1 : 0;
        LOGF("tractor: sheet height %d -> %s", h,
             s_skinRows != 0 ? "tractor rows present"
                             : "vanilla sheet, romfs skin not installed");
        return s_skinRows != 0;
    }

    void* CurrentTractorHorse() {
        void* pl = reinterpret_cast<void* (*)(void*)>(g_base + kGetPlayerOff)(nullptr);
        if (pl != nullptr && Plausible(pl)) {
            void* mountRef = *reinterpret_cast<void**>(
                static_cast<char*>(pl) + kFarmerNetMountOff);
            if (mountRef != nullptr && Plausible(mountRef)) {
                void* horse = *reinterpret_cast<void**>(
                    static_cast<char*>(mountRef) + kNetValueOff2);
                /* Already known to be ours?  Skip the Guid compare. */
                if (horse != nullptr && horse == s_tractor) {
                    return horse;
                }
                if (IsOurTractor(horse)) {
                    s_tractor = horse;
                    return horse;
                }
            }
        }
        if (s_tractor != nullptr && Plausible(s_tractor) && IsOurTractor(s_tractor)) {
            return s_tractor;
        }
        /* Not riding and nothing cached (e.g. right after loading a save).
         * Walking the character list is expensive, so do it only when the
         * location actually changed. */
        void* loc = reinterpret_cast<void* (*)(void*)>(g_base + kGetLocNowOff)(nullptr);
        if (loc == nullptr || !Plausible(loc)) {
            return nullptr;
        }
        if (loc == s_scanLoc && s_scanCountdown > 0) {
            s_scanCountdown--;
            return nullptr;
        }
        s_scanLoc = loc;
        s_scanCountdown = 300;
        void* found = FindTractorIn(loc);
        if (found != nullptr) {
            s_tractor = found;
        }
        return found;
    }

    void FixTractorSprite() {
        void* horse = CurrentTractorHorse();
        if (horse == nullptr) {
            s_fixHorse = nullptr;
            s_fixSprite = nullptr;
            s_fixFrame = -1;
            return;
        }
        if (horse != s_fixHorse) {
            s_fixHorse = horse;
            s_fixSprite = nullptr;
            s_fixFrame = -1;
            void* ref = *reinterpret_cast<void**>(
                static_cast<char*>(horse) + kCharSpriteRefOff);
            if (ref != nullptr && Plausible(ref)) {
                void* sp = *reinterpret_cast<void**>(
                    static_cast<char*>(ref) + kNetValueOff2);
                if (sp != nullptr && Plausible(sp)) {
                    s_fixSprite = sp;
                }
            }
        }
        if (s_fixSprite == nullptr) {
            return;
        }
        if (!SkinHasTractorRows(s_fixSprite)) {
            return;
        }
        int f = *reinterpret_cast<int*>(
            static_cast<char*>(s_fixSprite) + kSpriteFrameOff);
        if (f < 0) {
            f = 0;
        }
        if (f > 27) {          /* the horse never uses more than 4 rows */
            f = 27;
        }
        if (f == s_fixFrame) {
            return;            /* the rect for this frame is already in place */
        }
        s_fixFrame = f;
        *reinterpret_cast<unsigned char*>(
            static_cast<char*>(s_fixSprite) + kSpriteIgnoreRectOff) = 1;
        int* rect = reinterpret_cast<int*>(
            static_cast<char*>(s_fixSprite) + kSpriteSrcRectOff);
        rect[0] = (f % 7) * 32;
        rect[1] = (f / 7 + kTractorRowShift) * 32;
        rect[2] = 32;
        rect[3] = 32;
    }

    /* Give our horse the tractor icon on the animals page.
     *
     * FindAnimals builds one AnimalEntry per animal and returns the list, so we
     * let it finish and then walk the finished entries.  Every entry whose
     * Animal is our horse gets its TextureSourceRect pushed down into the
     * tractor half of the sheet (Y + 128), which is the whole trick: the entry
     * samples `horse.Sprite.Texture` at (0, H*2-26, W, 24), i.e. y = 38.
     *
     * The page can hold the horse twice (once from the location list, once from
     * Farmer.mount) and both are the same object, so both get fixed.
     *
     * v288: matching by pointer alone was not enough.  Right after a save is
     * loaded the entry could hold a Horse object that is not the one we cached
     * (the page showed a horse until the tractor had been summoned once), so
     * the fallback below accepts any entry whose Animal has the same runtime
     * type AND the same HorseId Guid.  Reading HorseId out of a chicken is what
     * the type check prevents -- MethodTable is a single deref of a live
     * pointer, and only a real Horse can reach the Guid read. */
    static void* (*s_origFindAnimals)(void*) = nullptr;

    void* HookFindAnimals(void* self) {
        void* list = s_origFindAnimals != nullptr ? s_origFindAnimals(self) : nullptr;
        const bool first = (s_iconCalls++ == 0);
        if (!kTractor || g_tractorOn == 0 || list == nullptr || !Plausible(list)) {
            return list;
        }
        /* v288: resolve the horse HERE instead of trusting s_tractor.
         *
         * Reported symptom: on a freshly loaded save the animal page still
         * shows a horse until the tractor is summoned once.  s_tractor is only
         * filled in by FixTractorSprite on a game update, and the page can be
         * built from a path that runs before that resolution -- or against a
         * Horse object we have not cached yet.  Calling CurrentTractorHorse()
         * here is safe: FindAnimals is ordinary managed code, not the renderer,
         * so calling Game1.get_player / get_currentLocation from it is exactly
         * what the game itself does one frame later. */
        void* horse = s_tractor;
        if (horse == nullptr || !Plausible(horse)) {
            horse = CurrentTractorHorse();
        }
        if (horse == nullptr) {
            /* CurrentTractorHorse throttles its list walk, so one call is not a
             * guarantee.  Walk the current location directly -- this hook runs
             * once per page build, not per frame, so the cost is irrelevant. */
            void* loc = reinterpret_cast<void* (*)(void*)>(g_base + kGetLocNowOff)(nullptr);
            if (loc != nullptr && Plausible(loc)) {
                horse = FindTractorIn(loc);
            }
            if (horse == nullptr) {
                return list;
            }
            s_tractor = horse;
            LOGF("tractor: animal page resolved the horse on its own");
        }
        if (first) {
            s_iconHorse = 1;
            /* Same two hops FixTractorSprite uses: Character.Sprite is a NetRef
             * (+0x50 -> +0x68), and get_TextureHeight takes the AnimatedSprite,
             * not the Character. */
            void* sref = *reinterpret_cast<void**>(
                static_cast<char*>(horse) + kCharSpriteRefOff);
            if (sref != nullptr && Plausible(sref)) {
                void* sp = *reinterpret_cast<void**>(
                    static_cast<char*>(sref) + kNetValueOff2);
                if (sp != nullptr && Plausible(sp)) {
                    s_iconTexH = static_cast<unsigned>(
                        reinterpret_cast<int (*)(void*)>(g_base + kTextureHeightOff)(sp));
                }
            }
        }
        void* store = *reinterpret_cast<void**>(
            static_cast<char*>(list) + kTrkListStoreOff);
        if (store == nullptr || !Plausible(store)) {
            return list;
        }
        const int n = *reinterpret_cast<int*>(static_cast<char*>(list) + kTrkListSizeOff);
        void* buf = *reinterpret_cast<void**>(static_cast<char*>(store) + kTrkStoreBufOff);
        if (buf == nullptr || !Plausible(buf) || n <= 0 || n > 64) {
            return list;
        }
        void** elems = reinterpret_cast<void**>(buf);
        /* Same runtime type as our horse -- a single deref of a valid object
         * pointer, and the gate that makes the HorseId read below safe for
         * entries belonging to chickens, pets and cows. */
        void* horseMT = *reinterpret_cast<void**>(horse);
        int fixed = 0;
        int byPtr = 0;
        int byGuid = 0;
        int mtMatch = 0;
        int y0 = -1;
        for (int i = 0; i < n; i++) {
            void* entry = elems[i];
            if (entry == nullptr || !Plausible(entry)) {
                continue;
            }
            void* animal = *reinterpret_cast<void**>(
                static_cast<char*>(entry) + kAnimalEntryAnimalOff);
            if (animal == nullptr || !Plausible(animal)) {
                continue;
            }
            const bool sameType =
                (horseMT != nullptr && *reinterpret_cast<void**>(animal) == horseMT);
            if (sameType) {
                ++mtMatch;
            }
            if (animal == horse) {
                ++byPtr;
            }
            else {
                if (!sameType || !IsOurTractor(animal)) {
                    continue;
                }
                ++byGuid;
            }
            int* rect = reinterpret_cast<int*>(
                static_cast<char*>(entry) + kAnimalEntryRectOff);
            if (y0 < 0) {
                y0 = rect[1];
            }
            /* Idempotent: 38 (horse half) -> 166 (tractor half).  Anything
             * already in the tractor half is left alone, so a cached list that
             * FindAnimals hands back a second time is not shifted twice. */
            if (rect[1] < kTractorRowShift * 32) {
                rect[1] += kTractorRowShift * 32;
                ++fixed;
            }
        }
        if (first) {
            s_iconN = static_cast<unsigned>(n);
            s_iconMt = static_cast<unsigned>(mtMatch);
            s_iconPtr = static_cast<unsigned>(byPtr);
            s_iconGuid = static_cast<unsigned>(byGuid);
            s_iconFixed = static_cast<unsigned>(fixed);
            s_iconY0 = static_cast<unsigned>(y0 < 0 ? 0xFF : y0);
        }
        /* First few builds only: the page is rebuilt whenever the animal set
         * changes, and a hot log line here would bury the rest of the file. */
        static int s_iconLogs = 0;
        if (s_iconLogs < 8) {
            ++s_iconLogs;
            LOGF("tractor: animal page n=%d mt=%d ptr=%d guid=%d fixed=%d", n,
                 mtMatch, byPtr, byGuid, fixed);
        }
        return list;
    }

    static void (*s_origGameUpdate)(void*, void*) = nullptr;

    /* v343: open the file store now that the process is fully up.
     *
     * exl_main runs during rtld module initialisation.  Opening the store there
     * is what made the whole file channel a silent no-op on retail: the log
     * file never appeared and ReadSetting() always returned -1, with no way to
     * tell why (see the note in exl_main).  The plan recorded there was to do
     * it on the first HUD draw via a function called EnsureSettings() -- that
     * function was never written, which is why sdlog::Open() had no caller at
     * all.  This is that call.
     *
     * Retried a few times: the first attempt can land while the process still
     * holds every session slot, and each attempt repeats only the step that
     * failed (sdlog keeps g_fs_ready). */
    void EnsureSettings() {
        if (sdlog::IsOpen()) {
            return;
        }
        /* Retried forever, slowly.  The failure is a service-session problem in
         * this process, so the only way in is to catch a moment when a slot is
         * free (e.g. while the game has just torn something down).  Each retry
         * repeats only the step that failed -- sdlog keeps g_fs_ready. */
        if (s_logTry >= 20u) {
            return;                 /* given up: fs is not reachable here */
        }
        if ((s_logFrame++ % 600u) != 0u && s_logTry != 0u) {
            return;
        }
        if (s_logTry < 0xFFFFu) {
            ++s_logTry;
        }
        if (sdlog::Open()) {
            LOGF("log: open ok channel=%d", sdlog::Channel());
        }
    }

    /* Defined further down with the rest of the HUD.  Declared here because
     * the update hook calls it, and it must stay out of the draw path: see the
     * comment on the definition. */
    void BuildHudStrings();

    void HookGameUpdate(void* self, void* gameTime) {
        EnsureSettings();
        if (s_saveAskFrame < 0xFFFFu) {
            ++s_saveAskFrame;      /* v379: throttle counter for switch saves */
        }
        /* v346: flush the trace into the preferences file when it changes, at
         * most once every 5 s.  g_saveWanted is consumed by TryPerformSave()
         * from the Update-time animation hook, which is the only place the
         * async save is known to complete without freezing. */
        /* v349: a flat 2 s, never shorter.
         *
         * v348 flushed 0.5 s after an action, which on a busy farm can be
         * twice a second.  Combined with "a pass also marks dirty" (v347) the
         * file was being rewritten continuously.  Now only real actions mark
         * dirty AND the gap never goes below 2 s, so this writes strictly less
         * often than v347 -- which ran without the black screen. */
        /* v378: OFF.  The crash this trace existed for was found and fixed in
         * v357, and while automation runs TraceStep() marks it dirty on every
         * action -- so this block was asking the game to SAVE every 2 seconds.
         * docs/FEATURES.md 3.3 records exactly that pattern as the source of
         * Switch error 2002-0007 ("后台 Task 出错，主线程还活着"): v310 did the
         * same thing per click and had to be removed in v311.  The trace value
         * still lives in RAM for the readout; it just never goes to disk. */
        /* v383: NO save request, ever.  Hardware, twice:
         *   v379 (request from the click handler) -> 2002-0007 + black screen
         *   v382 (request from this update phase, 2 s later, once) -> the same
         * So on this build any TryPerformSave() we issue breaks the save path.
         * The consequence is accepted: switch changes only reach the file when
         * the game writes it by itself, so a switch may come back at its
         * default after a restart.  Stability first. */
        if (kTraceFlush && s_traceDirty && ++s_traceFrame >= 120u) {
            s_traceFrame = 0;
            s_traceDirty = 0;
            s_traceHot = 0;
            g_saveWanted = 1;
            /* Consumed here rather than left to the animation hook: while the
             * player stands still nothing may be animating, and then the save
             * would never fire -- exactly the situation this trace exists for.
             * This is the same update phase the animation hook runs in, which
             * is the only place the async save is known to complete. */
            TryPerformSave();
        }
        if (kTractor && !kDiagNoAuto && g_tractorOn != 0) {
            AutoTractorUse();
        }
        /* v293: sampled here because the draw path must not call managed code.
         * Game1.get_CurrentEvent is a plain static getter. */
        s_eventActive =
            reinterpret_cast<void* (*)(void*)>(g_base + kGetCurrentEventOff)(nullptr)
                != nullptr ? 1u : 0u;
        /* v410: same reason -- get_DailyLuck is managed, so the number is
         * taken here (once a second) and the HUD only draws the copy. */
        if (kHudInfo) {
            SampleDailyLuck();
            if (++s_toolFrame >= 60) {
                s_toolFrame = 0;
                SampleToolUpgrade();
            }
            /* Strings are built here, not in the draw hook: ManagedStr
             * allocates a managed object, and allocating while the game is
             * mid-draw can trigger a collection that locks the console up. */
            BuildHudStrings();
        }
        if (s_origGameUpdate != nullptr) {
            s_origGameUpdate(self, gameTime);
        }
        /* v392: the switch mask <-> Farmer.disconnectDay.
         *
         * The pending write goes FIRST.  HookOptionsPageDraw only raises the
         * flag (it must not call CarryPlayer -- that is a managed getter, and
         * this project has frozen the console before by calling managed code
         * from a render hook).  Writing here also means a read-back below can
         * never undo a toggle that has not reached the field yet.
         *
         * The read-back is throttled to once a second; the farm save restores
         * the field before the first Update, so a loaded farm is picked up on
         * the first pass. */
        if (s_discDirty != 0) {
            s_discDirty = 0;
            FlushMaskToDisconnectDay();
        }
        if (s_discFrame == 0) {
            ApplyDisconnectDayMask();
        }
        if (s_discFrame < 60u) {
            ++s_discFrame;
        }
        else {
            s_discFrame = 0;
        }
        /* v337: run after the vanilla update so machine timers have already
         * advanced for this tick -- the same ordering PC Automate uses. */
        if (kAutomate) {
            AutomateTick();
        }
        /* After the game has advanced every sprite, before it draws: this is
         * the only window where the source rect we write will be the one used.
         * Doing it in the draw path would mean calling managed code from the
         * renderer, which is what froze the console back in GameMenu.draw. */
        if (kTractor && !kDiagNoFix && g_tractorOn != 0) {
            FixTractorSprite();
        }
        if (kTractor && !kDiagNoAuto && g_tractorOn != 0) {
            MaintainTractorPresence();
        }
    }

    int HookStringEquals(void* a, void* b) {
        if (kDiagNoStr) {
            return s_origStrEquals != nullptr ? s_origStrEquals(a, b) : 0;
        }
        /* v235: the lie is confined to the icon lookup by RETURN ADDRESS.
         *
         * v234 relied on IsSpecialName() to keep the borrowed name out of the
         * other content comparisons in GameMenu.  That worked, but it also
         * meant we could not borrow a name whose icon we actually wanted (the
         * settings tab's, for instance).
         *
         * Every one of those other comparisons lives outside GameMenu.draw's
         * icon-lookup block, and this hook can see where it was called from:
         * the game reaches the lookup through `bl 0x3943f0` at 0x17987A0 ..
         * 0x1798A24, all inside 0x1798790..0x1798A40.  Anywhere else we defer
         * to the real function, so a borrowed name can never change how the
         * tab is clicked, drawn elsewhere, or resolved to a number.
         *
         * The comparison itself is still two pointer compares -- no allocation,
         * nothing managed, and a stale static can only make it fail. */
        if (a != nullptr && b != nullptr && s_modTabName != nullptr &&
            s_modIconTarget != nullptr) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(
                __builtin_return_address(0));
            const uintptr_t rel = ret - g_base;
            if (rel >= kIconLookupLo && rel < kIconLookupHi) {
                if ((a == s_modTabName && b == s_modIconTarget) ||
                    (b == s_modTabName && a == s_modIconTarget)) {
                    return 1;
                }
            }
        }
        if (s_origStrEquals != nullptr) {
            return s_origStrEquals(a, b);
        }
        return 0;
    }

    /* v211: ShippingMenu.update pass-through.
     *
     * Runs in the normal Update pass on the main thread, so Plausible /
     * RangeReadable are safe here (they are NOT on a task thread -- see the
     * HookWriteSettings note).  No allocation, no managed call, no re-entry:
     * the original runs exactly once and we only adjust one integer after it
     * returns. */
    void HookShippingUpdate(void* self, void* gameTime) {
        if (s_origShippingUpdate != nullptr) {
            s_origShippingUpdate(self, gameTime);
        }
        if (g_animFast <= 1 || self == nullptr || !Plausible(self) ||
            !RangeReadable(self, kShippingTimerOff + 4u)) {
            return;
        }
        const unsigned char* b = reinterpret_cast<const unsigned char*>(self);
        /* Only while the intro is actually running: activated, not finished. */
        if (b[kShippingActivatedOff] == 0 || b[kShippingFinishedOff] != 0) {
            return;
        }
        int* timer = reinterpret_cast<int*>(
            const_cast<unsigned char*>(b) + kShippingTimerOff);
        const int t = *timer;
        /* Armed to 2000 ms, so anything outside a sane range means this is not
         * the countdown we think it is. */
        if (t < 1 || t > 60000) {
            return;
        }
        int nudged = t - 16 * (g_animFast - 1);
        /* Never drive it below 1.  The original skips its own expiry branch
         * when the timer is already < 1 (`cmp w21,#1; b.lt`), so leaving it at
         * 1 lets the game finish the phase itself on the next tick instead of
         * us stalling it. */
        if (nudged < 1) {
            nudged = 1;
        }
        *timer = nudged;
    }

    /* v206 read-side probes.  Both are pass-through, allocation-free and
     * non-blocking -- they run on the boot thread, long before the HUD. */
    void HookReadSettings(void* self, void* reader) {
        if (s_evtRead == 0xFFu) {
            s_evtRead = s_evtSeq & 0xFFu;
        }
        s_evtSeq = s_evtSeq + 1u;
        if (s_origReadSettings != nullptr) {
            s_origReadSettings(self, reader);
        }
        /* Sampled AFTER the original on purpose.  Before it runs, +0x50 still
         * holds the constructor's own `new Options()`, whose +0x24 is 0 --
         * that is exactly the false positive this probe has to avoid.
         *
         * v208: no Plausible()/RangeReadable() here either, and that is not a
         * loosening -- it is the same trust the original function already
         * places.  readSettings writes `strb w8,[x19,#0x10]` straight through
         * `self` with no check, so a bad `self` crashes the game before this
         * hook can matter.  And +0x50 has exactly three writers (ctor -> new
         * Options(), 0x181A1A8 -> get_options(), 0x1ADFD4C -> the deserialised
         * copy), so after the original returns it is a valid Options or null.
         * A null is checked; a valid one is safe to read.  The `v <= 1` test
         * still discards anything that is not a bool. */
        if (self != nullptr) {
            void* co = *reinterpret_cast<void**>(
                reinterpret_cast<unsigned char*>(self) + kPrefsOptsOff);
            if (co != nullptr) {
                const unsigned v = *reinterpret_cast<unsigned char*>(
                    reinterpret_cast<unsigned char*>(co) + kOptFlagOff);
                if (v <= 1u) {
                    s_fileBit = v;
                    /* v209: from this moment the file is the source of truth
                     * for the install.  Set before the boot wiring reaches
                     * set_options, which is strictly later (0x178ADA8 ->
                     * 0x178ADC4). */
                    s_applyTarget = v;
                }
                /* v314: the switch bitmap, out of preferredResolutionX.
                 *
                 * Applied here and not at the first menu open: the gates in
                 * AutoTractorUse read g_rowOn, and a save loaded and driven off
                 * in must not keep the previous session's switches.  g_savedMask
                 * is seeded from the same value so a save taken before the menu
                 * is ever opened cannot write defaults over it.
                 *
                 * No tag means "not ours" (first run, or a resolution the game
                 * itself stored) -- leave everything on. */
                {
                    const unsigned char* cp =
                        reinterpret_cast<const unsigned char*>(co);
                    unsigned v = 0;
                    for (int b = 0; b < 4; b++) {
                        v |= static_cast<unsigned>(cp[kMaskOff + b]) << (8 * b);
                    }
                    if ((v & kMaskTag) != 0) {
                        s_fileMask = v & kMaskBits;
                        s_fileMaskOk = 1;
                        g_savedMask = kMaskTag | s_fileMask;
                        for (int i = 1; i < kRowCount; i++) {
                            g_rowOn[i] = (s_fileMask >> i) & 1u;
                        }
                        g_tractorOn = g_rowOn[kRowMaster];
                        g_mapOn = g_rowOn[kRowMap];
                    }
                }
            }
        }
    }

    /* v313: put back the Options bytes v312 seeded away from their defaults.
     *
     * v312 borrowed ten bools for the switches and seeded them to 1.  Three of
     * them are not inert on a console, and one is fatal to the UI: +0x2F is
     * `_hardwareCursor`, and Game1 only draws the finger cursor when it is
     * FALSE (`if (!freezeControls && !options.hardwareCursor)`).  Seeding it to
     * 1 hid the cursor on every screen that draws one.
     *
     * Runs on the live Options as soon as the game installs it, which is before
     * the first frame, so the repair does not depend on the player opening the
     * menu.  +0x27 is the marker: 1 = seeded by v312, 2 = repaired. */
    void RepairBorrowedFields(void* opts) {
        if (opts == nullptr) {
            return;
        }
        unsigned char* p = reinterpret_cast<unsigned char*>(opts);
        if (p[0x27u] >= 2u) {
            return;
        }
        p[0x2Fu] = 0;      /* _hardwareCursor           default false */
        p[0x29u] = 0;      /* showClearBackgrounds      default false */
        p[0x3Cu] = 0;      /* useLegacySlingshotFiring  default false */
        p[0x27u] = 2u;     /* marker */
        p[kOptDirtyOff] = 1;
    }

    void HookSetOptions(void* opts) {
        if (s_evtSetOpt == 0xFFu) {
            s_evtSetOpt = s_evtSeq & 0xFFu;
        }
        s_evtSeq = s_evtSeq + 1u;
        /* set_options is a bare store (`str x0,[x8,#0x90]`), so it cannot
         * modify the object; reading before the original is equivalent and
         * keeps the value even if a later build changes the shape.
         *
         * v208: no Plausible() -- same reason as HookReadSettings.  The
         * original stores `opts` into Game1+0x90 with no check, so it already
         * trusts the pointer; its only callers pass prefs+0x50 or
         * new Options().  Null is checked, `v <= 1` filters non-bools. */
        if (s_setOptCalls < 15u) {
            s_setOptCalls = s_setOptCalls + 1u;
        }
        if (opts != nullptr) {
            RepairBorrowedFields(opts);
            unsigned char* p = reinterpret_cast<unsigned char*>(opts) +
                               kOptFlagOff;
            const unsigned v = *p;
            if (v <= 1u) {
                if (s_appliedBit == 0xFu) {
                    s_appliedBit = v;
                }
                /* The install that matters is the one after the file has been
                 * read; anything earlier cannot know the player's choice. */
                if (s_evtRead != 0xFFu && s_appliedAfterRead == 0xFu) {
                    s_appliedAfterRead = v;
                }
                /* THE FIX.  Written before the original stores the pointer into
                 * Game1+0x90, so the live Options carries the file's value from
                 * the very first frame.  Guarded by s_applyTarget <= 1, which is
                 * only true once readSettings has actually produced a value. */
                if (s_applyTarget <= 1u && *p != s_applyTarget) {
                    *p = static_cast<unsigned char>(s_applyTarget);
                    if (s_forced < 15u) {
                        s_forced = s_forced + 1u;
                    }
                }
            }
        }
        if (s_origSetOptions != nullptr) {
            s_origSetOptions(opts);
        }
    }

    void HookOptionsPageDraw(void* self, void* batch) {
        s_origOptionsDraw(self, batch);
        if (kDiagNoDraw) {
            return;
        }
        g_optionsPage = self;

        /* The element is read HERE and nowhere else.
         *
         * g_ourElem points into this page's own options list, and the page is
         * destroyed when the menu closes.  Reading it from the HUD hook meant
         * reading freed memory most of the time: a freed block keeps its
         * MethodTable, so Plausible() passes, and +0x28 / +0x51 then hold
         * whatever object reused the block.  That is what made the readout look
         * like it changed by itself -- row 3 once showed 3FC60000, which is not
         * a whichOption but the UI-scale float at that offset in whatever took
         * the memory.  g_appendedTo == self pins the read to the page the
         * element was actually appended to. */
        if (g_ourElem != nullptr && g_appendedTo == self &&
            Plausible(g_ourElem) && RangeReadable(g_ourElem, 0x58u)) {
            const unsigned which = *reinterpret_cast<unsigned*>(
                reinterpret_cast<unsigned char*>(g_ourElem) + 0x28);
            if (s_elemWhich == 0) {
                /* Latched, not live: this is the one number that has to prove
                 * the ctor argument landed, so it must not be able to drift. */
                s_elemWhich = which;
            }
            s_elemChecked = *reinterpret_cast<unsigned char*>(
                reinterpret_cast<unsigned char*>(g_ourElem) + 0x51);
            s_optFlags |= 4u;   /* latched: an element exists for this page */
        }
        /* v312: every row below the animation one is ours alone -- sentinel
         * `which`, so the only thing that can change a box is a click.  Read
         * them under the same pinning rule as before: g_appendedTo == self
         * proves the pointer still belongs to the page being drawn, which is
         * what keeps a freed element from being read as live. */
        if (g_ourCount > 1 && g_appendedTo == self) {
            for (unsigned i = 1; i < g_ourCount && i < kRowCount; i++) {
                void* el = g_ourElems[i];
                if (el == nullptr || !Plausible(el) ||
                    !RangeReadable(el, 0x58u)) {
                    continue;
                }
                g_rowOn[i] = (*reinterpret_cast<unsigned char*>(
                    reinterpret_cast<unsigned char*>(el) + 0x51) != 0) ? 1u : 0u;
            }
            g_tractorOn = g_rowOn[kRowMaster];
            g_mapOn = g_rowOn[kRowMap];

            /* Stage the values TryPerformSave injects into the serialised copy.
             *
             * v311: deliberately NOT raising g_saveWanted here.  v310 did, and
             * the console threw 2002-0007 right after: the save Task faults and
             * the main thread survives, so the game keeps running with an error
             * dialog behind it.  The request is unnecessary anyway -- the game
             * saves the preferences itself when the menu is left and when it
             * quits, and the injection runs on every save it takes part in. */
            if (!s_rowPrevInit) {
                for (unsigned i = 0; i < kRowCount; i++) {
                    s_rowPrev[i] = static_cast<unsigned char>(g_rowOn[i]);
                }
                s_rowPrevInit = 1;
            }
            else {
                bool changed = false;
                for (unsigned i = 1; i < kRowCount && i < g_ourCount; i++) {
                    if (s_rowPrev[i] != static_cast<unsigned char>(g_rowOn[i])) {
                        changed = true;
                    }
                }
                if (changed) {
                    for (unsigned i = 0; i < kRowCount; i++) {
                        s_rowPrev[i] = static_cast<unsigned char>(g_rowOn[i]);
                    }
                    unsigned m = 0;
                    for (int i = 1; i < kRowCount; i++) {
                        if (g_rowOn[i] != 0) {
                            m |= (1u << i);
                        }
                    }
                    g_savedMask = kMaskTag | m;
                    /* v392: hand the write to HookGameUpdate.  This is a render
                     * hook; CarryPlayer() is a managed getter and calling one
                     * from here is the exact pattern that froze the console
                     * before.  One flag, one frame of delay, no risk. */
                    s_discDirty = 1;
                    /* v382: do NOT call TryPerformSave from the click path --
                     * that is what raised 2002-0007 and black-screened sleep
                     * (v379).  Just mark it dirty; HookGameUpdate issues ONE
                     * request two seconds later, from the update phase where
                     * the async save is known to complete. */
                    s_maskDirty = 1;
                    s_maskFrame = 0;
                }
            }
        }
    }

    /* Mount the settings store and restore the checkbox.
     *
     * Called from the first HUD draw, not from exl_main.  Mounting during
     * rtld module init failed on retail (the SD log file never appeared), and
     * the HUD only draws once the world is running, which is the first moment
     * the fs service is reliably usable.  Retried a few times because the very
     * first draw can still land before the game has settled.
     *
     * g_animFast is set here too, not just g_pendingFast: the latch in the
     * animation hook only copies pending -> live when an animation ends, which
     * would leave the first animations at 1x after every boot.  Both are set
     * before any world animation can run, because drawHUD starts a long time
     * before the player can move. */
    /* Sanity-check a candidate Options before writing through it.
     *
     * `*(Game1+0x90)` is a managed pointer.  On the first frames after boot it
     * can be null, stale, or point at something that is not an Options at all --
     * and `*(Game1+0x90)` itself is only as trustworthy as the Game1 pointer we
     * started from.  Writing +0x24 through a wrong pointer corrupts a live
     * object, so the object has to prove its identity:
     *
     *   - readable for the whole 0x170 bytes
     *   - MethodTable pointer lands inside our own module
     *   - the two floats we know real values for are in a sane range:
     *     baseZoomLevel (+0x58) defaults to 1.0, singlePlayerDesiredUIScale
     *     (+0x64) is whatever the player set (1.55 on this save).
     *
     * Written as `!(a > lo && a < hi)` rather than `a <= lo || a >= hi` so that
     * NaN is rejected instead of passing both comparisons.
     */
    bool LooksLikeOptions(void* p) {
        if (p == nullptr || !Plausible(p) || !RangeReadable(p, kOptSize)) {
            return false;
        }
        const uintptr_t mt = *reinterpret_cast<uintptr_t*>(p);
        if (mt < g_base || mt >= g_base + 0x10000000u) {
            return false;
        }
        const float zoom = *reinterpret_cast<float*>(
            reinterpret_cast<unsigned char*>(p) + 0x58);
        const float ui = *reinterpret_cast<float*>(
            reinterpret_cast<unsigned char*>(p) + 0x64);
        if (!(zoom > 0.25f && zoom < 4.0f)) {
            return false;
        }
        if (!(ui > 0.5f && ui < 3.0f)) {
            return false;
        }
        return true;
    }

    /* Is this pointer still a LIVE StartupPreferences?
     *
     * Plausible() only proves the page is mapped, and that is not enough here.
     * The instance is captured in HookLoadPreferences, and the boot path
     *
     *     0x178AD70   prefs = new StartupPreferences()
     *     0x178ADA8   loadPreferences(prefs)
     *     0x178ADC4   Game1.set_options(prefs + 0x50)      <-- the only escape
     *
     * keeps no reference to the wrapper: only its clientOptions reaches the
     * game.  So the wrapper is garbage the moment the GC next runs, and the
     * captured pointer would then point at a recycled block.  Writing
     * clientOptions through it corrupts whatever now lives there, and passing
     * it to savePreferences makes _savePreferences File.Create (truncate!) and
     * then serialise a stranger -- which is how a working
     * startup_preferences can be destroyed by a failed save.
     *
     * v197's readout is consistent with exactly that: clientOptions read back
     * as null (bit 7 = 0) and the save task ended at status 7 = Faulted.  It is
     * equally consistent with a genuine null, so this version verifies the
     * instance instead of assuming either answer.
     *
     * The fingerprint uses only declared fields and only value ranges that no
     * unrelated object is likely to satisfy by accident:
     *
     *   7 bools at 0x10..0x16, 3 bools at 0x58..0x5a, all 0/1
     *   timesPlayed 0..1M, windowMode 0..2, displayIndex -1..16,
     *   playerLimit -1..32, and languageCode either null or a real
     *   module-local string of 1..32 printable ASCII characters.
     */
    bool LooksLikeStartupPrefs(void* p) {
        s_fpFail = 0u;
        if (p == nullptr || !Plausible(p) || !RangeReadable(p, kPrefsSize)) {
            return false;
        }
        unsigned char* b = reinterpret_cast<unsigned char*>(p);
        s_fpFail = 1u;
        const uintptr_t mt = *reinterpret_cast<uintptr_t*>(b);
        if (mt < g_base || mt >= g_base + 0x10000000u) {
            return false;
        }
        s_fpFail = 2u;
        for (unsigned off = 0x10u; off <= 0x16u; ++off) {
            if (b[off] > 1u) {
                return false;
            }
        }
        s_fpFail = 3u;
        if (b[0x58] > 1u || b[0x59] > 1u || b[0x5a] > 1u) {
            return false;
        }
        s_fpFail = 4u;
        const int timesPlayed = *reinterpret_cast<int*>(b + 0x18);
        if (timesPlayed < 0 || timesPlayed > 1000000) {
            return false;
        }
        s_fpFail = 5u;
        const int windowMode = *reinterpret_cast<int*>(b + 0x1c);
        if (windowMode < 0 || windowMode > 2) {
            return false;
        }
        s_fpFail = 6u;
        const int displayIndex = *reinterpret_cast<int*>(b + 0x20);
        if (displayIndex < -1 || displayIndex > 16) {
            return false;
        }
        s_fpFail = 7u;
        const int playerLimit = *reinterpret_cast<int*>(b + 0x28);
        if (playerLimit < -1 || playerLimit > 32) {
            return false;
        }
        s_fpFail = 8u;
        void* lang = *reinterpret_cast<void**>(b + kPrefsLangOff);
        if (lang != nullptr) {
            if (!Plausible(lang) || !RangeReadable(lang, 0x14u)) {
                return false;
            }
            unsigned char* ls = reinterpret_cast<unsigned char*>(lang);
            const int len = *reinterpret_cast<int*>(ls + 0x10);
            if (len < 1 || len > 32) {
                return false;
            }
            if (!RangeReadable(ls + 0x14, static_cast<size_t>(len) * 2u)) {
                return false;
            }
            for (int i = 0; i < len; ++i) {
                const unsigned c = *reinterpret_cast<unsigned short*>(
                    ls + 0x14 + i * 2);
                if (c < 0x20u || c > 0x7Eu) {
                    return false;
                }
            }
        }
        s_fpFail = 0xFu;
        return true;
    }

    /* Pull the Options pointer out of a candidate Game1 instance, or null. */
    void* OptionsFromGame1(void* game1) {
        if (game1 == nullptr || !Plausible(game1) ||
            !RangeReadable(game1, 0x98u)) {
            return nullptr;
        }
        void* p = *reinterpret_cast<void**>(
            reinterpret_cast<unsigned char*>(game1) + 0x90);
        return LooksLikeOptions(p) ? p : nullptr;
    }

    /* Read the animation toggle out of the game's own Options object.
     *
     * v185 did a hand-rolled two-way sync here (+0x51 <-> +0x24), and that was
     * the bug: it passed whichOption = 0, so the element was wired to
     * Options+0x10 (autoRun) and every click wrote the player's run setting
     * while this function tried to copy the element's byte into +0x24 after the
     * fact.  With whichOption = kOptIndex the game does both directions itself
     * -- ctor reads +0x24 into the box, receiveLeftClick writes it back -- so
     * doing it a second time here would only give two writers for one field.
     * All that is left is reading the stored byte.
     *
     * The resolve step stays: the first HUD draws happen before a save is
     * loaded (Options can still be null) and opening a save can replace it.
     */
    /* v204: construct a StartupPreferences we own.
     *
     * Returns false when the live Options is not resolved -- without it the
     * instance would serialise a fresh default Options and wipe every option
     * the player has, which is far worse than not persisting the checkbox.
     *
     * The constructor allocates internally (x0 = 0) and returns the object in
     * x0; it leaves +0x50 = new Options(), so +0x50 is overwritten with the
     * live object here.  Value-type scalars come from s_prefsSnap when one was
     * captured, so windowMode / displayIndex / playerLimit / timesPlayed /
     * fullscreenResolution keep the player's real values instead of 0. */
    bool BuildOwnPrefs() {
        if (g_options == nullptr || !LooksLikeOptions(g_options)) {
            return false;
        }
        void* p = nullptr;
        if (g_ourPrefs != nullptr && LooksLikeStartupPrefs(g_ourPrefs)) {
            p = g_ourPrefs;            /* reuse: do not allocate per frame */
        }
        else {
            /* THE ARGUMENT IS MANDATORY, even though the game passes zero.
             *
             * 0x1ADE8C0 opens with
             *     01ade8cc: mov x19, x0
             *     01ade8d0: cbz x0, #0x1ade958   ; only x0 == 0 allocates
             * so x0 is `this`.  Declaring the function as taking no arguments
             * (which is what v204 did) leaves x0 holding whatever the call
             * site left there; the constructor then treats that garbage as
             * `this` and writes displayIndex/playerLimit/clientOptions into an
             * arbitrary address.  That is the "software was closed because an
             * error occurred" on v204: memory corruption, not a managed
             * exception.  Passing nullptr explicitly is what makes the
             * allocator branch run and return a real object in x0. */
            typedef void* (*PrefsCtorFn)(void* self);
            auto ctor = reinterpret_cast<PrefsCtorFn>(g_base + kPrefsCtorOff);
            p = ctor(nullptr);
            if (p == nullptr || !Plausible(p) ||
                !RangeReadable(p, kPrefsSize)) {
                return false;
            }
        }
        unsigned char* b = reinterpret_cast<unsigned char*>(p);
        /* 0x10..0x37: seven bools, timesPlayed, windowMode, displayIndex,
         * gamepadMode, playerLimit, fullscreenResolutionX/Y.  Stops before
         * +0x38 because that is a managed string pointer. */
        if (s_snapOk != 0) {
            const unsigned char* sn = s_prefsSnap;
            for (unsigned i = 0x10u; i < 0x38u; ++i) {
                b[i] = sn[i];
            }
            /* +0x40 safeRegionSize is a float; copy it as a word. */
            *reinterpret_cast<unsigned*>(b + 0x40) =
                *reinterpret_cast<const unsigned*>(sn + 0x40);
        }
        *reinterpret_cast<void**>(b + kPrefsOptsOff) = g_options;
        b[0x58] = 1;                                   /* isLoaded */
        b[0x59] = 0;                                   /* _isBusy */
        b[0x5a] = 0;                                   /* pending language */
        *reinterpret_cast<void**>(b + 0x60) = nullptr; /* _task */
        *reinterpret_cast<void**>(b + 0x68) = nullptr; /* reset viewport */
        g_ourPrefs = p;
        return true;
    }

    /* The save itself, lifted out of the draw hook on purpose.
     *
     * v191 and v193 both called this from HookDrawHud and both froze the
     * console the instant the box was ticked.  The work is known-good -- the
     * game runs the identical code on the main thread at boot -- so what
     * kills it is the context: drawHUD is inside the render submission and
     * an XmlSerializer-sized allocation there cannot reach a GC safe point.
     *
     * The request is still raised in the draw hook (that is where the flag
     * change is noticed), but it is consumed in HookCheckSingleAnimation,
     * which runs in the normal Update pass where the game allocates freely
     * every frame.  The call is asynchronous as well, so even the queueing
     * cost stays out of the render thread. */
    void TryPerformSave() {
        if (!g_saveWanted) {
            return;
        }
        unsigned dbg = 0x01u;                      /* no usable instance */

        /* v204: build our own instance.
         *
         * The captured pointer is dead by the time the player clicks (v202's
         * autopsy: check 6, displayIndex out of range), and v203's attempt to
         * borrow the game's own instance from Game1+0x150 failed at check 2 --
         * Game1's field table ends at +0xeb, so the method at 0x185F870 that
         * stores a StartupPreferences at this+0x150 is not a Game1 method at
         * all.  There is no reachable long-lived holder, so construct one.
         *
         * Two traps, both already measured:
         *
         *   1. loadPreferences takes a skip-wait flag (w21: `tbnz w21,#0`
         *      jumps over Task.Wait) and the boot path passes 0, i.e. it waits.
         *      Calling that here is the v196 freeze, so the instance is NOT
         *      seeded through loadPreferences.
         *   2. the constructor only sets displayIndex = -1, playerLimit = -1,
         *      lastEnteredIP, safeRegionSize and clientOptions.  windowMode,
         *      timesPlayed, gamepadMode and fullscreenResolution stay 0, so
         *      saving a bare instance would reset the player's display
         *      settings.  Instead the value-type scalars are copied from
         *      s_prefsSnap, which the update hook keeps while the captured
         *      wrapper is still readable.
         *
         * Pointer fields are deliberately NOT copied: +0x38 lastEnteredIP and
         * +0x48 languageCode point at managed strings that die with the
         * captured wrapper.  They keep the constructor's values, and w2 = true
         * makes savePreferences refresh +0x48 from the live game anyway. */
        void* use = nullptr;
        if (g_startupPrefs != nullptr && LooksLikeStartupPrefs(g_startupPrefs)) {
            use = g_startupPrefs;              /* should not happen any more */
            s_prefsSrc = 2u;
        }
        else if (BuildOwnPrefs()) {
            use = g_ourPrefs;
            s_prefsSrc = 1u;
        }
        else {
            s_prefsSrc = 3u;
            s_saveDbg = dbg;
            return;
        }
        g_savePrefs = use;
        dbg |= 0x02u;                              /* usable instance */
        unsigned char* sp = reinterpret_cast<unsigned char*>(use);
        {
        /* bit 1 = "the captured pointer is still a live StartupPreferences"
         * (LooksLikeStartupPrefs).  It used to mean Plausible(), which only
         * says the page is mapped -- a recycled block passes that.  Nothing is
         * written and no save is issued unless this bit is set: a bad instance
         * would truncate startup_preferences via _savePreferences' File.Create
         * and then throw, i.e. destroy the very file we are trying to update.
         * Reading 01 in row 2 group 1 means exactly that -- the pointer is dead
         * and the next build has to allocate its own instance instead. */
            /* The instance the serializer reads is THIS instance's own
             * clientOptions member (+0x50).  v197 read it back as NULL (bit 7
             * = 0, bit 3 = 0) and its save task ended at status 7 = Faulted,
             * which is what serialising a null clientOptions does.
             *
             * Exactly three sites write +0x50, and only one can produce null:
             *
             *   0x1ADE938  ctor          -> clientOptions = new Options()  (never null)
             *   0x181A1A8  boot save     -> clientOptions = Game1.get_options()
             *   0x1ADFD4C  readSettings  -> clientOptions = source.clientOptions
             *
             * so the null would have come from readSettings copying it out of
             * the deserialised object -- i.e. the file had no usable
             * <clientOptions> body.  But a DEAD instance reads as null too
             * (freed bytes), which is why the fingerprint above decides first
             * whether this pointer is worth reading at all.
             *
             * The repair is the game's OWN boot assignment, not an invention:
             *
             *   0x181A1A8  bl #0x1443520   ; Game1.get_options
             *   0x181A1AC  str x0,[x19,#0x50]
             *
             * "the serialised copy IS the live Options".  With it restored the
             * file gets a complete <clientOptions> block again and the write
             * finally has something to persist.
             *
             * bit7 reports the state BEFORE the repair on purpose: 0 + bit2 = 1
             * means "it was null and we fixed it", 1 + bit2 = 1 means "it was
             * already the live object", 1 + bit2 = 0 means "it is some other
             * Options". */
            void* co = *reinterpret_cast<void**>(sp + kPrefsOptsOff);
            g_serializedOptions = co;
            /* v386: capture at the earliest point the hook has an object, so
             * "hook never runs" and "guard rejected the object" are told apart.
             * v385 put this inside the LooksLikeOptions guard, which is why the
             * readout said 0000 -- it could not distinguish the two. */
            if (s_dbgHookRuns < 0xFu) {
                ++s_dbgHookRuns;
            }
            s_dbgCoIsLive = (co != nullptr && co == g_options) ? 1u : 0u;
            s_dbgCoVal = 0;
            if (co != nullptr && RangeReadable(
                    reinterpret_cast<unsigned char*>(co) + kMaskOff, 4u)) {
                const unsigned char* cb = reinterpret_cast<const unsigned char*>(co);
                unsigned got = 0;
                for (int b = 0; b < 4; b++) {
                    got |= static_cast<unsigned>(cb[kMaskOff + b]) << (8 * b);
                }
                s_dbgCoVal = got & 0xFFFu;
            }
            if (co != nullptr) {
                dbg |= 0x80u;                  /* clientOptions was not null */
            }
            if (!LooksLikeOptions(co) && g_options != nullptr &&
                RangeReadable(sp, kPrefsSize)) {
                co = g_options;
                *reinterpret_cast<void**>(sp + kPrefsOptsOff) = co;
                g_serializedOptions = co;
            }
            if (co != nullptr && co == g_options) {
                dbg |= 0x04u;                  /* serialised copy == live Options */
            }
            if (co != nullptr && LooksLikeOptions(co)) {
                unsigned char* cs = reinterpret_cast<unsigned char*>(co);
                cs[kOptFlagOff] = g_savedFlag;
                /* v314: and the switch bitmap, for exactly the same reason the
                 * flag above is written here and not left to the live object:
                 * this IS the copy the serializer reads. */
                for (int b = 0; b < 4; b++) {
                    cs[kMaskOff + b] =
                        static_cast<unsigned char>((g_savedMask >> (8 * b)) & 0xFFu);
                }
                cs[kOptDirtyOff] = 1;
                s_serBit = cs[kOptFlagOff];
                dbg |= 0x08u;                  /* wrote the serialised copy */
                /* v385: prove what the serializer will actually read back out of
                 * the copy we just wrote, and whether that copy IS the live
                 * Options.  Read-only -- nothing here changes behaviour. */
                s_dbgWrote = 1;
                {
                    unsigned live = 0;
                    if (g_options != nullptr) {
                        const unsigned char* lo =
                            reinterpret_cast<const unsigned char*>(g_options);
                        for (int b = 0; b < 4; b++) {
                            live |= static_cast<unsigned>(lo[kMaskOff + b]) << (8 * b);
                        }
                    }
                    s_dbgLiveVal = live & 0xFFFu;
                }
            }
            s_busyVal = *(sp + 0x59);              /* informational now */
            if (s_busyVal != 0) {
                dbg |= 0x10u;
                /* Somebody else is already saving -- the game itself does this
                 * at boot and when a farm is saved.  savePreferences would take
                 * the "already busy" branch and return without writing, so
                 * leave the request pending and try again on a later frame. */
                s_saveDbg = dbg;
                return;
            }
            dbg |= 0x20u;                          /* about to call */
            g_saveWanted = 0;
            s_saveDbg = dbg;   /* survives even if the call never returns */
            /* Arm the v199 write-path probes for THIS attempt.  The cumulative
             * counts are deliberately left alone -- they are how we tell
             * "hook never installed" from "hook installed but not reached". */
            s_wrEntered0 = s_wrEntered;
            s_wrWrite0 = s_wrWrite;
            s_wrBack = 0;
            s_wrWriteBack = 0;
            s_wrArmed = 1;   /* from here on the "this attempt" bits are real */
            /* w1 = TRUE: queue the write on the thread pool and do NOT wait.
             *
             * The attempts so far, and what each one taught us:
             *
             *   v191  savePreferences(..., false, false)  froze -- render hook
             *   v193  _savePreferences(...) directly      froze -- render hook
             *   v195  savePreferences(..., true,  false)  OK   -- update hook
             *   v196  savePreferences(..., false, false)  froze -- update hook
             *
             * v196 is the decisive one: the SAME synchronous form that the
             * game itself uses (0x18600F4 in the gameplay flow, 0x181A1C4 at
             * boot) froze the console when we called it from the update hook --
             * the main thread sat in Task.Wait() and never came back, music
             * and all.  So the wait is not safe outside the boot path, and
             * "the game does it too" is not evidence that it is safe here.
             *
             * The asynchronous form is the only one that has actually run to
             * completion on hardware.  Its one flaw is that savePreferences
             * clears _isBusy (+0x59) ONLY in the waiting path, so the flag
             * would stay 1 and block every later save -- the game's own
             * included.  WatchSaveTask() below clears it as soon as the task
             * reports that it is done, which is exactly what the waiting path
             * would have done. */
            auto savePrefs =
                reinterpret_cast<void (*)(void*, bool, bool)>(
                    g_base + kSavePrefsOff);
            /* w2 = TRUE as well now (v198).  savePreferences' second flag is
             * "update the display settings first": with it set the wrapper
             * refreshes +0x48 languageCode from the live game before writing
             * (0x1ADEF40..0x1ADEF88).  The only runtime save the game itself
             * performs -- the language-change path at 0x1ADEE38 -- passes
             * (w1 = 0, w2 = 1), so this is the shape that is known to run
             * outside the boot path.
             *
             * It matters for fidelity, not for the checkbox: with w2 = false
             * the file is written with whatever languageCode this instance
             * happens to hold, and this instance is the one readSettings
             * filled from the file.  Refreshing it from the live game keeps
             * every other element of <StartupPreferences> as the player has
             * it, which is the whole point of writing the file at all. */
            /* v346: the trace goes in before the write, on the very object the
             * serialiser reads.  timesPlayed is only a play counter, so this
             * costs nothing the player can see. */
            if (RangeReadable(sp + 0x18, 4u)) {
                *reinterpret_cast<unsigned*>(sp + 0x18) = s_trace;
            }
            savePrefs(use, true, true);
            /* The wrapper stores the Task it started at +0x60. */
            g_saveTask = *reinterpret_cast<void**>(sp + 0x60);
            s_taskState = 0;
            s_busySeen = 1u;                       /* armed: start watching */
            s_saveCalls = (s_saveCalls < 15u) ? s_saveCalls + 1u : 15u;
            dbg |= 0x40u;                          /* returned */
        }
        s_saveDbg = dbg;
    }

    /* Watch the queued save and release the busy flag once it is done.
     *
     * Task's status lives at +0x48 and Task.get_IsCompleted is exactly
     * "status > 4" (0x3CA090).  Status 5 is RanToCompletion -- the file was
     * written -- 6 is Canceled and 7 is Faulted (get_IsCanceled 0x3CA150 tests
     * == 6, get_IsFaulted 0x3CA160 tests == 7; v197's readout said 7, so its
     * write threw, it was not cancelled).
     *
     * Clearing _isBusy is not a hack: it is what the waiting path of
     * savePreferences does after Task.Wait() returns, and we are simply doing
     * it one step later because we refused to wait.  Without it the flag would
     * stay 1 for the rest of the session and every later save -- the game's
     * own included -- would take the "already busy" branch and refuse to
     * write. */
    void WatchSaveTask() {
        if (g_saveTask == nullptr) {
            return;
        }
        if (!Plausible(g_saveTask) ||
            !RangeReadable(g_saveTask, 0x4Cu)) {
            g_saveTask = nullptr;
            s_taskState = -1;
            return;
        }
        const int st = *reinterpret_cast<int*>(
            reinterpret_cast<unsigned char*>(g_saveTask) + 0x48);
        s_taskState = st;
        if (st < 5) {
            return;                    /* still running */
        }
        if (g_savePrefs != nullptr && LooksLikeStartupPrefs(g_savePrefs)) {
            *reinterpret_cast<unsigned char*>(
                reinterpret_cast<unsigned char*>(g_savePrefs) + 0x59) = 0;
        }
        s_busySeen |= 4u;              /* the write is over */
        s_busyVal = 0;
        g_saveTask = nullptr;
    }

    void SyncFastFromOptions(void* self) {
        /* v204: snapshot the captured instance while it is still readable.
         *
         * It dies shortly after boot (v202), so this has to run every frame
         * from the start and simply stop updating once the fingerprint fails.
         * Row `E` byte 1 reports whether a snapshot was ever taken -- if that
         * bit is 0, the instance we build in BuildOwnPrefs falls back to
         * constructor defaults for windowMode and the rest. */
        const bool capOk = LooksLikeStartupPrefs(g_startupPrefs);
        s_fpFailCap = s_fpFail;
        if (capOk) {
            const unsigned char* src =
                reinterpret_cast<const unsigned char*>(g_startupPrefs);
            for (unsigned i = 0; i < kPrefsSize; ++i) {
                s_prefsSnap[i] = src[i];
            }
            s_snapOk = 1;
            if (s_snapFrames < 7u) {
                s_snapFrames++;
            }
        }

        void* cand = OptionsFromGame1(self);
        s_optFlags &= ~0x30u;
        if (cand != nullptr) {
            s_optFlags |= 0x10u;              /* resolved via drawHUD's `this` */
        }
        else {
            /* drawHUD is reached through a vtable slot, so its x0 is only
             * presumably Game1.  Fall back to the static instance slot, which
             * is what Game1.get_options (0x1443520) itself dereferences:
             *     ldr x8,[0xC95C228] ; ldr x0,[x8,#0x90]
             * One dereference, identical expression. */
            void* g1 = *reinterpret_cast<void**>(g_base + kGame1SlotOff);
            cand = OptionsFromGame1(g1);
            if (cand != nullptr) {
                s_optFlags |= 0x20u;          /* resolved via static slot */
            }
        }

        unsigned char* flag = nullptr;
        if (cand != nullptr) {
            flag = reinterpret_cast<unsigned char*>(cand) + kOptFlagOff;
            /* A bool only ever holds 0 or 1.  Anything else means this is not
             * the object we think it is, whatever the checks said. */
            if (*flag > 1u) {
                flag = nullptr;
            }
        }
        g_options = (flag != nullptr) ? cand : nullptr;
        s_optFlags = (s_optFlags & ~3u) | ((flag != nullptr) ? 3u : 0u);

        s_optBit = (flag != nullptr) ? static_cast<unsigned>(*flag) : 0u;
        if (flag != nullptr) {
            unsigned char* opt = reinterpret_cast<unsigned char*>(g_options);
            /* Sticky, not live.  optionsDirty goes back to 0 when the game
             * saves, so reporting it live makes the readout flicker for no
             * reason; "was it ever set" is the question that matters. */
            if (*(opt + kOptDirtyOff) != 0) {
                s_optDirty = 1;
            }
            if (!g_bootLatched) {
                /* Latch before the player can click anything: after a restart
                 * this is what came out of startup_preferences, not what we
                 * just put in.
                 *
                 * The value is read from the copy that was DESERIALISED from
                 * the file, not from the live Options.  The two are separate
                 * objects at runtime (see TryPerformSave), and the live one
                 * only gets the file's value when the boot path takes branch A
                 * of Game1.set_options.  Reading the loaded copy answers the
                 * question we actually care about -- "what was in the file" --
                 * and pushing it into the live copy makes the box tick even if
                 * the boot path did not.
                 *
                 * The UI scale is latched on the SAME frame on purpose.  It is
                 * the one option value we know survives a restart (155% here),
                 * so if it does not read back at the latch frame then the latch
                 * happened before the file was applied and s_optBoot is
                 * meaningless -- that is what row 1 is for. */
                unsigned char* src = opt;
                if (LooksLikeStartupPrefs(g_startupPrefs)) {
                    void* co = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(g_startupPrefs) +
                        kPrefsOptsOff);
                    if (LooksLikeOptions(co)) {
                        src = reinterpret_cast<unsigned char*>(co);
                    }
                }
                /* v197 read s_serBit = 8, and 8 is not a bool: it is byte 0 of
                 * an Options instance, i.e. the low byte of the MethodTable
                 * pointer.  `*src` is the FIRST BYTE of the object, not +0x24
                 * -- src is already a byte pointer, so the offset has to be
                 * applied here.  Two consequences, both silent:
                 *
                 *   - s_optBoot was 8, and `8 & 1` is 0, so row 0's boot bit
                 *     could never be 1 and "the file said no" was never
                 *     actually observed;
                 *   - the mirror write put a MethodTable byte into a bool,
                 *     which SyncFastFromOptions then rejected as "> 1" and
                 *     discarded -- so the push never happened either.
                 *
                 * Reading through kOptFlagOff fixes both. */
                s_optBoot = src[kOptFlagOff];
                s_serBit = src[kOptFlagOff];
                if (src != opt) {
                    opt[kOptFlagOff] = src[kOptFlagOff];
                }
                s_bootUiBits = *reinterpret_cast<unsigned*>(opt + 0x64);
                g_bootLatched = 1;
            }
            else if (s_optBit != s_optBoot) {
                g_wroteFlag = 1;   /* something moved it, so the click landed */
            }

            /* No mirror any more.  v187 copied the value into +0x2c and +0x2e,
             * and that was a false positive: setToDefaults writes
             *
             *     stur w8,[x19,#0x2a]   with w8 = 0x01010000  -> +0x2c = 1
             *     strb w8,[x19,#0x2e]   with w8 = 1           -> +0x2e = 1
             *
             * so BOTH mirror slots are 1 by default.  The OR was therefore
             * always 1, the box came up ticked every boot, and it could never
             * be turned off again.  "It was already checked" was the default
             * leaking through, not persistence.
             *
             * This also kills the "let the game write an invisible field"
             * design outright: every offset reachable from the 47-index click
             * table is a real user-facing option.  Invisible storage can only
             * be written by us, by hand. */
            if (!g_desiredInit) {
                g_desired = (*flag != 0) ? 1 : 0;
                g_desiredInit = 1;
            }
            else if (*flag != g_desired) {
                /* The player clicked.  The game's own handler already wrote
                 * +0x24, so that byte IS the new intent. */
                g_desired = *flag;
                *(opt + kOptDirtyOff) = 1;
                g_wroteFlag = 1;
                /* The value that has to reach the file.  The live Options is
                 * not the one the serializer reads, so the bit is remembered
                 * here and copied into the serialised copy when the save is
                 * issued. */
                g_savedFlag = *flag;
                /* v209: the player's new choice must also win the next
                 * set_options, otherwise a later install would revert it. */
                s_applyTarget = *flag;
                g_saveWanted = 1;   /* ask for a real save on this frame */
            }
            s_optBit = *flag;
        }
        s_desired = g_desired;
        s_optFlags = (s_optFlags & ~8u) | (g_wroteFlag ? 8u : 0u);

        /* The request was raised above (g_saveWanted = 1); the call itself
         * happens in TryPerformSave, which is driven from the Update hook.
         * Doing it here froze the console twice -- see that function. */
        TryPerformSave();

        /* Watch the queued save finish.
         *
         *   bit0  armed -- we issued a save and are watching
         *   bit1  _isBusy read back non-zero at least once (task is running)
         *   bit2  the task finished and we released the busy flag
         *
         * The task's own status is the real evidence and is reported in row 2
         * byte 1: 3 = running, 5 = RanToCompletion (the file was written),
         * 6 = canceled, 7 = faulted (the write threw). */
        WatchSaveTask();
        if ((s_busySeen & 1u) != 0 && LooksLikeStartupPrefs(g_startupPrefs)) {
            const unsigned char busy = *reinterpret_cast<unsigned char*>(
                reinterpret_cast<unsigned char*>(g_startupPrefs) + 0x59);
            s_busyVal = busy;
            if (busy != 0) {
                s_busySeen |= 2u;
            }
            else if ((s_busySeen & 2u) != 0) {
                s_busySeen |= 4u;
            }
        }

        /* The two floats are no longer reported (row 1 carries +0x64 from the
         * latch frame instead), and LooksLikeOptions already validates them. */

        /* Nothing here touches g_ourElem any more.  It points into the options
         * page's own list, and the page is destroyed when the menu closes.  A
         * freed block usually keeps its MethodTable, so Plausible() still says
         * yes and +0x28 / +0x51 then read whatever object reused the memory --
         * which is exactly why row 3 once reported 3FC60000: that is not a
         * whichOption at all, it is the UI-scale float sitting at that offset in
         * whatever took the block.  The element is read in
         * HookOptionsPageDraw instead, where the page is provably alive. */

        /* The multiplier follows the stored byte.  Before the page has ever been
         * opened there is no element, so +0x24 is the only source -- and it is
         * the same field the box will be initialised from. */
        g_pendingFast = (s_optBit != 0) ? 2 : 1;

        /* Only drive g_animFast directly while the process is still on its very
         * first HUD frames.  Changing it underneath a running animation is
         * exactly what froze the game in v156..v158; after that, leave the
         * latch in the animation hook to apply it when the current animation
         * ends. */
        if (g_hudFrames < 3) {
            g_animFast = g_pendingFast;
        }
        g_hudFrames++;
    }

    /* ---- v398: HUD info overlay (UI Info Suite style) --------------------
     *
     * First use of dynamically built text in this mod.  Everything below is
     * guarded: a null batch, a null font or a null string simply skips the
     * draw, because this runs every frame of normal play.
     */
    void* ManagedStr(const char16_t* text) {
        if (text == nullptr) {
            return nullptr;
        }
        auto ctor = reinterpret_cast<void* (*)(void*, const char16_t*)>(
            g_base + kStringCtorCharsOff);
        return ctor(nullptr, text);
    }

    /* A managed string we built is NOT rooted.
     *
     * The GC only scans roots it knows about, and this module's data segment
     * is not one of them -- a pointer sitting in one of our globals is
     * invisible to it.  v412 showed exactly what that costs: after a trip to
     * the blacksmith both HUD lines came back as " value", i.e. our blocks had
     * been freed and handed to the game for its own strings, and the text only
     * returned after a day change rebuilt it.
     *
     * So each cached string remembers the length and first character it was
     * built with, and is rebuilt the moment the object no longer looks like
     * ours.  Validating costs two compares a frame; rebuilding unconditionally
     * would allocate three strings every frame. */
    struct CachedStr {
        void* obj;
        int len;
        char16_t first;
    };

    bool CachedStrOk(const CachedStr& c) {
        if (c.obj == nullptr || c.len <= 0 || !Plausible(c.obj)) {
            return false;
        }
        if (ManagedStrLen(c.obj) != c.len) {
            return false;
        }
        const char16_t* s = reinterpret_cast<const char16_t*>(
            reinterpret_cast<const unsigned char*>(c.obj) + 0x14);
        return *s == c.first;
    }

    void CachedStrBuild(CachedStr* c, const char16_t* buf, int len) {
        c->obj = ManagedStr(buf);
        c->len = len;
        c->first = len > 0 ? buf[0] : 0;
    }


    int AppendUnsigned(char16_t* buf, int at, int value) {
        char16_t tmp[12];
        int n = 0;
        if (value == 0) {
            tmp[n++] = u'0';
        }
        while (value > 0 && n < 12) {
            tmp[n++] = static_cast<char16_t>(u'0' + (value % 10));
            value /= 10;
        }
        for (int i = n - 1; i >= 0; i--) {
            buf[at++] = tmp[i];
        }
        return at;
    }

    /* Draws an already-built managed string.  Keeping the two steps apart means
     * the tool-upgrade line can be cached instead of rebuilt every frame. */
    void DrawHudString(void* msg, float x, float y, unsigned colour) {
        if (msg == nullptr || !Plausible(msg)) {
            return;
        }
        void* batch = *reinterpret_cast<void**>(g_base + kGame1SpriteBatchOff);
        void* font = *reinterpret_cast<void**>(g_base + kGame1SmallFontOff);
        if (!Plausible(batch) || !Plausible(font)) {
            return;
        }
        auto drawString = reinterpret_cast<DrawStringFn>(g_base + kDrawStringOff);
        drawString(batch, font, msg, colour, x, y);
    }

    int  s_lastUpgradeDays = -1;   /* >0: draw the tool row */
    int  s_upgradeShown = -1;      /* value s_upgradeLine was built from */
    CachedStr s_upgradeLine{nullptr, 0, 0};
    CachedStr s_weatherLine{nullptr, 0, 0};
    CachedStr s_luckLine{nullptr, 0, 0};
    int   s_weatherKey = -1;

    /* Reads a managed String.  Layout from the metadata: length at +0x10, the
     * first UTF-16 character at +0x14. */
    const char16_t* StrChars(void* str, int* len) {
        if (str == nullptr || !Plausible(str) || !RangeReadable(str, 0x18u)) {
            return nullptr;
        }
        auto* b = reinterpret_cast<unsigned char*>(str);
        const int n = *reinterpret_cast<int*>(b + 0x10);
        if (n < 0 || n > 64) {
            return nullptr;
        }
        if (n > 0 &&
            !RangeReadable(b + 0x14, static_cast<size_t>(n) * 2u)) {
            return nullptr;
        }
        *len = n;
        return reinterpret_cast<const char16_t*>(b + 0x14);
    }

    /* Case-insensitive "does this managed string start with that ASCII
     * literal".  Prefix, not equality: the game's ids are not spelled the way
     * the PC mods spell them -- a clear day comes back as "Sun", not "Sunny",
     * and an exact compare silently showed the raw English word. */
    bool StrStartsWithAscii(void* str, const char* ascii) {
        int n = 0;
        const char16_t* c = StrChars(str, &n);
        if (c == nullptr) {
            return false;
        }
        int m = 0;
        while (ascii[m] != 0) {
            m++;
        }
        if (n < m) {
            return false;
        }
        for (int i = 0; i < m; i++) {
            char16_t a = c[i];
            char b = ascii[i];
            if (a >= u'A' && a <= u'Z') {
                a = static_cast<char16_t>(a - u'A' + u'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = static_cast<char>(b - 'A' + 'a');
            }
            if (a != static_cast<char16_t>(b)) {
                return false;
            }
        }
        return true;
    }

    /* Game1.weatherForTomorrow holds an internal id, not the text the player
     * sees.  The spellings are the ones this build actually produces -- a
     * clear day is "Sun" -- with the PC-mod spellings kept as aliases so a
     * game update that renames them does not blank the HUD again.
     *
     * Prefix match, first hit wins, so put the shortest alias first when two
     * would both match.  An id we fail to match shows the raw English word
     * and no icon; that is the bug this table exists to prevent. */
    struct WeatherId {
        const char* id;
        int slot;
    };
    constexpr WeatherId kWeatherIds[] = {
        {"sun", 0},
        {"sunny", 0},
        {"rain", 1},
        {"storm", 2},
        {"lightning", 2},
        {"snow", 3},
        {"wind", 4},
        {"windy", 4},
        {"debris", 4},
        {"greenrain", 5},
        {"festival", 6},
        {"wedding", 7},
    };

    /* Index = slot.  Must stay in step with kWeatherIcons. */
    constexpr const char16_t* kWeatherNames[] = {
        u"晴", u"雨", u"雷雨", u"雪", u"大风", u"绿雨", u"节日", u"婚礼",
    };
    constexpr int kWeatherSlotCount =
        static_cast<int>(sizeof(kWeatherNames) / sizeof(kWeatherNames[0]));
    static_assert(kWeatherSlotCount == kWeatherIconCount,
                  "weather name table and icon table must line up");

    /* Weather id -> slot, or -1. */
    int WeatherSlot(void* str) {
        for (const WeatherId& w : kWeatherIds) {
            if (StrStartsWithAscii(str, w.id)) {
                return w.slot;
            }
        }
        return -1;
    }

    const char16_t* WeatherLabel(void* str) {
        const int slot = WeatherSlot(str);
        return slot >= 0 ? kWeatherNames[slot] : nullptr;
    }

    /* The calendar is fixed: eight festivals a year, on the same dates every
     * year, so this needs no content load and no managed call.  Seasons are
     * 0..3 and every month is 28 days.
     *
     * Names are kept short -- they go on one HUD line next to the weather. */
    /* Dates are the eight single-day festivals, checked against the wiki:
     * spring 13/24, summer 11/28, fall 16/27, winter 8/25.  The Flower Dance
     * is the 24th, not the 25th -- v410 had it wrong, and because every other
     * date was right the only day that showed the bug was spring 23.
     *
     * The four multi-day "passive" festivals (Desert Festival, Trout Derby,
     * SquidFest, Night Market) are deliberately absent: they live in
     * Data/PassiveFestivals, the town stays open, and the game never sets
     * weatherForTomorrow to "Festival" for them. */
    struct FestivalDate {
        int season;
        int day;
        const char16_t* name;
    };
    constexpr FestivalDate kFestivals[] = {
        {0, 13, u"蛋蛋节"},
        {0, 24, u"花舞节"},
        {1, 11, u"夏威夷宴"},
        {1, 28, u"月光水母"},
        {2, 16, u"星露谷展览"},
        {2, 27, u"万圣节"},
        {3, 8, u"冰雪节"},
        {3, 25, u"冬星节"},
    };

    /* Name of the festival on a given date, or nullptr. */
    const char16_t* FestivalName(int season, int day) {
        for (const FestivalDate& f : kFestivals) {
            if (f.season == season && f.day == day) {
                return f.name;
            }
        }
        return nullptr;
    }

    /* Tomorrow's date.  Months are 28 days; the last day of winter rolls into
     * spring of the next year, and only the season index matters here. */
    void NextDate(int season, int day, int* outSeason, int* outDay) {
        if (day >= 1 && day < 28) {
            *outSeason = season;
            *outDay = day + 1;
            return;
        }
        *outSeason = (season + 1) & 3;
        *outDay = 1;
    }

    /* Blits one 32x32 icon from its run list.  `white` is Game1.staminaRect, a
     * 1x1 white texture the game already loads, so a run is just a tinted
     * rectangle -- no texture of our own, no filtering, pixel-exact. */
    void DrawIconRuns(void* white, const IconRun* runs, int count,
                      int ox, int oy, int scale) {
        void* batch = *reinterpret_cast<void**>(g_base + kGame1SpriteBatchOff);
        if (!Plausible(batch) || !Plausible(white) || runs == nullptr ||
            count <= 0 || count > 4096) {
            return;
        }
        auto draw = reinterpret_cast<DrawRectFn>(g_base + kDrawRectOff);
        for (int i = 0; i < count; i++) {
            const IconRun& r = runs[i];
            const u64 xy =
                (static_cast<u64>(static_cast<u32>(oy + r.y * scale)) << 32) |
                static_cast<u32>(ox + r.x * scale);
            const u64 wh =
                (static_cast<u64>(static_cast<u32>(r.h * scale)) << 32) |
                static_cast<u32>(r.w * scale);
            draw(batch, white, xy, wh, r.argb);
        }
    }

    /* Weather id -> icon slot.  Order matches ICONS in tools/png_to_icons.py. */
    int WeatherIconIndex(void* str) {
        return WeatherSlot(str);
    }

    /* Builds the three HUD strings.  Runs in the update hook, never while
     * drawing.
     *
     * ManagedStr calls String::.ctor, which allocates a managed object.  Doing
     * that from the draw path can trigger a collection while the game holds
     * the sprite batch, and the console locks up -- hard reset only.  It is
     * intermittent by nature: a string is rebuilt only when the value behind
     * it changes, so it looked like a random freeze rather than a repeatable
     * one.  Same rule as v392: managed calls live in update, draw only reads.
     */
    void BuildHudStrings() {
        if (!kHudInfo) {
            return;
        }

        /* Tomorrow's weather.  Rebuilt only when the calendar day changes. */
        void* weather = *reinterpret_cast<void**>(g_base + kWeatherTomorrowOff);
        const int season = *reinterpret_cast<const int*>(g_base + kSeasonOff);
        const int day = *reinterpret_cast<const int*>(g_base + kDayOfMonthOff);
        const int key = season * 100 + day;
        if (key != s_weatherKey || !CachedStrOk(s_weatherLine)) {
            s_weatherKey = key;
            const char16_t* label = WeatherLabel(weather);
            /* A festival day arrives as the weather id "Festival", which is
             * useless on its own -- the calendar knows which one it is, so
             * say so: "明日 花舞节" instead of "明日 节日". */
            if (WeatherSlot(weather) == 6) {
                int ts = 0;
                int td = 0;
                NextDate(season, day, &ts, &td);
                const char16_t* name = FestivalName(ts, td);
                if (name != nullptr) {
                    label = name;
                }
            }
            char16_t buf[32];
            int n = 0;
            const char16_t pre[] = u"明日 ";
            for (int i = 0; pre[i] != 0 && n < 24; i++) {
                buf[n++] = pre[i];
            }
            if (label != nullptr) {
                for (int i = 0; label[i] != 0 && n < 30; i++) {
                    buf[n++] = label[i];
                }
            }
            else {
                /* Unknown id: show it raw rather than showing nothing.  This
                 * is how v409 found out a clear day is called "Sun" -- the
                 * fallback is the only reason that was visible at all. */
                int rawLen = 0;
                const char16_t* raw = StrChars(weather, &rawLen);
                for (int i = 0; raw != nullptr && i < rawLen && n < 30; i++) {
                    buf[n++] = raw[i];
                }
            }
            buf[n] = 0;
            CachedStrBuild(&s_weatherLine, buf, n);
        }

        /* Daily luck.  s_luckHundredths is sampled once a second in update. */
        if (s_luckHundredths != s_luckShown || !CachedStrOk(s_luckLine)) {
            s_luckShown = s_luckHundredths;
            char16_t buf[32];
            int n = 0;
            const char16_t pre[] = u"今日运气 ";
            for (int i = 0; pre[i] != 0 && n < 20; i++) {
                buf[n++] = pre[i];
            }
            const int hundredths = s_luckHundredths;
            const int mag = hundredths < 0 ? -hundredths : hundredths;
            buf[n++] = hundredths < 0 ? u'-' : u'+';
            n = AppendUnsigned(buf, n, mag / 100);
            buf[n++] = u'.';
            const int frac = mag % 100;
            buf[n++] = static_cast<char16_t>(u'0' + frac / 10);
            buf[n++] = static_cast<char16_t>(u'0' + frac % 10);
            buf[n] = 0;
            CachedStrBuild(&s_luckLine, buf, n);
        }

        /* Tool upgrade countdown.  s_lastUpgradeDays is only the "does this row
         * exist" flag the draw path reads; s_upgradeShown is what the string
         * was built from, so clearing the flag does not force a rebuild. */
        s_lastUpgradeDays = -1;
        void* pl = CarryPlayer();
        if (pl == nullptr) {
            return;
        }
        void* slot = NetValueSlot(pl, kFarmerToolUpgradeDaysOff, 4u);
        if (slot == nullptr) {
            return;
        }
        const int days = *reinterpret_cast<int*>(slot);
        if (days <= 0 || days > 99) {
            return;
        }
        s_lastUpgradeDays = days;
        if (days != s_upgradeShown || !CachedStrOk(s_upgradeLine)) {
            char16_t buf[40];
            int n = 0;
            const char16_t prefix[] = u"工具升级 剩余 ";
            for (int i = 0; prefix[i] != 0 && n < 30; i++) {
                buf[n++] = prefix[i];
            }
            n = AppendUnsigned(buf, n, days);
            const char16_t tail[] = u" 天";
            for (int i = 0; tail[i] != 0 && n < 36; i++) {
                buf[n++] = tail[i];
            }
            buf[n] = 0;
            CachedStrBuild(&s_upgradeLine, buf, n);
            s_upgradeShown = days;
        }
    }

    void DrawHudInfo() {
        if (!kHudInfo) {
            return;
        }
        /* Nothing here may allocate: every string below was built in the
         * update hook.  DrawHudString ignores a null or implausible object. */
        DrawHudString(s_weatherLine.obj, kHudTextX, kHudTextY, 0xFFFFFFFFu);
        if (kIconProbe) {
            /* Our own art, encoded as filled rectangles: no texture to load.
             * The slot is null until that icon has been drawn, so a missing
             * icon simply shows the text alone instead of drawing garbage. */
            void* weather =
                *reinterpret_cast<void**>(g_base + kWeatherTomorrowOff);
            const int idx = WeatherIconIndex(weather);
            if (idx >= 0 && idx < kWeatherIconCount &&
                kWeatherIcons[idx].count > 0) {
                void* white =
                    *reinterpret_cast<void**>(g_base + kStaminaRectOff);
                if (Plausible(white)) {
                    DrawIconRuns(white, kWeatherIcons[idx].runs,
                                 kWeatherIcons[idx].count,
                                 kHudIconX, kHudIconY, kHudIconScale);
                }
            }
        }

        /* Daily luck.  Icon comes from the game's own cursor sheet -- the same
         * 10x14 region UI Info Suite 2 uses -- tinted by tier so the colour
         * answers the question before you read the number. */
        DrawHudString(s_luckLine.obj, kHudTextX, kHudLuckTextY, 0xFFFFFFFFu);
        {
            void* batch =
                *reinterpret_cast<void**>(g_base + kGame1SpriteBatchOff);
            void* cursors =
                *reinterpret_cast<void**>(g_base + kMouseCursorsOff);
            if (Plausible(batch) && Plausible(cursors)) {
                NullableRect src;
                src.x = 50;
                src.y = 428;
                src.w = 10;
                src.h = 14;
                src.present = 1;
                for (int q = 0; q < 7; q++) {
                    src.pad[q] = 0;
                }
                const int iw = 10 * kHudLuckScale;
                const int ih = 14 * kHudLuckScale;
                const u64 dxy =
                    (static_cast<u64>(static_cast<u32>(kHudLuckY)) << 32) |
                    static_cast<u32>(kHudLuckX);
                const u64 dwh =
                    (static_cast<u64>(static_cast<u32>(ih)) << 32) |
                    static_cast<u32>(iw);
                auto drawSprite =
                    reinterpret_cast<DrawSpriteFn>(g_base + kDrawSpriteOff);
                drawSprite(batch, cursors, dxy, dwh, &src,
                           LuckColour(s_luckHundredths), 0.0f, 0.0f, 0.0f,
                           1.0f, 0);
            }
        }

        /* Tool upgrade countdown: only when something is actually upgrading. */
        if (s_lastUpgradeDays <= 0) {
            return;
        }
        void* batch = *reinterpret_cast<void**>(g_base + kGame1SpriteBatchOff);
        void* font = *reinterpret_cast<void**>(g_base + kGame1SmallFontOff);
        if (!Plausible(batch) || !Plausible(font)) {
            return;
        }
        /* The tool's own sprite when we have it, our hammer otherwise.  Both
         * are drawn 64x64 so the row keeps its size either way. */
        int drew = 0;
        if (s_toolRectValid != 0 && Plausible(s_toolSheet)) {
            NullableRect src;
            src.x = s_toolRectX;
            src.y = s_toolRectY;
            src.w = s_toolRectW;
            src.h = s_toolRectH;
            src.present = 1;
            for (int q = 0; q < 7; q++) {
                src.pad[q] = 0;
            }
            const u64 dxy =
                (static_cast<u64>(static_cast<u32>(kHudToolY)) << 32) |
                static_cast<u32>(kHudToolX);
            const u64 dwh =
                (static_cast<u64>(static_cast<u32>(64)) << 32) |
                static_cast<u32>(64);
            auto drawSprite =
                reinterpret_cast<DrawSpriteFn>(g_base + kDrawSpriteOff);
            drawSprite(batch, s_toolSheet, dxy, dwh, &src, 0xFFFFFFFFu,
                       0.0f, 0.0f, 0.0f, 1.0f, 0);
            drew = 1;
        }
        if (drew == 0 && kHud_tool.count > 0) {
            void* white = *reinterpret_cast<void**>(g_base + kStaminaRectOff);
            if (Plausible(white)) {
                DrawIconRuns(white, kHud_tool.runs, kHud_tool.count,
                             kHudToolX, kHudToolY, kHudToolScale);
            }
        }
        DrawHudString(s_upgradeLine.obj, kHudTextX, kHudToolTextY, 0xFFFFFFFFu);
    }

    void HookDrawHud(void* self) {
        SyncFastFromOptions(self);
        MaybeGrowBackpack();

        /* The checkbox needs no syncing from us any more: with whichOption =
         * kOptIndex the ctor reads Options+0x24 into the box and
         * receiveLeftClick writes it back.  SyncFastFromOptions() above only
         * reads the stored byte to decide the multiplier. */

        /* Stage 166: append our checkbox to the live options list, once per
         * page instance.  Everything is guarded: the list shape is checked, and
         * the write only happens when the backing array has spare capacity, so
         * no reallocation is ever attempted from native code. */
        /* v212: once our own tab has its OWN page, stop putting the checkbox
         * into the vanilla options page -- otherwise it appears twice.
         * v216: the gate is kModPage, not kModTab.  During the tab-only bisect
         * stage the tab opens the vanilla options page, so the checkbox has to
         * stay there or the player would have no way to reach it.
         * v229: and never while the page is shrunk.  The append writes at
         * `_size`, which is 1 on the Mod tab -- it would land on top of a
         * vanilla element and lose it.  Skipping costs nothing: leaving the tab
         * restores the count, and the next frame appends as usual. */
        if (!kAppendInTab && !(kModPage && s_tabState == 1u) &&
            s_savedOptCount < 0 &&
            g_optionsPage != nullptr && Plausible(g_optionsPage) &&
            RangeReadable(g_optionsPage, 0xB0u) &&
            g_optionsPage != g_appendedTo) {
            auto getPlayer2 = reinterpret_cast<void* (*)(void*)>(
                g_base + kGetPlayerOff);
            void* pl = getPlayer2(nullptr);
            /* Deliberately NOT marking this page handled here.  The page can be
             * open while Game1.player is still null; giving up at that point
             * would mean the box silently never appears for that page, with
             * appendState left at 0 and no way to tell why.  Nothing is
             * allocated until the attempt gets past the label, so retrying on
             * the next frame costs nothing. */
            void* nm = nullptr;
            if (pl != nullptr && Plausible(pl) && RangeReadable(pl, 0xB0u)) {
                void* nn = *reinterpret_cast<void**>(
                    reinterpret_cast<unsigned char*>(pl) + 0xA8);
                if (nn != nullptr && Plausible(nn) &&
                    RangeReadable(nn, 0x70u)) {
                    nm = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(nn) + 0x68);
                }
            }
            void* label = nullptr;
            if (nm != nullptr && Plausible(nm)) {
                /* PadLeft to 6 then Substring(0,6) yields a FRESH 6-char string
                 * whatever the name length is -- padding alone would hand back
                 * `this` for names of 6+ chars and we would then overwrite the
                 * player's own name. */
                auto padLeft = reinterpret_cast<void* (*)(void*, int, char16_t)>(
                    g_base + 0x39AF40);
                auto substr = reinterpret_cast<void* (*)(void*, int, int)>(
                    g_base + 0x395F40);
                void* wide = padLeft(nm, 6, u'\u3000');
                if (wide != nullptr && Plausible(wide)) {
                    label = substr(wide, 0, 6);
                }
            }
            if (label != nullptr && Plausible(label) &&
                ManagedStrLen(label) == 6 &&
                RangeReadable(reinterpret_cast<unsigned char*>(label) + 0x14,
                              12u)) {
                char16_t* lp = reinterpret_cast<char16_t*>(
                    reinterpret_cast<unsigned char*>(label) + 0x14);
                lp[0] = u'\u52A8';   /* 动 */
                lp[1] = u'\u753B';   /* 画 */
                lp[2] = u'\u52A0';   /* 加 */
                lp[3] = u'\u901F';   /* 速 */
                lp[4] = u'2';
                lp[5] = u'\u500D';   /* 倍 */
                auto makeElem2 = reinterpret_cast<void* (*)(void*, void*, int,
                                                            int, int)>(
                    g_base + 0x17F6780);
                void* elem = makeElem2(nullptr, label, kOptIndex, -1, -1);
                if (elem != nullptr && Plausible(elem) &&
                    RangeReadable(elem, 0x58u)) {
                    void* list = *reinterpret_cast<void**>(
                        reinterpret_cast<unsigned char*>(g_optionsPage) + 0xA0);
                    if (list != nullptr && Plausible(list) &&
                        RangeReadable(list, 0x1Cu)) {
                        const int cnt = *reinterpret_cast<int*>(
                            reinterpret_cast<unsigned char*>(list) + 0x18);
                        void* wrapper = *reinterpret_cast<void**>(
                            reinterpret_cast<unsigned char*>(list) + 0x10);
                        if (cnt >= 0 && cnt < 256 && wrapper != nullptr &&
                            Plausible(wrapper) &&
                            RangeReadable(wrapper, 0x28u)) {
                            void* capPtr = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(wrapper) +
                                0x20);
                            void* buf = *reinterpret_cast<void**>(
                                reinterpret_cast<unsigned char*>(wrapper) +
                                0x10);
                            if (capPtr != nullptr &&
                                RangeReadable(capPtr, 4u) && buf != nullptr &&
                                Plausible(buf)) {
                                const int cap = *reinterpret_cast<int*>(capPtr);
                                if (cap > cnt) {
                                    reinterpret_cast<void**>(buf)[cnt] = elem;
                                    *reinterpret_cast<int*>(
                                        reinterpret_cast<unsigned char*>(list) +
                                        0x18) = cnt + 1;
                                    g_appendState = 1;
                                    g_appendCount = static_cast<unsigned>(cnt + 1);
                                    g_ourElem = elem;
                                    g_appendedTo = g_optionsPage;
                                    /* No seeding here.  The ctor's last act is
                                     * getCheckBoxOption(options, elem), which
                                     * has already loaded +0x51 from
                                     * Options+0x24 -- overwriting it is exactly
                                     * what stopped v185 from ever reporting the
                                     * stored value. */
                                }
                                else {
                                    g_appendState = 2;   /* no spare capacity */
                                    g_appendCount = static_cast<unsigned>(cnt);
                                    g_appendedTo = g_optionsPage;
                                }
                            }
                        }
                        else {
                            g_appendState = 3;
                            g_appendedTo = g_optionsPage;
                        }
                    }
                }
                else if (elem != nullptr) {
                    /* Allocation handed back something we cannot trust.  Retrying
                     * would leak one element per frame, so give up on this page
                     * and report it instead of spinning. */
                    g_appendState = 4;
                    g_appendedTo = g_optionsPage;
                }
            }
        }

        const int s = *reinterpret_cast<const int*>(g_base + kSeasonOff);
        const int d = *reinterpret_cast<const int*>(g_base + kDayOfMonthOff);
        if (s >= 0 && s <= 3 && d >= 1 && d <= 28) {
            const int today = s * 100 + d;
            if (g_bdayNotified != today) {
                /* Marked before the attempt: a failure must not retry every
                 * frame, and this is the only place that pushes. */
                g_bdayNotified = today;
                PushBirthdayToast(FindBirthdayName());
            }
        }
        /* v398: drawn BEFORE the game's HUD, not after.  drawHUD is called
         * with a SpriteBatch already begun; whether that batch is still open
         * when it returns is not something this code can see, and drawing into
         * a closed batch throws a managed exception.  Before the call the
         * batch is definitely open. */
        DrawHudInfo();
        s_origDrawHud(self);
        if (kDiagNoDraw) {
            return;
        }
    }

namespace nn::ro::detail {
    /* Retail's sdk does not export this; rtld would leave the slot at 0 and
     * derail.  exlaunch only reads it behind a null check. */
    rtld::lookup_global_t g_LookupGlobalManualFunctionPointer = nullptr;


}

extern "C" void exl_main(void* x0, void* x1) {
    (void)x0;
    (void)x1;

#ifdef SDV_DIAG_HANG
    for (;;) {
        __asm__ volatile("yield");
    }
#endif

    /* File logging and the stored settings are NOT opened here.
     *
     * exl_main runs during rtld module initialisation, i.e. before the
     * process has finished coming up.  Doing it here is what made the whole
     * file channel a silent no-op on retail: `sdmc:/sdv_map.log` never
     * appeared, so ReadSetting() always returned -1 and WriteSetting() always
     * failed, with no way to tell why.  EnsureSettings() does it on the first
     * HUD draw instead -- see its comment.
     *
     * Consequence: the early LOGF lines below are dropped, because Line() is a
     * no-op until the store is open.  Nothing that matters is in them; the
     * addresses are all in docs/. */

    const uintptr_t base = exl::util::modules::GetTargetStart();
    g_base = base;
    LOGF("stage2: main base %p", reinterpret_cast<void*>(base));

    exl::hook::Initialize();

    ApplyBiggerBackpackPatches();
    ApplyBackpackShopPatches();
    ApplyNamingFixPatch();

    /* v210: BUILD FINGERPRINT -- do not install a single hook on a build we
     * were not written for.
     *
     * Every hook is an absolute offset into the game's text segment, taken from
     * update E7F84509 (build E7F845093E8CBC68 / game 1.6.15.13).  On any other
     * build those offsets land inside unrelated functions, so hooking them does
     * not "fail" -- it overwrites code that is still executing and the console
     * dies.  That is fine on this machine, where the build is known, and
     * completely unacceptable for a file other people are meant to copy.
     *
     * So: verify the first two instruction words at EVERY hook site against the
     * bytes in data/update-E7F84509/main.text.bin.  These are literally the
     * bytes the trampoline is about to relocate, so a match means the hook is
     * safe by construction, not merely "probably the right version".  Sixteen
     * sites, 128 bytes verified.
     *
     * On a mismatch the mod installs NOTHING and the game runs vanilla.  That is
     * the whole point: a wrong build must degrade to "no mod", never to a crash.
     * There is no on-screen message because drawing one would need the very
     * hooks we just refused to install -- if the mod appears to do nothing,
     * this check is why.
     *
     * Regenerating / verifying: `python tools/build_fingerprint.py --check`
     * simulates this exact loop on main.text.bin, so the table cannot drift
     * away from the addresses it protects.  Run it after touching any kXxxOff
     * constant or adding a hook -- a stale entry fails here instead of on
     * hardware, where the only symptom is "the mod does nothing". */
    {
        struct BuildProbe {
            uintptr_t off;
            u32 w0;
            u32 w1;
        };
        static const BuildProbe kProbes[] = {
        { 0x005D3C8Cu, 0xB005A8C8u, 0xB005A8D3u },
        { 0x0177BA70u, 0xD102C3FFu, 0xFD0023E8u },
        { 0x015DCBC0u, 0x6DBC23E9u, 0xA9017BFDu },
        { 0x015FD310u, 0xD10243FFu, 0xFD0023EAu },
        { 0x017981F0u, 0xA9BE7BFDu, 0xA9014FF4u },
        { 0x01797E00u, 0xA9BE7BFDu, 0xA9014FF4u },
        { 0x01797FB0u, 0xD10083FFu, 0xA9017BFDu },
        { 0x0147BDE0u, 0xD10643FFu, 0xFD007BEEu },
        { 0x01B3CF40u, 0xD10783FFu, 0xFD00BBECu },
        { 0x01712830u, 0xD10243FFu, 0xA9037BFDu },
        { 0x01A0E610u, 0xD10183FFu, 0x6D0223E9u },
        { 0x019DF2D0u, 0xD10303FFu, 0x6D0523E9u },
        { 0x0120AE00u, 0xD101C3FFu, 0x6D0223E9u },
        { 0x014500A0u, 0xD10543FFu, 0xA9107BFDu },
        { 0x003943F0u, 0x17FFFFA4u, 0x00000000u },
        { 0x0181D1F0u, 0xD10683FFu, 0x6D1133EDu },
        { 0x01ADF4A0u, 0xD10103FFu, 0xA9017BFDu },
        { 0x01ADF190u, 0xA9BE7BFDu, 0xA9014FF4u },
        { 0x01ADF2F0u, 0xA9BD7BFDu, 0xF9000BF5u },
        { 0x01ADFC30u, 0xA9BD7BFDu, 0xF9000BF5u },
        { 0x01443540u, 0xB005A8C8u, 0x9108A108u },
        { 0x017FBA90u, 0xD10683FFu, 0xA9147BFDu },
        { 0x01400090u, 0x39429008u, 0x34000088u },
        { 0x014888D0u, 0x6DB63BEFu, 0x6D0133EDu },
        { 0x01440570u, 0xF005A8C8u, 0x912D4108u },
        { 0x015182D0u, 0x6DB63BEFu, 0x6D0133EDu },
        { 0x017786A0u, 0xA9BB7BFDu, 0xF9000BF9u },
        { 0x01A80AB0u, 0xA9BD7BFDu, 0xF9000BF5u },
        { 0x01A0A3C0u, 0xA9BD7BFDu, 0xF9000BF5u },
        { 0x01A0B530u, 0xD2F7F008u, 0xF9015C1Fu },
        { 0x015B0E20u, 0xD10143FFu, 0xA9017BFDu },
        { 0x017E8ED0u, 0xD105C3FFu, 0xFD0073ECu },
        { 0x01BBB510u, 0xD101C3FFu, 0xFD001BEAu },
        { 0x017E91F0u, 0xD10243FFu, 0x6D012BEBu },
        };
        const unsigned probeCount =
            static_cast<unsigned>(sizeof(kProbes) / sizeof(kProbes[0]));
        unsigned bad = 0xFFFFFFFFu;
        for (unsigned i = 0; i < probeCount; ++i) {
            const u32* w = reinterpret_cast<const u32*>(base + kProbes[i].off);
            if (w[0] != kProbes[i].w0 || w[1] != kProbes[i].w1) {
                bad = i;
                break;
            }
        }
        if (bad != 0xFFFFFFFFu) {
            LOGF("build: fingerprint mismatch at probe %u (off %p), mod disabled",
                 bad, reinterpret_cast<void*>(kProbes[bad].off));
            return;
        }
        LOGF("build: fingerprint ok, %u probes", probeCount);
    }


    if (kMapStage >= 40 && kMapStage < 90) {
        s_origGetItem = HOOKF(base + kGetItemOff, &HookGetItem, true);
        LOGF("getitem: hook installed, trampoline %p",
             reinterpret_cast<void*>(s_origGetItem));
    }

    s_origDialogueUpdate = reinterpret_cast<void (*)(void*, void*)>(
        HOOKF(base + 0x177BA70, &HookDialogueUpdate, true));
    LOGF("dialogue: update hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origDialogueUpdate));

    s_origBusUpdate = reinterpret_cast<void (*)(void*, void*)>(
        HOOKF(base + 0x15DCBC0, &HookBusUpdate, true));
    LOGF("bus: update hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origBusUpdate));

    s_origDesertUpdate = reinterpret_cast<void (*)(void*, void*)>(
        HOOKF(base + 0x15FD310, &HookDesertUpdate, true));
    LOGF("desert: update hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origDesertUpdate));

    s_origGameMenuUpdate = reinterpret_cast<void (*)(void*, void*)>(
        HOOKF(base + kGameMenuUpdateOff, &HookGameMenuUpdate, true));
    LOGF("tab: GameMenu.update hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origGameMenuUpdate));

    s_origTabNumFromName = reinterpret_cast<int (*)(void*, void*)>(
        HOOKF(base + kTabNumFromNameOff, &HookTabNumFromName, true));
    LOGF("tab: getTabNumberFromName hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origTabNumFromName));

    s_origTabLabelFromIdx = reinterpret_cast<void* (*)(int)>(
        HOOKF(base + kTabLabelFromIdxOff, &HookTabLabelFromIdx, true));

    /* v248: the tractor trigger.  pressUseToolButton is `bool ()` with no
     * arguments, so the hook is a plain pass-through. */
    if (kTractor && kTractorActive) {
        s_origPressUseTool = reinterpret_cast<bool (*)()>(
            HOOKF(base + kPressUseToolOff, &HookPressUseTool, true));
    }
    /* stage 2: widen the tool's area of effect while riding the tractor. */
    if (kTractor && kTractorActive) {
        s_origTilesAffected = reinterpret_cast<void* (*)(void*, Vec2, int, void*)>(
            HOOKF(base + kTilesAffectedOff, &HookTilesAffected, true));
    }

    /* v287: the animals page icon (hooked on the list, not the constructor). */
    if (kTractor) {
        s_origFindAnimals = reinterpret_cast<void* (*)(void*)>(
            HOOKF(base + kFindAnimalsOff, &HookFindAnimals, true));
    }

    /* v337/v338: Automate Lite.  The chest hook is the whole scheduler --
     * GameLocation.updateWhenCurrentLocation already walks every object in the
     * current location, so this sees each chest once per frame with no scan. */
    if (kAutomate) {
        s_origChestUpdate = reinterpret_cast<void (*)(void*, void*)>(
            HOOKF(base + kChestUpdateOff, &HookChestUpdate, true));
        s_origObjectUpdate = reinterpret_cast<void (*)(void*, void*)>(
            HOOKF(base + kObjUpdateOff, &HookObjectUpdate, true));
        LOGF("automate: object update hook %p",
             reinterpret_cast<void*>(s_origObjectUpdate));
        LOGF("automate: chest update hook %p",
             reinterpret_cast<void*>(s_origChestUpdate));
        /* v371: fish ponds are buildings; FishPond.Update is the per-frame hook
         * that hands us each one (see GameLocation.updateWhenCurrentLocation). */
        s_origPondUpdate = reinterpret_cast<void (*)(void*, void*)>(
            HOOKF(base + kFishPondUpdateOff, &HookFishPondUpdate, true));
        LOGF("automate: fish pond hook %p",
             reinterpret_cast<void*>(s_origPondUpdate));
    }

    /* stage 2c: per-frame auto tool use while driving.
     * v337: also required by Automate Lite, so install it whenever either
     * feature is on rather than only when the tractor auto-use is. */
    if ((kTractor && kTractorActive && kTractorAutoUse) || kAutomate) {
        s_origGameUpdate = reinterpret_cast<void (*)(void*, void*)>(
            HOOKF(base + kGameUpdateOff, &HookGameUpdate, true));
        LOGF("update: per-frame hook %p",
             reinterpret_cast<void*>(s_origGameUpdate));
    }
    LOGF("tab: getLabelOfTabFromIndex hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origTabLabelFromIdx));

    s_origStrEquals = reinterpret_cast<int (*)(void*, void*)>(
        HOOKF(base + kStrEqualsOff, &HookStringEquals, true));
    LOGF("tab: String.Equals hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origStrEquals));

    s_origShippingUpdate = reinterpret_cast<void (*)(void*, void*)>(
        HOOKF(base + kShippingUpdateOff, &HookShippingUpdate, true));
    LOGF("shipping: update hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origShippingUpdate));

    s_origLoadPrefs = reinterpret_cast<void (*)(void*, int, int, int)>(
        HOOKF(base + kLoadPrefsOff, &HookLoadPreferences, true));
    LOGF("prefs: loadPreferences hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origLoadPrefs));

    s_origSavePrefsImpl = reinterpret_cast<void (*)(void*)>(
        HOOKF(base + kSavePrefsImplOff, &HookSavePreferencesImpl, true));
    LOGF("prefs: _savePreferences probe installed, trampoline %p",
         reinterpret_cast<void*>(s_origSavePrefsImpl));

    s_origWriteSettings = reinterpret_cast<void (*)(void*, void*)>(
        HOOKF(base + kWriteSettingsOff, &HookWriteSettings, true));
    LOGF("prefs: writeSettings probe installed, trampoline %p",
         reinterpret_cast<void*>(s_origWriteSettings));

    s_origReadSettings = reinterpret_cast<void (*)(void*, void*)>(
        HOOKF(base + kReadSettingsOff, &HookReadSettings, true));
    LOGF("prefs: readSettings probe installed, trampoline %p",
         reinterpret_cast<void*>(s_origReadSettings));

    s_origSetOptions = reinterpret_cast<void (*)(void*)>(
        HOOKF(base + kSetOptionsOff, &HookSetOptions, true));
    LOGF("prefs: set_options probe installed, trampoline %p",
         reinterpret_cast<void*>(s_origSetOptions));

    s_origOptionsDraw = reinterpret_cast<void (*)(void*, void*)>(
        HOOKF(base + 0x17FBA90, &HookOptionsPageDraw, true));
    LOGF("options: draw hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origOptionsDraw));

    s_origCheckSingle = reinterpret_cast<bool (*)(void*, void*)>(
        HOOKF(base + 0x1400090, &HookCheckSingleAnimation, true));
    LOGF("anim: checkForSingleAnimation hooked, trampoline %p",
         reinterpret_cast<void*>(s_origCheckSingle));

    s_origDrawHud = HOOKF(base + 0x14888D0, &HookDrawHud, true);
    LOGF("hud: hook installed, trampoline %p",
         reinterpret_cast<void*>(s_origDrawHud));

    s_origGetPlayer = HOOKF(base + kGetPlayerOff, &HookGetPlayer, true);
    LOGF("stage2: hook installed, trampoline %p, slot %p",
         reinterpret_cast<void*>(s_origGetPlayer),
         reinterpret_cast<void*>(base + kPlayerSlotOff));

    /* v328: the 48-slot tier gets its own price and its own wording.  Three
     * hooks, each installs on its own so one missing trampoline only disables
     * that half instead of taking the shop down with it. */
    if (kBiggerBackpack) {
        if (kBagTier3Price) {
            s_origAnswerDialogueAction = reinterpret_cast<AnswerDialogueActionFn>(
                HOOKF(base + kAnswerDialogueActionOff, &HookAnswerDialogueAction, true));
            LOGF("bag: answerDialogueAction hook %p",
                 reinterpret_cast<void*>(s_origAnswerDialogueAction));
        }
        if (kBagTier3Text) {
            s_origDialogueBoxCtor = reinterpret_cast<DialogueBoxCtorFn>(
                HOOKF(base + kDialogueBoxCtorOff, &HookDialogueBoxCtor, true));
            s_origResponseCtor = reinterpret_cast<ResponseCtorFn>(
                HOOKF(base + kResponseCtorOff, &HookResponseCtor, true));
            LOGF("bag: text hooks %p %p",
                 reinterpret_cast<void*>(s_origDialogueBoxCtor),
                 reinterpret_cast<void*>(s_origResponseCtor));
        }
    }

    /* v334: carry chests with their contents.  Three independent hooks, each
     * installed on its own so one missing trampoline cannot take the others
     * down with it. */
    if (kCarryChest) {
        s_origChestHit = reinterpret_cast<ChestHitFn>(
            HOOKF(base + kChestHitOff, &HookChestHit, true));
        s_origChestPlace = reinterpret_cast<ChestPlaceFn>(
            HOOKF(base + kChestPlaceOff, &HookChestPlace, true));
        s_origCanStack = reinterpret_cast<CanStackFn>(
            HOOKF(base + kCanStackOff, &HookCanStack, true));
        LOGF("carrychest: hooks %p %p %p",
             reinterpret_cast<void*>(s_origChestHit),
             reinterpret_cast<void*>(s_origChestPlace),
             reinterpret_cast<void*>(s_origCanStack));
    }

    if (kMapStage >= 0) {
        s_origDrawMini = HOOKF(base + kDrawMiniPortraitsOff,
                                         &HookDrawMiniPortraits, true);
    }
    LOGF("map: hook installed, trampoline %p",
                reinterpret_cast<void*>(s_origDrawMini));

    if (kMapStage == 0) {
        s_origPixel = HOOKF(base + kPixelMathOff, &HookPixel, true);
        LOGF("pixel: hook installed, trampoline %p",
                    reinterpret_cast<void*>(s_origPixel));

        s_origDrawPortrait = HOOKF(base + kDrawPortraitOff,
                                             &HookDrawPortrait, true);
        LOGF("portrait: hook installed, trampoline %p",
                    reinterpret_cast<void*>(s_origDrawPortrait));
    }

    /* One-hit kill used to be written here at boot, unconditionally: every
     * launch rewrote 0x14EA504 / 0x14EA508 to `mov w22,#9999` / `mov w21,#9999`.
     *
     * That is why a game process always came up with the damage site already
     * patched, in every launch and after every reboot, before anything else had
     * a chance to attach -- and why a menu switch reading "off" could not be
     * trusted: there was no way to tell our write from this one.
     *
     * The overlay plugin owns this site now.  It pokes the same two words on
     * demand and writes the originals back when the switch is off, so the state
     * follows the menu instead of being decided once at launch, and the damage
     * site reads as `2A0403F6 / 2A0303F5` in a freshly started process.
     *
     * Note the cost: the tool swing below (kDamageMonsterOff) relied on
     * min = max = 9999 being forced in the body to guarantee a lethal hit.  With
     * this off, a swing does the damage the weapon actually computes.
     *
     * Flip to true to bring the old behaviour back. */
    constexpr bool kBootOneHit = false;
    if (kBootOneHit) {
        exl::patch::RandomAccessPatcher patcher;

        const u32 curMax = patcher.Read<u32>(kDmgMaxOff);
        const u32 curMin = patcher.Read<u32>(kDmgMinOff);
        LOGF("stage2: damage words %08x %08x", curMax, curMin);

        if (curMax == kDmgMaxOrig && curMin == kDmgMinOrig) {
            patcher.Write<u32>(kDmgMaxOff, kDmgMaxHit);
            patcher.Write<u32>(kDmgMinOff, kDmgMinHit);
            LOGF("stage2: one-hit kill applied");
        }
        else {
            LOGF("stage2: one-hit skipped, words do not match");
        }
    }
}

extern "C" NORETURN void exl_exception_entry() {
    EXL_ABORT("unexpected exception in sdv_native");
}
