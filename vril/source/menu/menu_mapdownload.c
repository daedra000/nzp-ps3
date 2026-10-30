/*
menu_mapdownload.c: DOWNLOAD MAPS (PS3). The list and the downloads are
platform/ps3/ps3_mapdl.c; this draws them and adds new maps to CUSTOM MAPS.
*/
#include "../nzportable_def.h"
#include "menu_defs.h"

#ifdef __PS3__
#include "../platform/ps3/ps3_mapdl.h"

void Menu_CustomMaps_AddMap (char *map_name);	// menu_custommaps.c
void Menu_CustomMaps_RemoveMap (int index);
void Menu_DrawPicStretch (int x, int y, int index, int width, int height);	// menu_helper.c
void Menu_DrawMapBorder (int x_pos, int y_pos, int image_width, int image_height);

#define DL_PER_PAGE		6		// rows 1-6; 7/8 pages; bottom: manage, refresh, back
#define DL_NAME_CHARS	17		// what fits left of the panel

static int	dl_page;
static int	dl_shown = -1;	// list size the buttons were built for
static char	slot_names[DL_PER_PAGE][DL_NAME_CHARS + 1];

/*
===============
Menu_MapDownload_Poll: every menu frame. A finished install's maps join
the custom map list (and their thumbnails load) on the main thread.
===============
*/
void Menu_MapDownload_Poll (void)
{
	char names[16][64];
	int n = PS3_MapDL_TakeInstalled(names, 16);

	for (int i = 0; i < n; i++) {
		qboolean known = false;
		for (int j = 0; j < num_user_maps; j++) {
			if (custom_maps[j].map_name && !strcasecmp(custom_maps[j].map_name, names[i])) {
				known = true;
				break;
			}
		}
		if (known || num_user_maps >= MAX_CUSTOMMAPS)
			continue;
		Menu_CustomMaps_AddMap(names[i]);
		menu_usermap_image[num_user_maps - 1] = -1;
		if (custom_maps[num_user_maps - 1].map_use_thumbnail) {
			int img = Image_LoadImage(custom_maps[num_user_maps - 1].map_thumbnail_path,
				IMAGE_PNG | IMAGE_TGA | IMAGE_JPG, 1, false, false);
			if (img >= 0)
				menu_usermap_image[num_user_maps - 1] = img;
		}
	}
}

static qboolean Menu_MapDownload_Busy (void)
{
	int st = PS3_MapDL_State();
	return st == MAPDL_DOWNLOADING || st == MAPDL_EXTRACTING;
}

static void Menu_MapDownload_Pressed (void)
{
	int i = dl_page * DL_PER_PAGE + current_menu.cursor;

	if (!PS3_MapDL_Entry(i))
		return;
	if (Menu_MapDownload_Busy()) {
		if (PS3_MapDL_Active() == i)
			PS3_MapDL_Cancel();
		return;
	}
	PS3_MapDL_Download(i);
}

static void Menu_MapDownload_NextPage (void)
{
	Menu_ResetMenuButtons();
	Menu_SetSound(MENU_SND_ENTER);
	dl_page++;
}

static void Menu_MapDownload_PrevPage (void)
{
	Menu_ResetMenuButtons();
	Menu_SetSound(MENU_SND_ENTER);
	dl_page--;
}

static void Menu_MapDownload_Refresh (void)
{
	if (Menu_MapDownload_Busy() || PS3_MapDL_ListState() == MAPDL_LIST_LOADING)
		return;
	Menu_ResetMenuButtons();
	dl_page = 0;
	PS3_MapDL_FetchList();
}

void Menu_MapDownload_Set (void)
{
	Menu_ResetMenuButtons();
	if (PS3_MapDL_ListState() != MAPDL_LIST_READY && PS3_MapDL_ListState() != MAPDL_LIST_LOADING)
		PS3_MapDL_FetchList();
	dl_page = 0;
	dl_shown = PS3_MapDL_Count();
	key_dest = key_menu;
	m_previous_state = m_main;
	m_state = m_mapdownload;
}

static void Menu_MapDownload_Size (char *out, int size, unsigned long long bytes)
{
	if (bytes >= 1024 * 1024)
		snprintf(out, size, "%.1f MB", bytes / (1024.0 * 1024.0));
	else
		snprintf(out, size, "%llu KB", (bytes + 1023) / 1024);
}

// one line of the right-hand panel, centered like the map descriptions
static void Menu_MapDownload_Line (int line, char *text, int r, int g, int b)
{
	int image_width = vid.width / 3;
	int map_region_center = (150 + (((vid.width - 150) / 2) + 2));
	int x = map_region_center - ((image_width / vid.scale) / 2) + image_width / 2;
	int y = 40 * vid.scale + line * 12 * vid.scale;

	UI_SetAlignment(UI_ANCHOR_LEFT, UI_ANCHOR_TOP);
	Menu_DrawStringCentered(x, y, text, r, g, b, 255);
}

static void Menu_MapDownload_Info (int i)
{
	mapdl_entry_t *e = PS3_MapDL_Entry(i);
	char buf[160], a[32], b[32];
	unsigned long long got, total;
	int line = 0, st = PS3_MapDL_State();

	if (!e)
		return;
	snprintf(buf, sizeof(buf), "%s", e->name);
	strtoupper(buf);
	Menu_MapDownload_Line(line++, buf, 255, 255, 0);
	if (e->author[0]) {
		snprintf(buf, sizeof(buf), "by %s", e->author);
		Menu_MapDownload_Line(line++, buf, 255, 255, 255);
	}
	buf[0] = 0;
	if (e->version[0])
		snprintf(buf, sizeof(buf), "Version %s", e->version);
	if (e->size) {
		Menu_MapDownload_Size(a, sizeof(a), e->size);
		snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), "%s%s", buf[0] ? "   " : "", a);
	}
	if (buf[0])
		Menu_MapDownload_Line(line++, buf, 200, 200, 200);
	line++;
	for (int j = 0; j < MAPDL_DESC_LINES; j++)
		if (e->desc[j][0])
			Menu_MapDownload_Line(line++, e->desc[j], 255, 255, 255);
	line++;

	if (PS3_MapDL_Active() == i && st != MAPDL_IDLE) {
		PS3_MapDL_Progress(&got, &total);
		switch (st) {
		case MAPDL_DOWNLOADING:
			Menu_MapDownload_Size(a, sizeof(a), got);
			if (total) {
				Menu_MapDownload_Size(b, sizeof(b), total);
				snprintf(buf, sizeof(buf), "DOWNLOADING %d%%  (%s / %s)", (int)(got * 100 / total), a, b);
			} else {
				snprintf(buf, sizeof(buf), "DOWNLOADING  %s", a);
			}
			Menu_MapDownload_Line(line++, buf, 255, 255, 0);
			Menu_MapDownload_Line(line++, "Press again to cancel", 160, 160, 160);
			return;
		case MAPDL_EXTRACTING:
			snprintf(buf, sizeof(buf), "INSTALLING  %llu / %llu files", got, total);
			Menu_MapDownload_Line(line++, buf, 255, 255, 0);
			return;
		case MAPDL_DONE:
			Menu_MapDownload_Line(line++, "INSTALLED! Find it in SOLO > CUSTOM MAPS", 0, 255, 0);
			return;
		case MAPDL_FAILED:
			snprintf(buf, sizeof(buf), "FAILED: %s", PS3_MapDL_Message());
			Menu_MapDownload_Line(line++, buf, 255, 64, 64);
			Menu_MapDownload_Line(line++, "Press to try again", 160, 160, 160);
			return;
		}
	}
	if (Menu_MapDownload_Busy())
		Menu_MapDownload_Line(line++, "Another map is downloading", 160, 160, 160);
	else if (e->installed)
		Menu_MapDownload_Line(line++, "INSTALLED  (press to download again)", 0, 255, 0);
	else
		Menu_MapDownload_Line(line++, "Press to download", 255, 255, 255);
}

/*
===============
MANAGE MAPS: the installed custom maps, press one to delete it
===============
*/
static int	mm_page;
static int	mm_list[MAX_CUSTOMMAPS];
static int	mm_count;
static int	mm_confirm = -1;	// custom_maps index waiting for YES/NO
static char	mm_names[DL_PER_PAGE][DL_NAME_CHARS + 1];
static char	mm_msg[96];

static void Menu_MapManage_Build (void)
{
	mm_count = 0;
	for (int i = 0; i < num_user_maps; i++)
		if (custom_maps[i].map_name && !Menu_IsStockMap(custom_maps[i].map_name))
			mm_list[mm_count++] = i;
	if (mm_page * DL_PER_PAGE >= mm_count && mm_page > 0)
		mm_page = (mm_count - 1) / DL_PER_PAGE;
}

void Menu_MapManage_Set (void)
{
	Menu_ResetMenuButtons();
	mm_page = 0;
	mm_confirm = -1;
	mm_msg[0] = 0;
	Menu_MapManage_Build();
	key_dest = key_menu;
	m_previous_state = m_mapdownload;
	m_state = m_mapmanage;
}

static void Menu_MapManage_Pressed (void)
{
	int i = mm_page * DL_PER_PAGE + current_menu.cursor;
	if (i < 0 || i >= mm_count)
		return;
	mm_confirm = mm_list[i];
	Menu_ResetMenuButtons();
}

static void Menu_MapManage_Yes (void)
{
	char bsp[64];
	int n;

	if (mm_confirm < 0 || mm_confirm >= num_user_maps) { mm_confirm = -1; return; }
	snprintf(bsp, sizeof(bsp), "%s", custom_maps[mm_confirm].map_name);
	n = PS3_MapDL_Delete(bsp);
	if (n < 0) {
		snprintf(mm_msg, sizeof(mm_msg), "Wait for the download to finish");
	} else {
		snprintf(mm_msg, sizeof(mm_msg), "Deleted %s (%d files)", custom_maps[mm_confirm].map_name_pretty, n);
		Menu_CustomMaps_RemoveMap(mm_confirm);
	}
	mm_confirm = -1;
	Menu_MapManage_Build();
	Menu_ResetMenuButtons();
}

static void Menu_MapManage_No (void)
{
	mm_confirm = -1;
	Menu_ResetMenuButtons();
}

static void Menu_MapManage_NextPage (void) { Menu_ResetMenuButtons(); Menu_SetSound(MENU_SND_ENTER); mm_page++; }
static void Menu_MapManage_PrevPage (void) { Menu_ResetMenuButtons(); Menu_SetSound(MENU_SND_ENTER); mm_page--; }

void Menu_MapManage_Draw (void)
{
	int items = 0, start = mm_page * DL_PER_PAGE, on_page, i;

	Menu_DrawCustomBackground(true);
	Menu_DrawTitle("MANAGE MAPS", MENU_COLOR_WHITE);
	Menu_DrawMapPanel();

	on_page = mm_count - start;
	if (on_page > DL_PER_PAGE)
		on_page = DL_PER_PAGE;
	for (i = 0; i < on_page; i++) {
		usermap_t *m = &custom_maps[mm_list[start + i]];
		snprintf(mm_names[i], sizeof(mm_names[i]), "%s", m->map_name_pretty ? m->map_name_pretty : m->map_name);
		strtoupper(mm_names[i]);
	}

	if (mm_confirm >= 0) {
		char line[128];
		for (i = 0; i < on_page; i++)
			Menu_DrawGreyButton(i + 1, mm_names[i]);
		snprintf(line, sizeof(line), "Delete %s?", custom_maps[mm_confirm].map_name_pretty);
		Menu_DrawSubMenu(line, "Its files are removed from the PS3.");
		Menu_DrawButton(7, 0, "DELETE IT", "", Menu_MapManage_Yes);
		Menu_DrawButton(8, 1, "KEEP IT", "", Menu_MapManage_No);
		return;
	}

	if (mm_count == 0)
		Menu_MapDownload_Line(0, "No custom maps installed", 255, 255, 255);
	else if (mm_msg[0])
		Menu_MapDownload_Line(0, mm_msg, 0, 255, 0);
	else
		Menu_MapDownload_Line(0, "Press a map to delete it", 200, 200, 200);

	for (i = 0; i < on_page; i++) {
		usermap_t *m = &custom_maps[mm_list[start + i]];
		Menu_DrawButton(i + 1, items++, mm_names[i], m->map_author && m->map_author[0] ? m->map_author : "Delete this map.", Menu_MapManage_Pressed);
	}
	if (current_menu.cursor >= 0 && current_menu.cursor < on_page) {
		usermap_t *m = &custom_maps[mm_list[start + current_menu.cursor]];
		int img = menu_usermap_image[mm_list[start + current_menu.cursor]];
		int image_width = vid.width / 3, image_height = vid.height / 3;
		int x = (150 + (((vid.width - 150) / 2) + 2)) - ((image_width / vid.scale) / 2);
		if (img >= 0) {
			UI_SetAlignment(UI_ANCHOR_LEFT, UI_ANCHOR_TOP);
			Menu_DrawPicStretch(x, 66 * vid.scale, img, image_width, image_height);
			Menu_DrawMapBorder(x / vid.scale, 66, image_width, image_height);
		}
		Menu_MapDownload_Line(1, m->map_name, 160, 160, 160);
	}

	Menu_DrawDivider(7);
	if (start + on_page < mm_count)
		Menu_DrawButton(7, items++, "NEXT PAGE", "Show more maps.", Menu_MapManage_NextPage);
	else
		Menu_DrawGreyButton(7, "NEXT PAGE");
	if (mm_page > 0)
		Menu_DrawButton(8, items++, "PREVIOUS PAGE", "Show the last page.", Menu_MapManage_PrevPage);
	else
		Menu_DrawGreyButton(8, "PREVIOUS PAGE");
	Menu_DrawButton(-1, items, "BACK", "Return to Download Maps.", Menu_MapDownload_Set);
}

void Menu_MapDownload_Draw (void)
{
	int items = 0, count, start, on_page, i;
	int list = PS3_MapDL_ListState();

	Menu_DrawCustomBackground(true);
	Menu_DrawTitle("DOWNLOAD MAPS", MENU_COLOR_WHITE);
	Menu_DrawMapPanel();

	count = PS3_MapDL_Count();
	if (count != dl_shown) {
		// the list arrived (or changed): buttons were built for another layout
		Menu_ResetMenuButtons();
		dl_shown = count;
	}
	start = dl_page * DL_PER_PAGE;
	if (start >= count && dl_page > 0) {
		dl_page = 0;
		start = 0;
	}
	on_page = count - start;
	if (on_page > DL_PER_PAGE)
		on_page = DL_PER_PAGE;

	if (list == MAPDL_LIST_LOADING) {
		Menu_MapDownload_Line(0, "Fetching the map list...", 255, 255, 255);
	} else if (list == MAPDL_LIST_FAILED) {
		Menu_MapDownload_Line(0, "Couldn't get the map list", 255, 64, 64);
		Menu_MapDownload_Line(1, (char *)PS3_MapDL_Message(), 200, 200, 200);
		Menu_MapDownload_Line(3, "Check the PS3 is online, then REFRESH", 160, 160, 160);
	} else if (list == MAPDL_LIST_READY && count == 0) {
		Menu_MapDownload_Line(0, "The map list is empty", 255, 255, 255);
	}

	for (i = 0; i < on_page; i++) {
		mapdl_entry_t *e = PS3_MapDL_Entry(start + i);
		snprintf(slot_names[i], sizeof(slot_names[i]), "%s", e->name);
		strtoupper(slot_names[i]);
		Menu_DrawButton(i + 1, items++, slot_names[i],
			e->installed ? "Installed. Press to download again." : "Download and install this map.",
			Menu_MapDownload_Pressed);
	}
	if (current_menu.cursor >= 0 && current_menu.cursor < on_page)
		Menu_MapDownload_Info(start + current_menu.cursor);

	Menu_DrawDivider(7);
	if (start + on_page < count)
		Menu_DrawButton(7, items++, "NEXT PAGE", "Show more maps.", Menu_MapDownload_NextPage);
	else
		Menu_DrawGreyButton(7, "NEXT PAGE");
	if (dl_page > 0)
		Menu_DrawButton(8, items++, "PREVIOUS PAGE", "Show the last page.", Menu_MapDownload_PrevPage);
	else
		Menu_DrawGreyButton(8, "PREVIOUS PAGE");

	// always buttons, so the indices (and the callbacks they keep) never shift
	Menu_DrawButton(-3, items++, "MANAGE MAPS", "See and delete installed maps.", Menu_MapManage_Set);
	Menu_DrawButton(-2, items++, "REFRESH", "Fetch the map list again.", Menu_MapDownload_Refresh);
	Menu_DrawButton(-1, items, "BACK", "Return to Main Menu. Downloads carry on.", Menu_Main_Set);
}
#endif
