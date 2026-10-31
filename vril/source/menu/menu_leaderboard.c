/*
menu_leaderboard.c: LEADERBOARDS (PS3). The ps3.world boards, read by
platform/ps3/ps3_board.c: the maps on the left, the highlighted map's top 10
on the right. Solo or split screen, all time or this week, and the switch for
sending your own scores (ps3_leaderboard).
*/
#include "../nzportable_def.h"
#include "menu_defs.h"

#ifdef __PS3__
#include "../platform/ps3/ps3_board.h"

extern cvar_t ps3_leaderboard;	// ps3_scores.c

#define LB_PER_PAGE		5		// rows 1-5; 6/7 pages; bottom: sending, mode, time, back
#define LB_NAME_CHARS	17

static int		lb_page;
static int		lb_sel = -1;		// the map shown on the right (stays when the cursor leaves the maps)
static int		lb_duo, lb_week;
static int		lb_shown = -1;		// map count the buttons were built for
static double	lb_maps_time = -1;
static char		lb_slot_names[LB_PER_PAGE][LB_NAME_CHARS + 1];

static void Menu_Leaderboard_Pressed (void)
{
	Menu_SetSound(MENU_SND_ENTER);		// the board's already on the right
}

static void Menu_Leaderboard_NextPage (void)
{
	Menu_ResetMenuButtons();
	Menu_SetSound(MENU_SND_ENTER);
	lb_page++;
}

static void Menu_Leaderboard_PrevPage (void)
{
	Menu_ResetMenuButtons();
	Menu_SetSound(MENU_SND_ENTER);
	lb_page--;
}

static void Menu_Leaderboard_Mode (void)
{
	Menu_SetSound(MENU_SND_ENTER);
	lb_duo = !lb_duo;
}

static void Menu_Leaderboard_Time (void)
{
	Menu_SetSound(MENU_SND_ENTER);
	lb_week = !lb_week;
}

static void Menu_Leaderboard_Sending (void)
{
	Menu_SetSound(MENU_SND_ENTER);
	Cvar_SetValue("ps3_leaderboard", ps3_leaderboard.value ? 0 : 1);
}

void Menu_Leaderboard_Set (void)
{
	int st = PS3_Board_MapsState();

	Menu_ResetMenuButtons();
	Menu_SetSound(MENU_SND_ENTER);
	// the map tabs: again if they failed or are over 2 minutes old (new custom maps)
	if (st == BOARD_NONE || st == BOARD_FAILED || (st == BOARD_READY && realtime - lb_maps_time > 120))
		PS3_Board_FetchMaps();
	lb_page = 0;
	lb_sel = -1;
	lb_shown = PS3_Board_NumMaps();
	key_dest = key_menu;
	m_previous_state = m_main;
	m_state = m_leaderboard;
}

/*
===============
The right-hand panel (pixels)
===============
*/
static int lb_left (void) { return (150 + 8) * vid.scale; }
static int lb_right (void) { return vid.width - 8 * vid.scale; }
static int lb_center (void) { return (lb_left() + lb_right()) / 2; }

static void Menu_Leaderboard_Text (int x, int y, char *text, int r, int g, int b)
{
	UI_SetAlignment(UI_ANCHOR_LEFT, UI_ANCHOR_TOP);
	Draw_ColoredString(x, y, text, r, g, b, 255, vid.scale);
}

static void Menu_Leaderboard_TextRight (int x, int y, char *text, int r, int g, int b)
{
	Menu_Leaderboard_Text(x - getTextWidth(text, vid.scale), y, text, r, g, b);
}

static void Menu_Leaderboard_Centered (int y, char *text, int r, int g, int b)
{
	char fit[128];
	int w = lb_right() - lb_left();

	snprintf(fit, sizeof(fit), "%s", text);
	for (int l = strlen(fit); l > 3 && getTextWidth(fit, vid.scale) > w; )
		fit[--l] = 0;
	UI_SetAlignment(UI_ANCHOR_LEFT, UI_ANCHOR_TOP);
	Menu_DrawStringCentered(lb_center(), y, fit, r, g, b, 255);
}

static void Menu_Leaderboard_Panel (const board_map_t *m)
{
	board_t b;
	char buf[128], name[64];
	int lh = 12 * vid.scale, y = 40 * vid.scale;
	int bottom = vid.height - (big_bar_height + small_bar_height + 2) * vid.scale - lh;
	int col_rank = lb_left() + 4 * vid.scale;
	int col_name = col_rank + getTextWidth("10 ", vid.scale);
	int col_kills = lb_right() - 4 * vid.scale;			// right edges
	int col_round = col_kills - getTextWidth("KILLS  ", vid.scale);
	int name_room = col_round - getTextWidth("ROUND ", vid.scale) - col_name;

	PS3_Board_Want(m->id, lb_duo, lb_week);
	PS3_Board_Get(&b);

	snprintf(buf, sizeof(buf), "%s", m->name);
	strtoupper(buf);
	Menu_Leaderboard_Centered(y, buf, 255, 255, 0);
	y += lh;

	if (b.state == BOARD_LOADING) {
		snprintf(buf, sizeof(buf), "%s, %s", lb_duo ? "Split screen" : "Solo", lb_week ? "this week" : "all time");
		Menu_Leaderboard_Centered(y, buf, 200, 200, 200);
		Menu_Leaderboard_Centered(y + 2 * lh, "Loading...", 255, 255, 255);
		return;
	}
	if (b.state == BOARD_FAILED) {
		Menu_Leaderboard_Centered(y + lh, "Couldn't reach ps3.world", 255, 64, 64);
		Menu_Leaderboard_Centered(y + 2 * lh, (char *)PS3_Board_Message(), 200, 200, 200);
		Menu_Leaderboard_Centered(y + 4 * lh, "Check the PS3 is online", 160, 160, 160);
		return;
	}

	// "Solo, all time: 12 games, best round 23"
	if (b.games)
		snprintf(buf, sizeof(buf), "%s, %s: %d game%s, best round %d", lb_duo ? "Split screen" : "Solo",
			lb_week ? "this week" : "all time", b.games, b.games == 1 ? "" : "s", b.best);
	else
		snprintf(buf, sizeof(buf), "%s, %s", lb_duo ? "Split screen" : "Solo", lb_week ? "this week" : "all time");
	Menu_Leaderboard_Centered(y, buf, 200, 200, 200);
	y += lh + 4 * vid.scale;

	if (!b.num_rows) {
		Menu_Leaderboard_Centered(y + lh, lb_week ? "No scores this week. Be the first!" : "No scores yet. Be the first!",
			255, 255, 255);
		Menu_Leaderboard_Centered(y + 3 * lh, "Play a classic game from round 1", 160, 160, 160);
		Menu_Leaderboard_Centered(y + 4 * lh, "and your score is sent at game over.", 160, 160, 160);
		return;
	}

	Menu_Leaderboard_Text(col_rank, y, "#", 160, 160, 160);
	Menu_Leaderboard_Text(col_name, y, lb_duo ? "PLAYERS" : "PLAYER", 160, 160, 160);
	Menu_Leaderboard_TextRight(col_round, y, "ROUND", 160, 160, 160);
	Menu_Leaderboard_TextRight(col_kills, y, "KILLS", 160, 160, 160);
	y += lh;

	for (int i = 0; i < b.num_rows && y <= bottom; i++, y += lh) {
		board_row_t *r = &b.rows[i];
		// gold, silver, bronze, then white
		static const int colors[4][3] = {{255, 215, 0}, {210, 210, 220}, {205, 127, 50}, {255, 255, 255}};
		const int *c = colors[i < 3 ? i : 3];

		snprintf(buf, sizeof(buf), "%d", i + 1);
		Menu_Leaderboard_Text(col_rank, y, buf, c[0], c[1], c[2]);
		if (r->players > 1)
			snprintf(name, sizeof(name), "%s + %s", r->name, r->name2[0] ? r->name2 : "Player 2");
		else
			snprintf(name, sizeof(name), "%s", r->name);
		for (int l = strlen(name); l > 1 && getTextWidth(name, vid.scale) > name_room; )
			name[--l] = 0;
		Menu_Leaderboard_Text(col_name, y, name, c[0], c[1], c[2]);
		snprintf(buf, sizeof(buf), "%d", r->value);
		Menu_Leaderboard_TextRight(col_round, y, buf, c[0], c[1], c[2]);
		snprintf(buf, sizeof(buf), "%d", r->value2);
		Menu_Leaderboard_TextRight(col_kills, y, buf, 200, 200, 200);
	}
}

void Menu_Leaderboard_Draw (void)
{
	int items = 0, count, start, on_page, i, st = PS3_Board_MapsState();
	int lh = 12 * vid.scale;

	Menu_DrawCustomBackground(true);
	Menu_DrawTitle("LEADERBOARDS", MENU_COLOR_WHITE);
	Menu_DrawMapPanel();

	count = PS3_Board_NumMaps();
	if (st == BOARD_READY && lb_maps_time < 0)
		lb_maps_time = realtime;
	if (count != lb_shown) {
		Menu_ResetMenuButtons();		// the tabs arrived: buttons were built for none
		lb_shown = count;
	}
	start = lb_page * LB_PER_PAGE;
	if (start >= count && lb_page > 0) {
		lb_page = 0;
		start = 0;
	}
	on_page = count - start;
	if (on_page > LB_PER_PAGE)
		on_page = LB_PER_PAGE;

	for (i = 0; i < on_page; i++) {
		const board_map_t *m = PS3_Board_Map(start + i);
		snprintf(lb_slot_names[i], sizeof(lb_slot_names[i]), "%s", m->name);
		strtoupper(lb_slot_names[i]);
		Menu_DrawButton(i + 1, items++, lb_slot_names[i],
			m->custom ? "Custom map: the top 10 at ps3.world/leaderboards." : "The top 10 at ps3.world/leaderboards.",
			Menu_Leaderboard_Pressed);
	}
	if (current_menu.cursor >= 0 && current_menu.cursor < on_page)
		lb_sel = start + current_menu.cursor;
	if (lb_sel < 0 && count)
		lb_sel = 0;

	if (count && PS3_Board_Map(lb_sel))
		Menu_Leaderboard_Panel(PS3_Board_Map(lb_sel));
	else if (st == BOARD_FAILED) {
		Menu_Leaderboard_Centered(40 * vid.scale, "Couldn't reach ps3.world", 255, 64, 64);
		Menu_Leaderboard_Centered(40 * vid.scale + lh, (char *)PS3_Board_Message(), 200, 200, 200);
		Menu_Leaderboard_Centered(40 * vid.scale + 3 * lh, "Check the PS3 is online, then come back here", 160, 160, 160);
	} else
		Menu_Leaderboard_Centered(40 * vid.scale, "Fetching the leaderboards...", 255, 255, 255);

	Menu_DrawDivider(6);
	if (start + on_page < count)
		Menu_DrawButton(6, items++, "NEXT PAGE", "Show more maps.", Menu_Leaderboard_NextPage);
	else
		Menu_DrawGreyButton(6, "NEXT PAGE");
	if (lb_page > 0)
		Menu_DrawButton(7, items++, "PREVIOUS PAGE", "Show the last page.", Menu_Leaderboard_PrevPage);
	else
		Menu_DrawGreyButton(7, "PREVIOUS PAGE");

	// always buttons, so their indices never shift
	Menu_DrawButton(-4, items++, ps3_leaderboard.value ? "SEND SCORES: ON" : "SEND SCORES: OFF",
		"Send your classic games to ps3.world at game over.", Menu_Leaderboard_Sending);
	Menu_DrawButton(-3, items++, lb_duo ? "SPLIT SCREEN" : "SOLO", "Solo or split screen boards.", Menu_Leaderboard_Mode);
	Menu_DrawButton(-2, items++, lb_week ? "THIS WEEK" : "ALL TIME", "All time or this week's best.", Menu_Leaderboard_Time);
	Menu_DrawButton(-1, items, "BACK", "Return to Main Menu.", Menu_Main_Set);
}
#endif
