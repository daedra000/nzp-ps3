/*
ps3_mapdl.h: the map downloader (ps3_mapdl.c).

A list (maps.json) is fetched from ps3_maps_url over HTTP(S). Each map's zip
is downloaded to nzp/ and unpacked there, on a worker thread. The menu polls.
*/
#ifndef PS3_MAPDL_H
#define PS3_MAPDL_H

#define MAPDL_MAX_MAPS		128
#define MAPDL_DESC_LINES	6

typedef struct {
	char		name[64];
	char		author[64];
	char		version[24];
	char		bsp[64];			// maps/<bsp>.bsp: how "installed" is decided
	char		url[512];
	unsigned	size;				// bytes, from the list (0 = unknown)
	char		desc[MAPDL_DESC_LINES][64];
	int			installed;
} mapdl_entry_t;

enum { MAPDL_LIST_NONE, MAPDL_LIST_LOADING, MAPDL_LIST_READY, MAPDL_LIST_FAILED };
enum { MAPDL_IDLE, MAPDL_DOWNLOADING, MAPDL_EXTRACTING, MAPDL_DONE, MAPDL_FAILED };

void			PS3_MapDL_Init(void);			// registers ps3_maps_url
void			PS3_MapDL_FetchList(void);		// (re)loads the list
int				PS3_MapDL_ListState(void);
int				PS3_MapDL_Count(void);
mapdl_entry_t	*PS3_MapDL_Entry(int i);

void			PS3_MapDL_Download(int i);		// one at a time
void			PS3_MapDL_Cancel(void);
int				PS3_MapDL_State(void);
int				PS3_MapDL_Active(void);			// index being downloaded, or -1
void			PS3_MapDL_Progress(unsigned long long *got, unsigned long long *total);
const char		*PS3_MapDL_Message(void);		// last error / result

// main thread: deletes maps/<bsp> and what its download put down; -1 while busy
int				PS3_MapDL_Delete(const char *bsp);

// main thread: the .bsp names a finished install added, once (returns count)
int				PS3_MapDL_TakeInstalled(char names[][64], int max);

#endif
