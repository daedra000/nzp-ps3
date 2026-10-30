/*
Copyright (C) 2025-2026 NZ:P Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
#include "../nzportable_def.h"
#include "menu_defs.h"

//=============================================================================
/* MAIN MENU */
qboolean in_submenu;

void Menu_Solo(void) { menu_is_solo = true; Cvar_SetValue("cl_splitscreen", 0); Menu_StockMaps_Set(); }
// split screen: two pads, one map; player 2 joins once the map is in (cl_seats.c)
void Menu_SplitScreen(void) { menu_is_solo = true; Cvar_SetValue("cl_splitscreen", 1); Menu_StockMaps_Set(); }
void Menu_Quit(void) { key_dest = key_console; Host_Quit_f(); }
void Menu_EnterSubMenu(void) { Menu_ResetMenuButtons(); in_submenu = true; }
void Menu_ExitSubMenu(void) { in_submenu = false; Menu_ResetMenuButtons(); }

/*
===============
Menu_Main_Set
===============
*/
void Menu_Main_Set (void)
{
	loadingScreen = 0;
	loadscreeninit = false;

	Menu_LoadPics();
	Menu_ResetMenuButtons();
	Menu_SetSound(MENU_SND_ENTER);
	if (key_dest != key_menu) {
		Cbuf_AddText("cd playstring tensioned_by_the_damned 1\n");
	}

	key_dest = key_menu;
	m_previous_state = 0;
	m_state = m_main;
	loadingScreen = 0;
}

/*
===============
Menu_Main_Draw
===============
*/
void Menu_Main_Draw (void)
{
	// Background
	Menu_DrawCustomBackground (true);

	// Header
	Menu_DrawTitle ("MAIN MENU", MENU_COLOR_WHITE);

	// Version String
	Menu_DrawBuildDate();

	if (!in_submenu) {
		Menu_DrawButton(1, 0, "SOLO", "Play Solo.", Menu_Solo);
#ifdef __PS3__
		// no online co-op on PS3: split screen, and custom maps from the internet
		Menu_DrawButton(2, 1, "SPLIT SCREEN", "Two Players, Two Controllers.", Menu_SplitScreen);
		Menu_DrawButton(3, 2, "DOWNLOAD MAPS", "Download Custom Maps", Menu_MapDownload_Set);
#define MAIN_IDX(n) ((n) + 2)
#define MAIN_ROW(n) ((n) + 1)
#else
		Menu_DrawGreyButton(2, "COOPERATIVE");
#define MAIN_IDX(n) (n)
#define MAIN_ROW(n) (n)
#endif

		Menu_DrawDivider(MAIN_ROW(3));

		Menu_DrawButton(MAIN_ROW(3), MAIN_IDX(1), "CONFIGURATION", "Tweak Game Related Options", Menu_Configuration_Set);
		Menu_DrawButton(MAIN_ROW(4), MAIN_IDX(2), "CHARACTER BIOS", "View Character Bios", Menu_Bios_Set);

		Menu_DrawDivider(MAIN_ROW(5));

		Menu_DrawButton(MAIN_ROW(5), MAIN_IDX(3), "CREDITS", "NZ:P Team + Special Thanks", Menu_Credits_Set);

		Menu_DrawDivider(MAIN_ROW(6));

		Menu_DrawButton(MAIN_ROW(6), MAIN_IDX(4), "QUIT GAME", "Return to Home Screen", Menu_EnterSubMenu);

		Menu_DrawSocialBadge (1, MENU_SOC_YOUTUBE);
		Menu_DrawSocialBadge (2, MENU_SOC_BLUESKY);
		Menu_DrawSocialBadge (3, MENU_SOC_PATREON);
		Menu_DrawSocialBadge (4, MENU_SOC_DOCS);
	} else {
		Menu_DrawGreyButton(1, "SOLO");
#ifdef __PS3__
		Menu_DrawGreyButton(2, "SPLIT SCREEN");
		Menu_DrawGreyButton(3, "DOWNLOAD MAPS");
#else
		Menu_DrawGreyButton(2, "COOPERATIVE");
#endif
		Menu_DrawDivider(MAIN_ROW(3));
		Menu_DrawGreyButton(MAIN_ROW(3), "CONFIGURATION");
		Menu_DrawGreyButton(MAIN_ROW(4), "CHARACTER BIOS");
		Menu_DrawDivider(MAIN_ROW(5));
		Menu_DrawGreyButton(MAIN_ROW(5), "CREDITS");
		Menu_DrawDivider(MAIN_ROW(6));
		Menu_DrawGreyButton(MAIN_ROW(6), "QUIT GAME");

		Menu_DrawSocialBadge (1, MENU_SOC_YOUTUBE);
		Menu_DrawSocialBadge (2, MENU_SOC_BLUESKY);
		Menu_DrawSocialBadge (3, MENU_SOC_PATREON);
		Menu_DrawSocialBadge (4, MENU_SOC_DOCS);

		// Draw Sub Menu
    	Menu_DrawSubMenu("Are you sure you want to quit?", "You will lose any progress that you have made.");

		Menu_DrawButton (MAIN_ROW(7), 0, "GET ME OUTTA HERE!", "", Menu_Quit);
		Menu_DrawButton (MAIN_ROW(8), 1, "I WILL PERSEVERE", "", Menu_ExitSubMenu);
	}
}
