/*
cl_seats.c: split screen. Two clients in one process ("seats"), each with its
own connection to the local server (loopback "local" and "local2").

The client code keeps its state in globals (cl, cls, cl_dlights, the HUD's
counters...). Rather than rewrite it around a pointer, each file registers
the globals that belong to one player (Seat_Register), and CL_SetSeat swaps
them: the current seat's values are saved, the other seat's put in place.
Everything not registered (models, the world, static entities, particles,
sounds) is shared, and only seat 0 parses the messages that set it up.
*/
#include "nzportable_def.h"

#define MAX_SEAT_VARS	256

typedef struct {
	void	*ptr;
	int		size;
	int		offset;		// in each seat's store
} seat_var_t;

static seat_var_t	seat_vars[MAX_SEAT_VARS];
static int			num_seat_vars;
static int			seat_store_size;
static byte			*seat_store[MAX_SEATS];
static qboolean		seats_ready;

int		cl_numseats = 1;	// seats in the current game
int		cl_curseat;			// the seat whose state is in the globals

cvar_t	cl_splitscreen = {"cl_splitscreen", "0", false};

/*
Seat_Register: ptr..ptr+size belongs to one player. Call at init, before
Seat_Init; every seat starts with what's there now.
*/
void Seat_Register (void *ptr, int size)
{
	if (seats_ready)
		Sys_Error ("Seat_Register after Seat_Init");
	if (num_seat_vars >= MAX_SEAT_VARS)
		Sys_Error ("Seat_Register: too many (%d)", MAX_SEAT_VARS);
	seat_vars[num_seat_vars].ptr = ptr;
	seat_vars[num_seat_vars].size = size;
	seat_vars[num_seat_vars].offset = seat_store_size;
	seat_store_size += (size + 15) & ~15;
	num_seat_vars++;
}

static void Seat_Save (int seat)
{
	for (int i = 0; i < num_seat_vars; i++)
		memcpy (seat_store[seat] + seat_vars[i].offset, seat_vars[i].ptr, seat_vars[i].size);
}

static void Seat_Load (int seat)
{
	for (int i = 0; i < num_seat_vars; i++)
		memcpy (seat_vars[i].ptr, seat_store[seat] + seat_vars[i].offset, seat_vars[i].size);
}

void Seat_Init (void)
{
	int i;

	for (i = 0; i < MAX_SEATS; i++) {
		seat_store[i] = malloc (seat_store_size ? seat_store_size : 16);
		if (!seat_store[i])
			Sys_Error ("Seat_Init: out of memory (%d bytes)", seat_store_size);
	}
	seats_ready = true;
	for (i = 0; i < MAX_SEATS; i++)
		Seat_Save (i);

	// what each seat must not share: its entities and its message buffer
	for (i = 1; i < MAX_SEATS; i++) {
		CL_SetSeat (i);
		cl_entities = CL_SeatEntities (i);
		cls.state = ca_disconnected;
		cls.netcon = NULL;
		cls.signon = 0;
		cls.message.data = malloc (1024);
		cls.message.maxsize = 1024;
		cls.message.cursize = 0;
		if (!cls.message.data)
			Sys_Error ("Seat_Init: out of memory");
	}
	CL_SetSeat (0);
	Con_DPrintf ("Seat_Init: %d variables, %d bytes a seat\n", num_seat_vars, seat_store_size);
}

static void CL_SeatSyncPlayer (void);
static void CL_SeatSyncPlayer (void)
{
	// the client side reads its player's server edict (listen server only)
	if (sv.active && cl.viewentity > 0 && cl.viewentity < sv.num_edicts)
		sv_player = EDICT_NUM(cl.viewentity);
}

/*
CL_SetSeat: put seat n's state in the globals.
*/
void CL_SetSeat (int n)
{
	if (!seats_ready || n < 0 || n >= MAX_SEATS)
		return;
	if (n != cl_curseat) {
		Seat_Save (cl_curseat);
		Seat_Load (n);
		cl_curseat = n;
	}
	CL_SeatSyncPlayer ();
}

/*
CL_Seats_Drop: seat 1 leaves (back to one view). The server going down or
changing map calls it first, so seat 1 joins again after seat 0.
*/
void CL_Seats_Drop (void)
{
	int old = cl_curseat;

	if (cl_numseats < 2 || !seats_ready)
		return;
	CL_SetSeat (1);
	if (cls.state == ca_connected && cls.netcon) {
		SZ_Clear (&cls.message);
		MSG_WriteByte (&cls.message, clc_disconnect);
		NET_SendUnreliableMessage (cls.netcon, &cls.message);
		SZ_Clear (&cls.message);
		NET_Close (cls.netcon);
	}
	cls.netcon = NULL;
	cls.state = ca_disconnected;
	cls.signon = 0;
	CL_SetSeat (old == 1 ? 0 : old);
	cl_numseats = 1;
	vid.recalc_refdef = true;
}

/*
CL_Seats_Frame: called each frame. In a split game, once seat 0 is in, seat 1
joins the local server too.
*/
void CL_Seats_Frame (void)
{
	int want = (cl_splitscreen.value && sv.active) ? 2 : 1;

	if (cl_curseat != 0)
		CL_SetSeat (0);

	if (want < cl_numseats) {
		CL_Seats_Drop ();
		return;
	}

	if (want == 2 && cl_numseats == 1 && cls.state == ca_connected && cls.signon >= 1 && sv.active) {
		struct qsocket_s *sock;
		CL_SetSeat (1);
		sock = NET_Connect ("local2");
		if (sock) {
			cls.netcon = sock;
			cls.state = ca_connected;
			cls.signon = 0;
			cls.demonum = -1;
			SZ_Clear (&cls.message);
			cl_numseats = 2;
			Con_Printf ("Split screen: player 2 joined\n");
		} else {
			Con_Printf ("Split screen: player 2 couldn't connect\n");
			Cvar_SetValue ("cl_splitscreen", 0);
		}
		CL_SetSeat (0);
		vid.recalc_refdef = true;
	}
}

/*
seat <n> <command>: runs a command as seat n (pad 2's buttons come this way)
*/
static void CL_Seat_f (void)
{
	int n, old = cl_curseat;
	char *args;

	if (Cmd_Argc () < 3)
		return;
	n = Q_atoi (Cmd_Argv (1));
	if (n < 0 || n >= cl_numseats)
		return;
	args = Cmd_Args ();
	// skip "<n> "
	while (*args == ' ') args++;
	while (*args && *args != ' ') args++;
	while (*args == ' ') args++;
	if (!*args)
		return;
	CL_SetSeat (n);
	Cmd_ExecuteString (args, src_command);
	CL_SetSeat (old);
}

void CL_Seats_Init (void)
{
	Cvar_RegisterVariable (&cl_splitscreen);
	Cmd_AddCommand ("seat", CL_Seat_f);
}

/*
A seat other than 0 parses the server's messages for its own player only:
the world, sounds, effects and music are seat 0's.
*/
qboolean CL_SeatIsQuiet (void)
{
	return cl_curseat != 0;
}
