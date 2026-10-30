/*
Copyright (C) 1996-1997 Id Software, Inc.

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
// net_loop.c

#include "nzportable_def.h"
#include "net_loop.h"

// split screen: pair 0 is "local", pair 1 "local2" (player 2's own client)
#define LOOP_PAIRS 2
qboolean	localconnectpending = false;
static qboolean	loop_pending[LOOP_PAIRS];
static qsocket_t	*loop_clients[LOOP_PAIRS];
static qsocket_t	*loop_servers[LOOP_PAIRS];
qsocket_t	*loop_client = NULL;	// pair 0's, as before
qsocket_t	*loop_server = NULL;

int Loop_Init (void)
{
	if (cls.state == ca_dedicated)
		return -1;
	return 0;
}


void Loop_Shutdown (void)
{
}


void Loop_Listen (qboolean state)
{
}


void Loop_SearchForHosts (qboolean xmit)
{
	if (!sv.active)
		return;

	hostCacheCount = 1;
	if (strcmp(hostname.string, "UNNAMED") == 0)
		Q_strcpy(hostcache[0].name, "local");
	else
		Q_strcpy(hostcache[0].name, hostname.string);
	Q_strcpy(hostcache[0].map, sv.name);
	hostcache[0].users = net_activeconnections;
	hostcache[0].maxusers = svs.maxclients;
	hostcache[0].driver = net_driverlevel;
	Q_strcpy(hostcache[0].cname, "local");
}


qsocket_t *Loop_Connect (char *host)
{
	int p;

	if (strcmp(host,"local") == 0)
		p = 0;
	else if (strcmp(host,"local2") == 0)
		p = 1;
	else
		return NULL;

	loop_pending[p] = true;
	if (p == 0)
		localconnectpending = true;

	if (!loop_clients[p])
	{
		if ((loop_clients[p] = NET_NewQSocket ()) == NULL)
		{
			Con_Printf("Loop_Connect: no qsocket available\n");
			return NULL;
		}
		Q_strcpy (loop_clients[p]->address, p ? "localhost2" : "localhost");
	}
	loop_clients[p]->receiveMessageLength = 0;
	loop_clients[p]->sendMessageLength = 0;
	loop_clients[p]->canSend = true;

	if (!loop_servers[p])
	{
		if ((loop_servers[p] = NET_NewQSocket ()) == NULL)
		{
			Con_Printf("Loop_Connect: no qsocket available\n");
			return NULL;
		}
		Q_strcpy (loop_servers[p]->address, p ? "LOCAL2" : "LOCAL");
	}
	loop_servers[p]->receiveMessageLength = 0;
	loop_servers[p]->sendMessageLength = 0;
	loop_servers[p]->canSend = true;

	loop_clients[p]->driverdata = (void *)loop_servers[p];
	loop_servers[p]->driverdata = (void *)loop_clients[p];

	loop_client = loop_clients[0];
	loop_server = loop_servers[0];
	return loop_clients[p];
}


qsocket_t *Loop_CheckNewConnections (void)
{
	int p;

	for (p = 0; p < LOOP_PAIRS; p++)
	{
		if (!loop_pending[p])
			continue;
		loop_pending[p] = false;
		if (p == 0)
			localconnectpending = false;
		loop_servers[p]->sendMessageLength = 0;
		loop_servers[p]->receiveMessageLength = 0;
		loop_servers[p]->canSend = true;
		loop_clients[p]->sendMessageLength = 0;
		loop_clients[p]->receiveMessageLength = 0;
		loop_clients[p]->canSend = true;
		return loop_servers[p];
	}
	return NULL;
}


static int IntAlign(int value)
{
	return (value + (sizeof(int) - 1)) & (~(sizeof(int) - 1));
}


int Loop_GetMessage (qsocket_t *sock)
{
	int		ret;
	int		length;

	if (sock->receiveMessageLength == 0)
		return 0;

	ret = sock->receiveMessage[0];
	length = sock->receiveMessage[1] + (sock->receiveMessage[2] << 8);
	// alignment byte skipped here
	SZ_Clear (&net_message);
	SZ_Write (&net_message, &sock->receiveMessage[4], length);

	length = IntAlign(length + 4);
	sock->receiveMessageLength -= length;

	if (sock->receiveMessageLength)
		Q_memcpy(sock->receiveMessage, &sock->receiveMessage[length], sock->receiveMessageLength);

	if (sock->driverdata && ret == 1)
		((qsocket_t *)sock->driverdata)->canSend = true;

	return ret;
}


int Loop_SendMessage (qsocket_t *sock, sizebuf_t *data)
{
	byte *buffer;
	int  *bufferLength;

	if (!sock->driverdata)
		return -1;

	bufferLength = &((qsocket_t *)sock->driverdata)->receiveMessageLength;

	if ((*bufferLength + data->cursize + 4) > NET_MAXMESSAGE)
		Sys_Error("overflow\n");

	buffer = ((qsocket_t *)sock->driverdata)->receiveMessage + *bufferLength;

	// message type
	*buffer++ = 1;

	// length
	*buffer++ = data->cursize & 0xff;
	*buffer++ = data->cursize >> 8;

	// align
	buffer++;

	// message
	Q_memcpy(buffer, data->data, data->cursize);
	*bufferLength = IntAlign(*bufferLength + data->cursize + 4);

	sock->canSend = false;
	return 1;
}


int Loop_SendUnreliableMessage (qsocket_t *sock, sizebuf_t *data)
{
	byte *buffer;
	int  *bufferLength;

	if (!sock->driverdata)
		return -1;

	bufferLength = &((qsocket_t *)sock->driverdata)->receiveMessageLength;

	if ((*bufferLength + data->cursize + sizeof(byte) + sizeof(short)) > NET_MAXMESSAGE)
		return 0;

	buffer = ((qsocket_t *)sock->driverdata)->receiveMessage + *bufferLength;

	// message type
	*buffer++ = 2;

	// length
	*buffer++ = data->cursize & 0xff;
	*buffer++ = data->cursize >> 8;

	// align
	buffer++;

	// message
	Q_memcpy(buffer, data->data, data->cursize);
	*bufferLength = IntAlign(*bufferLength + data->cursize + 4);
	return 1;
}


qboolean Loop_CanSendMessage (qsocket_t *sock)
{
	if (!sock->driverdata)
		return false;
	return sock->canSend;
}


qboolean Loop_CanSendUnreliableMessage (qsocket_t *sock)
{
	return true;
}


void Loop_Close (qsocket_t *sock)
{
	if (sock->driverdata)
		((qsocket_t *)sock->driverdata)->driverdata = NULL;
	sock->receiveMessageLength = 0;
	sock->sendMessageLength = 0;
	sock->canSend = true;
	for (int p = 0; p < LOOP_PAIRS; p++)
	{
		if (sock == loop_clients[p])
			loop_clients[p] = NULL;
		else if (sock == loop_servers[p])
			loop_servers[p] = NULL;
	}
	loop_client = loop_clients[0];
	loop_server = loop_servers[0];
}
