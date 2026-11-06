#pragma once

/* 由 tools/offgen.py 生成，勿手改。
 *
 * main.cpp 里每个常量都写成：
 *     constexpr uintptr_t kDrawRectOff = SDV_OFF(kDrawRectOff, 0xC1A80);
 * 第二个参数是 1.6.15.13 的值。对 1.6.15.13 构建，SDV_OFF 直接展开成这个值，
 * 预处理的 token 与没有这层包裹时完全一致，二进制逐字节不变。
 *
 * 换版本构建：make CXX_FLAGS=-DSDV_GAME_VERSION=<下面的数>
 *   1.6.15.13 -> 1061513（本文件默认）
 *   1-6-15-3 -> 1061503
 *
 * 没有强证据的常量**故意不定义**：换版本构建会在那一行直接报出常量名，
 * 而不是带着旧地址编译通过然后在机器上写错地方。候选值与原因写在对应
 * 版本的注释块里，人工确认后补一行 #define 即可。
 */

#ifndef SDV_GAME_VERSION
#define SDV_GAME_VERSION 1061513   /* 1.6.15.13 */
#endif

#if SDV_GAME_VERSION == 1061513
#  define SDV_OFF(name, value) value

#elif SDV_GAME_VERSION == 1061503   /* 1-6-15-3 */
#  define SDV_OFF(name, value) SDV_VAL_##name
/* 强证据 93 个 */
#  define SDV_VAL_kDrawRectOff                 0xC0240     /* SpriteBatch::Draw  <- 值变化 */
#  define SDV_VAL_kStaminaRectOff              0xE27F680   /* Game1::staminaRect  <- 值变化 */
#  define SDV_VAL_kMouseCursorsOff             0xE27F558   /* Game1::mouseCursors  <- 值变化 */
#  define SDV_VAL_kSeasonOff                   0xE27F720   /* Game1::season  <- 值变化 */
#  define SDV_VAL_kDayOfMonthOff               0xE27F7A8   /* Game1::dayOfMonth  <- 值变化 */
#  define SDV_VAL_kGame1ContentOff             0xE27F378   /* Game1::content  <- 值变化 */
#  define SDV_VAL_kObjectSheetOff              0xE27F4D8   /* Game1::objectSpriteSheet  <- 值变化 */
#  define SDV_VAL_kCharNameOff                 0xA8        /* Building::parentLocationName  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kCharDisplayNameOff          0xC0        /* Character::_displayName */
#  define SDV_VAL_kLoadSpriteTextureOff        0x10FFC60   /* AnimatedSprite::LoadTexture  <- 值变化 */
#  define SDV_VAL_kGetSpriteTexOff             0x10FF650   /* AnimatedSprite::get_Texture  <- 值变化 */
#  define SDV_VAL_kGetWorldPositionsOff        0x1B0A650   /* MapArea::GetWorldPositions  <- 值变化 */
#  define SDV_VAL_kGetPosNoFallbackOff         0x1B0EB20   /* WorldMapManager::GetPositionDataWithoutFallback  <- 值变化 */
#  define SDV_VAL_kGetPlayerOff                0x139C170   /* Game1::get_player  <- 值变化 */
#  define SDV_VAL_kPlayerSlotOff               0xE27F448   /* Game1::_player  <- 值变化 */
#  define SDV_VAL_kDrawMiniPortraitsOff        0x173C7F0   /* MapPage::drawMiniPortraits  <- 值变化 */
#  define SDV_VAL_kPixelMathOff                0x1B0C590   /* MapAreaPosition::GetMapPixelPosition  <- 值变化 */
#  define SDV_VAL_kMapPosOff                   0x68        /* MapPage::mapPosition */
#  define SDV_VAL_kMapBoundOff                 0xA4        /* MapPage::mapBounds */
#  define SDV_VAL_kMapHoverTextOff             0xC0        /* MapPage::hoverText */
#  define SDV_VAL_kMapAreasOff                 0x90        /* MapPage::mapAreas */
#  define SDV_VAL_kMapAreaTooltipsOff          0x18        /* CachedMultiplayerMap::appliedMapOverrides  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kMapAreaScrollTextOff        0x28        /* WorldMapAreaPositionScrollTextZoneData::ScrollText */
#  define SDV_VAL_kMapTooltipTextOff           0x38        /* SparklingText::text */
#  define SDV_VAL_kMapTooltipIdOff             0x40        /* TailorItemRecipe::CraftedItemIdFeminine  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kLocCharactersOff            0xB0        /* GameLocation::characters */
#  define SDV_VAL_kLocDisplayNameOff           0x198       /* GameLocation::_displayName  <- 值变化 */
#  define SDV_VAL_kLocNameOff                  0x1A0       /* GameLocation::name  <- 值变化 */
#  define SDV_VAL_kLocDisplayNameGetOff        0x1425BE0   /* GameLocation::get_DisplayName  <- 值变化 */
#  define SDV_VAL_kDrawStringOff               0xC0380     /* SpriteBatch::DrawString  <- 值变化 */
#  define SDV_VAL_kGame1SmallFontOff           0xE27F698   /* Game1::smallFont  <- 值变化 */
#  define SDV_VAL_kGame1DialogueFontOff        0xE27F690   /* Game1::dialogueFont  <- 值变化 */
#  define SDV_VAL_kGetTileXOff                 0x118C470   /* Character::getTileX  <- 值变化 */
#  define SDV_VAL_kGetTileYOff                 0x118C4A0   /* Character::getTileY  <- 值变化 */
#  define SDV_VAL_kGetTileBodyOff              0x1188420   /* Character::get_Tile  <- 值变化 */
#  define SDV_VAL_kGetPositionDataOff          0x1B0E7B0   /* WorldMapManager::GetPositionData  <- 值变化 */
#  define SDV_VAL_kGetCurrentLocationOff       0x1188700   /* Character::get_currentLocation  <- 值变化 */
#  define SDV_VAL_kGetTilePointOff             0x1188500   /* Character::get_TilePoint  <- 值变化 */
#  define SDV_VAL_kGetLocationsOff             0x139C3A0   /* Game1::get_locations  <- 值变化 */
#  define SDV_VAL_kOptFlagOff                  0x24        /* CommonAcl+<RemoveAceSpecific>c__AnonStorey1::propaga  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kOptDirtyOff                 0x150       /* Options::optionsDirty  <- 值变化 */
#  define SDV_VAL_kLoadPrefsOff                0x1A2F6F0   /* StartupPreferences::loadPreferences  <- 值变化 */
#  define SDV_VAL_kSavePrefsOff                0x1A2EF30   /* StartupPreferences::savePreferences  <- 值变化 */
#  define SDV_VAL_kWriteSettingsOff            0x1A2F4C0   /* StartupPreferences::writeSettings  <- 值变化 */
#  define SDV_VAL_kShippingUpdateOff           0x176FF20   /* ShippingMenu::update  <- 值变化 */
#  define SDV_VAL_kShippingTimerOff            0xE8        /* GeodeMenu::geodeAnimationTimer  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kShippingFinishedOff         0x118       /* ShippingMenu::_hasFinished */
#  define SDV_VAL_kShippingActivatedOff        0x119       /* ShippingMenu::_activated */
#  define SDV_VAL_kGameMenuUpdateOff           0x16EE010   /* GameMenu::update  <- 值变化 */
#  define SDV_VAL_kPageCtorOff                 0x174C330   /* OptionsPage::.ctor  <- 值变化 */
#  define SDV_VAL_kTabCtorOff                  0x16B1030   /* ClickableComponent::.ctor  <- 值变化 */
#  define SDV_VAL_kPadLeftOff                  0x38E4A0    /* String::PadLeft  <- 值变化 */
#  define SDV_VAL_kSubstrOff                   0x3896E0    /* String::Substring  <- 值变化 */
#  define SDV_VAL_kTabNumFromNameOff           0x16EDBA0   /* GameMenu::getTabNumberFromName  <- 值变化 */
#  define SDV_VAL_kTabLabelFromIdxOff          0x16EDDD0   /* GameMenu::getLabelOfTabFromIndex  <- 值变化 */
#  define SDV_VAL_kGrowInventoryOff            0x1323E60   /* Farmer::increaseBackpackSize  <- 值变化 */
#  define SDV_VAL_kMaxItemsOff                 0x570       /* Farmer::maxItems  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kStrEqualsOff                0x3878F0    /* String::op_Equality  <- 值变化 */
#  define SDV_VAL_kNumberOfTabsOff             0xE2820E0   /* GameMenu::numberOfTabs  <- 值变化 */
#  define SDV_VAL_kGmTabsOff                   0x80        /* CollectionsPage::sideTabs  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kGmPagesOff                  0x88        /* GameMenu::pages  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kGmCurrentTabOff             0x68        /* GameMenu::currentTab  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kLastSelectedIndexOff        0xE282404   /* OptionsPage::_lastSelectedIndex  <- 值变化 */
#  define SDV_VAL_kLastCurrentItemIndexOff     0xE282408   /* OptionsPage::_lastCurrentItemIndex  <- 值变化 */
#  define SDV_VAL_kPageCurrentItemIndexOff     0x78        /* AdvancedGameOptions::currentItemIndex  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kPageOptionsOff              0xA0        /* OptionsPage::options */
#  define SDV_VAL_kListSizeOff                 0x18        /* FontInfo::<Size>k__BackingField  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kReadSettingsOff             0x1A2FF50   /* StartupPreferences::readSettings  <- 值变化 */
#  define SDV_VAL_kSetOptionsOff               0x139F510   /* Game1::set_options  <- 值变化 */
#  define SDV_VAL_kPrefsLangOff                0x48        /* WorldChangeEvent::preEventLocation  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kPrefsCtorOff                0x1A2E890   /* StartupPreferences::.ctor  <- 值变化 */
#  define SDV_VAL_kSavePrefsImplOff            0x1A2F2F0   /* StartupPreferences::_savePreferences  <- 值变化 */
#  define SDV_VAL_kGame1SlotOff                0xE27FB10   /* Game1::game1  <- 值变化 */
#  define SDV_VAL_kEventUpOff                  0xE27F6BC   /* Game1::eventUp  <- 值变化 */
#  define SDV_VAL_kQuestLogOff                 0x1B0       /* Farmer::questLog  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kQuestAcceptedOff            0x30        /* Quest::accepted */
#  define SDV_VAL_kQuestCompletedOff           0x38        /* Quest::completed */
#  define SDV_VAL_kQuestDailyOff               0x40        /* Quest::dailyQuest */
#  define SDV_VAL_kQuestDaysLeftOff            0x78        /* Quest::daysLeft */
#  define SDV_VAL_kQuestTypeOff                0x70        /* Quest::questType */
#  define SDV_VAL_kHorseCtorOff                0x1199480   /* Horse::.ctor  <- 值变化 */
#  define SDV_VAL_kGetFarmOff                  0x13C31E0   /* Game1::getFarm  <- 值变化 */
#  define SDV_VAL_kGetLocNowOff                0x139C3C0   /* Game1::get_currentLocation  <- 值变化 */
#  define SDV_VAL_kNetRefSetOff                0x18D36C0   /* NetFarmerRef::set_Value  <- 值变化 */
#  define SDV_VAL_kPressUseToolOff             0x13DAC40   /* Game1::pressUseToolButton  <- 值变化 */
#  define SDV_VAL_kCurrentToolOff              0x1318EC0   /* Farmer::get_CurrentTool  <- 值变化 */
#  define SDV_VAL_kFarmerNetMountOff           0x740       /* Farmer::netMount */
#  define SDV_VAL_kHorseNetRiderOff            0x4A0       /* Horse::netRider */
#  define SDV_VAL_kHorseMountingOff            0x4B0       /* Horse::mounting */
#  define SDV_VAL_kTrkListSizeOff              0x18        /* FontInfo::<Size>k__BackingField  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kTrkListStoreOff             0x10        /* DesertFestival+<>c__DisplayClass96_0::store_data_she  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kTrkStoreCapOff              0x20        /* X509Certificate+CertificateContext::hCertStore  ?? 原符号未核实（值未变） */
#  define SDV_VAL_kTrkStoreBufOff              0x10        /* DesertFestival+<>c__DisplayClass96_0::store_data_she  ?? 原符号未核实（值未变） */

/* 未定义（22 个）——没有强证据，构建会在常量那一行报错。
 * 下面是候选值与原因，人工确认后把整行改成 #define。 */
/*   kDmgMinOff                 候选 0x144A78C  方法内偏移，只凭单条指令在方法体内唯一，上下文不足 */
/*   kDrawSpriteOff             候选 0xBF450    函数入口，被调集合只有 25%，与旧函数不像 */
/*   kLoadTextureOff            无候选                       指纹定位不确定：两个以上候选同分（0.94），无法区分 */
/*   kNpcSpriteVirtualSlot      无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kDmgMaxOff                 无候选                       方法内偏移，容器未佐证；交给 sigreloc，未定位则需人工 */
/*   kDmgMaxOrig                无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kDmgMinOrig                无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kDmgMaxHit                 无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kDmgMinHit                 无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kDrawPortraitOff           无候选                       指纹定位不确定：两个以上候选同分（0.77），无法区分 */
/*   kListGetItemOff            无候选                       指纹定位不确定：两个以上候选同分（0.71），无法区分 */
/*   kPixelPosOff               无候选                       方法内偏移，容器未佐证；交给 sigreloc，未定位则需人工 */
/*   kGetItemOff                无候选                       方法内偏移，容器未佐证；交给 sigreloc，未定位则需人工 */
/*   kOptSize                   无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kCreateStringOff           无候选                       方法在新版不存在 */
/*   kIconLookupLo              无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kIconLookupHi              无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kPrefsOptsOff              无候选                       实例字段偏移撞车，名字未佐证 */
/*   kPrefsSize                 无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kNetValueOff2              无候选                       指令字/结构大小，不是地址，不参与迁移 */
/*   kTrkCharsOff               无候选                       实例字段偏移撞车，名字未佐证 */
/*   kTrkCollListOff            无候选                       实例字段偏移撞车，名字未佐证 */

#else
#  error "SDV_GAME_VERSION 未知：先用 tools/migrate.py 迁移并重跑 tools/offgen.py"
#endif
