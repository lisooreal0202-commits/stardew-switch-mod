/* Generated from Data/WorldMap.xnb + Maps tbin files (PC 1.6.15).
 * rect is in world-map texture pixels; the game draws it at 4x.
 * Do not edit by hand -- see tools/gen_worldmap_table.py. */
#pragma once

struct WorldMapEntry {
    const char* name;
    short rectX, rectY, rectW, rectH;
    short mapW, mapH;
};

static const WorldMapEntry kWorldMap[] = {
    { "AbandonedJojaMart", 215, 79, 7, 5, 18, 17 },
    { "AdventureGuild", 219, 28, 3, 2, 14, 20 },
    { "AnimalShop", 102, 102, 2, 2, 35, 20 },
    { "ArchaeologyHouse", 220, 110, 5, 3, 50, 20 },
    { "Backwoods", 82, 38, 29, 24, 50, 40 },
    { "BathHouse_Entry", 146, 20, 3, 3, 10, 10 },
    { "BathHouse_MensLocker", 146, 17, 3, 3, 18, 28 },
    { "BathHouse_Pool", 149, 17, 6, 6, 28, 34 },
    { "BathHouse_WomensLocker", 146, 17, 3, 3, 18, 28 },
    { "Beach", 186, 137, 57, 31, 104, 50 },
    { "Blacksmith", 214, 104, 3, 3, 15, 20 },
    { "BugLand", 112, 151, 2, 2, 60, 60 },
    { "BusStop", 115, 50, 30, 29, 65, 30 },
    { "Club", 36, 20, 1, 1, 50, 60 },
    { "BeachNightMarket", 186, 137, 57, 31, 104, 50 },
    { "CommunityCenter", 184, 57, 5, 4, 25, 25 },
    { "Desert", 36, 20, 1, 1, 50, 60 },
    { "DesertFestival", 36, 20, 1, 1, 50, 60 },
    { "ElliottHouse", 211, 140, 2, 3, 16, 10 },
    { "Farm", 68, 62, 50, 34, 80, 65 },
    { "FarmCave", 98, 63, 1, 2, 80, 65 },
    { "FishShop", 202, 155, 5, 3, 12, 10 },
    { "Forest", 47, 96, 76, 65, 120, 120 },
    { "HaleyHouse", 162, 108, 5, 3, 25, 30 },
    { "HarveyRoom", 171, 84, 3, 3, 24, 13 },
    { "Hospital", 171, 84, 3, 3, 24, 20 },
    { "JojaMart", 215, 79, 7, 5, 30, 30 },
    { "JoshHouse", 188, 89, 4, 4, 25, 25 },
    { "LeahHouse", 111, 113, 3, 2, 16, 16 },
    { "ManorHouse", 189, 104, 6, 6, 25, 12 },
    { "MermaidHouse", 213, 155, 5, 3, 9, 12 },
    { "Mines", 204, 26, 1, 1, 135, 41 },
    { "Mountain", 165, 26, 98, 18, 135, 41 },
    { "MovieTheater", 180, 54, 10, 6, 23, 17 },
    { "MovieTheaterScreen", 180, 54, 10, 6, 14, 17 },
    { "Railroad", 140, 10, 46, 15, 70, 62 },
    { "Saloon", 177, 95, 4, 3, 46, 25 },
    { "SamHouse", 153, 107, 3, 3, 25, 25 },
    { "SandyHouse", 36, 20, 1, 1, 50, 60 },
    { "SandyShop", 36, 20, 1, 1, 50, 60 },
    { "ScienceHouse", 172, 36, 6, 4, 32, 25 },
    { "SebastianRoom", 172, 36, 6, 4, 13, 13 },
    { "SeedShop", 175, 84, 6, 3, 48, 32 },
    { "Sewer", 175, 120, 2, 2, 40, 50 },
    { "SkullCave", 36, 20, 1, 1, 50, 60 },
    { "Submarine", 181, 158, 9, 8, 25, 18 },
    { "Summit", 161, 0, 1, 1, 70, 62 },
    { "Sunroom", 176, 83, 6, 4, 12, 15 },
    { "Tent", 189, 28, 1, 2, 135, 41 },
    { "Town", 147, 46, 97, 80, 130, 110 },
    { "Trailer", 198, 94, 5, 3, 20, 12 },
    { "Trailer_Big", 198, 94, 4, 3, 30, 26 },
    { "Tunnel", 109, 57, 1, 1, 50, 40 },
    { "WitchHut", 174, 7, 1, 1, 70, 62 },
    { "WitchSwamp", 174, 7, 1, 1, 70, 62 },
    { "WitchWarpCave", 174, 7, 1, 1, 70, 62 },
    { "WizardHouse", 50, 107, 2, 3, 14, 25 },
    { "WizardHouseBasement", 50, 107, 2, 3, 20, 8 },
};

static const int kWorldMapCount = sizeof(kWorldMap) / sizeof(kWorldMap[0]);
