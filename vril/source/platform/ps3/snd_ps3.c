/*
Copyright (C) 1996-1997 Id Software, Inc.
Copyright (C) 2026 NZ:P PS3 port contributors

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
// snd_ps3.c -- PS3 audio port (PSL1GHT libaudio) behind Quake's DMA ring.
//
// The port setup and the event-driven feeding thread are IoQuake3-PS3's
// (code/audio/ps3_snd.c, on hardware): 2 channels, 8 blocks of 256 float frames at
// 48 kHz, a thread woken by the port's notify queue that writes the block after the
// one being played. Quake mixes into an int16 ring at 48 kHz; the thread converts it
// and adds the music (music_ps3.c), so effects and music share the one port.

#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_local.h"
#include <lv2/systime.h>

#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/event_queue.h>
#include <sysmodule/sysmodule.h>

#define PS3_AUDIO_RATE      48000
#define PS3_AUDIO_CHANNELS  2
#define PS3_AUDIO_BLOCK     256                 // frames per hardware block
#define PS3_RING_FRAMES     16384               // Quake's ring: ~341 ms
#define PS3_RING_SAMPLES    (PS3_RING_FRAMES * PS3_AUDIO_CHANNELS)

static audioPortConfig   port_config;
static u32               port_id;
static sys_event_queue_t port_queue;
static sys_ipc_key_t     port_key;
static sys_ppu_thread_t  feed_thread;
static volatile int      feed_quit;
static volatile int      audio_running;
static volatile u32      blocks_written;

static short ring[PS3_RING_SAMPLES] __attribute__((aligned(128)));

static volatile u64 audio_busy_us;

unsigned long long PS3_Audio_TakeBusyUs(void)
{
	u64 v = audio_busy_us;
	audio_busy_us = 0;
	return v;
}

static void PS3_AudioThread(void *arg)
{
	static float block[PS3_AUDIO_BLOCK * PS3_AUDIO_CHANNELS] __attribute__((aligned(16)));
	volatile u64 *read_index = (volatile u64 *)(u64)port_config.readIndex;
	f32 *data_start = (f32 *)(u64)port_config.audioDataStart;
	u64 num_blocks = port_config.numBlocks;
	sys_event_t event;
	int timeouts = 0;

	(void)arg;
	if (!read_index || !data_start || !num_blocks) {
		PS3_Log("snd_ps3: bad port configuration, no sound");
		sysThreadExit(1);
	}

	while (!feed_quit) {
		u32 write_block, pos, i;
		f32 *dst;

		if (sysEventQueueReceive(port_queue, &event, 20 * 1000) != 0) {
			if (++timeouts <= 3 || (timeouts % 500) == 0)
				PS3_Log("snd_ps3: port event timeout #%d", timeouts);
			continue;
		}
		if (feed_quit)
			break;

		u64 t0 = sysGetSystemTime();
		write_block = (u32)((*read_index + 1) % num_blocks);
		dst = data_start + write_block * PS3_AUDIO_CHANNELS * PS3_AUDIO_BLOCK;
		pos = (blocks_written * PS3_AUDIO_BLOCK * PS3_AUDIO_CHANNELS) % PS3_RING_SAMPLES;

		for (i = 0; i < PS3_AUDIO_BLOCK * PS3_AUDIO_CHANNELS; i++)
			block[i] = ring[pos + i] * (1.0f / 32768.0f);
		PS3_Music_Mix(block, PS3_AUDIO_BLOCK);
		for (i = 0; i < PS3_AUDIO_BLOCK * PS3_AUDIO_CHANNELS; i++) {
			float s = block[i];
			block[i] = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
		}
		// audioDataStart's alignment isn't guaranteed: plain copy.
		memcpy(dst, block, sizeof(block));
		blocks_written++;
		audio_busy_us += sysGetSystemTime() - t0;
	}
	sysThreadExit(0);
}

qboolean SNDDMA_Init(void)
{
	audioPortParam params;
	s32 ret;

	ret = sysModuleLoad(SYSMODULE_AUDIO);
	if (ret != 0 && ret != (s32)0x8001112E) {   // 0x8001112E: already loaded
		PS3_Log("snd_ps3: sysModuleLoad(AUDIO) = 0x%08x", (unsigned)ret);
		return false;
	}
	if (audioInit() != 0) {
		PS3_Log("snd_ps3: audioInit failed");
		return false;
	}

	memset(&params, 0, sizeof(params));
	params.numChannels = AUDIO_PORT_2CH;
	params.numBlocks   = AUDIO_BLOCK_8;
	params.attrib      = AUDIO_PORT_INITLEVEL;
	params.level       = 1.0f;
	if (audioPortOpen(&params, &port_id) != 0) {
		PS3_Log("snd_ps3: audioPortOpen failed");
		audioQuit();
		return false;
	}
	if (audioGetPortConfig(port_id, &port_config) != 0 ||
		audioCreateNotifyEventQueue(&port_queue, &port_key) != 0) {
		PS3_Log("snd_ps3: port config / notify queue failed");
		audioPortClose(port_id);
		audioQuit();
		return false;
	}
	if (audioSetNotifyEventQueue(port_key) != 0) {
		PS3_Log("snd_ps3: audioSetNotifyEventQueue failed");
		sysEventQueueDestroy(port_queue, 0);
		audioPortClose(port_id);
		audioQuit();
		return false;
	}
	sysEventQueueDrain(port_queue);
	if (audioPortStart(port_id) != 0) {
		PS3_Log("snd_ps3: audioPortStart failed");
		audioRemoveNotifyEventQueue(port_key);
		sysEventQueueDestroy(port_queue, 0);
		audioPortClose(port_id);
		audioQuit();
		return false;
	}

	memset(ring, 0, sizeof(ring));
	shm = &sn;
	shm->splitbuffer = 0;
	shm->samplebits = 16;
	shm->speed = PS3_AUDIO_RATE;
	shm->channels = PS3_AUDIO_CHANNELS;
	shm->samples = PS3_RING_SAMPLES;
	shm->samplepos = 0;
	shm->submission_chunk = 1;
	shm->buffer = (unsigned char *)ring;

	blocks_written = 0;
	feed_quit = 0;
	// Priority 100, well above the game thread's 1000 (0 = highest): each block has
	// a ~5.3 ms deadline, and a busy frame must not make it crackle.
	ret = sysThreadCreate(&feed_thread, PS3_AudioThread, NULL, 100, 0x4000,
		THREAD_JOINABLE, (char *)"nzp_audio");
	if (ret != 0) {
		PS3_Log("snd_ps3: sysThreadCreate = 0x%08x", (unsigned)ret);
		audioPortStop(port_id);
		audioRemoveNotifyEventQueue(port_key);
		sysEventQueueDestroy(port_queue, 0);
		audioPortClose(port_id);
		audioQuit();
		shm = NULL;
		return false;
	}
	audio_running = 1;
	PS3_Log("snd_ps3: %d Hz stereo, %d frame ring", PS3_AUDIO_RATE, PS3_RING_FRAMES);
	return true;
}

int SNDDMA_GetDMAPos(void)
{
	if (!audio_running)
		return 0;
	// in mono samples, as Quake counts them
	return (int)((blocks_written * PS3_AUDIO_BLOCK * PS3_AUDIO_CHANNELS) % PS3_RING_SAMPLES);
}

void SNDDMA_Submit(void) {}

void SNDDMA_Shutdown(void)
{
	u64 retval;

	if (!audio_running)
		return;
	audio_running = 0;
	feed_quit = 1;
	sysThreadJoin(feed_thread, &retval);
	audioPortStop(port_id);
	audioRemoveNotifyEventQueue(port_key);
	sysEventQueueDestroy(port_queue, 0);
	audioPortClose(port_id);
	audioQuit();
	PS3_Log("snd_ps3: shut down");
}
