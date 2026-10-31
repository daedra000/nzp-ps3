/*
ps3_board.h: reading the ps3.world leaderboards for the LEADERBOARDS menu (ps3_board.c).
*/
#ifndef PS3_BOARD_H
#define PS3_BOARD_H

#define BOARD_MAX_MAPS	64
#define BOARD_MAX_ROWS	10

enum { BOARD_NONE, BOARD_LOADING, BOARD_READY, BOARD_FAILED };

typedef struct {
	char	id[64];			// the .bsp name: what /api/scores takes
	char	name[64];		// "Nacht der Untoten"
	int		games, best, custom;
} board_map_t;

typedef struct {
	char	name[24], name2[24];
	int		value, value2, seconds, players, record;
} board_row_t;

typedef struct {
	int			state;		// BOARD_*
	int			games, players, best;
	int			num_rows;
	board_row_t	rows[BOARD_MAX_ROWS];
} board_t;

// the map tabs: stock maps, then custom maps with scores, most played first
void			PS3_Board_FetchMaps(void);
int				PS3_Board_MapsState(void);
int				PS3_Board_NumMaps(void);
const board_map_t *PS3_Board_Map(int i);

// one board (map, solo or split screen, all time or this week); a copy of
// the latest answer for exactly that board, or LOADING/FAILED
void			PS3_Board_Want(const char *map, int duo, int week);
void			PS3_Board_Get(board_t *out);
const char		*PS3_Board_Message(void);	// why it failed

#endif
