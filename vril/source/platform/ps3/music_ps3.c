/*
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
// music_ps3.c -- NZ:P's MP3 tracks (snd_music.c) for the PS3.
//
// A track (at most ~3 MB) is read whole into memory when it starts, then decoded a
// frame at a time by minimp3 on the audio thread, resampled to the port's 48 kHz and
// added into the block it is about to hand to the hardware (PS3_Music_Mix, called
// from snd_ps3.c). The audio thread only ever try-locks: if the game thread is
// swapping tracks, that block simply has no music.
//
// Looping is snd_music.c's job: when a track ends, music_job_started goes to 0 and it
// starts the track again.

#include "../../nzportable_def.h"
#include "ps3_platform.h"
#include "ps3_local.h"

#include <unistd.h>


#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "thirdparty/minimp3.h"

#define OUT_RATE 48000

volatile int music_job_started;
int music_volume;
qboolean music_paused;

// A plain atomic flag, not an lv2 lightweight mutex: with one, the game thread's
// sysLwMutexLock in music_stop never returned (the loading screen froze the
// console). The audio thread only ever tries it; the game thread waits briefly.
static volatile int music_lock;

static int Music_TryLock(void)
{
	if (!__sync_bool_compare_and_swap(&music_lock, 0, 1))
		return 0;
	__asm__ volatile("isync" ::: "memory");
	return 1;
}

static void Music_Lock(void)
{
	int waited = 0;
	while (!Music_TryLock()) {
		usleep(100);
		if (++waited == 20000) {   // ~2 s: the audio thread never holds it this long
			PS3_Log("music_ps3: WARNING: lock held for 2 s, taking it");
			music_lock = 0;
		}
	}
}

static void Music_Unlock(void)
{
	__asm__ volatile("lwsync" ::: "memory");
	music_lock = 0;
}
static qboolean music_initialized;

// The playing track: owned by whoever holds music_lock.
static struct {
	byte   *file;           // the whole MP3
	int     file_size;
	int     file_pos;
	mp3dec_t dec;
	short   pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
	int     pcm_frames;     // decoded frames in pcm[]
	int     pcm_channels;
	double  pcm_pos;        // read position in pcm[], in input frames
	int     in_rate;
	float   prev[2];        // the frame just before pcm[0], for interpolation
	qboolean done;
} track;

static void Track_Free(void)
{
	free(track.file);
	memset(&track, 0, sizeof(track));
}

// Decodes MP3 frames until there is one with samples. false at the end of the file.
static qboolean Track_Decode(void)
{
	mp3dec_frame_info_t info;

	while (track.file_pos < track.file_size) {
		int frames = mp3dec_decode_frame(&track.dec, track.file + track.file_pos,
			track.file_size - track.file_pos, track.pcm, &info);
		if (info.frame_bytes <= 0)
			break;   // nothing more it can make sense of
		track.file_pos += info.frame_bytes;
		if (frames > 0) {
			track.pcm_frames = frames;
			track.pcm_channels = info.channels;
			track.in_rate = info.hz;
			return true;
		}
	}
	return false;
}

void PS3_Music_Mix(float *out, int frames)
{
	float gain;
	double step;
	int i;

	if (!music_initialized || !Music_TryLock())
		return;
	if (!track.file || track.done || music_paused || music_volume <= 0) {
		Music_Unlock();
		return;
	}

	gain = (float)music_volume / PLATFORM_VOLUME_MAX;
	step = track.in_rate > 0 ? (double)track.in_rate / OUT_RATE : 1.0;

	for (i = 0; i < frames; i++) {
		int idx;
		float frac, a[2], b[2];
		int ch;

		// Move to the frame holding the next output sample.
		while (track.pcm_pos >= track.pcm_frames) {
			int last = track.pcm_frames - 1;
			if (track.pcm_frames > 0) {
				track.prev[0] = track.pcm[last * track.pcm_channels] * (1.0f / 32768.0f);
				track.prev[1] = track.pcm[last * track.pcm_channels + track.pcm_channels - 1] * (1.0f / 32768.0f);
			}
			track.pcm_pos -= track.pcm_frames;
			if (!Track_Decode()) {
				track.done = true;
				music_job_started = 0;   // snd_music.c restarts it if it loops
				Music_Unlock();
				return;
			}
		}

		idx = (int)track.pcm_pos;
		frac = (float)(track.pcm_pos - idx);
		for (ch = 0; ch < 2; ch++) {
			int c = track.pcm_channels > 1 ? ch : 0;
			b[ch] = track.pcm[idx * track.pcm_channels + c] * (1.0f / 32768.0f);
			a[ch] = idx > 0 ? track.pcm[(idx - 1) * track.pcm_channels + c] * (1.0f / 32768.0f)
			                : track.prev[ch];
		}
		// linear interpolation between the sample before and this one
		out[i * 2]     += (a[0] + (b[0] - a[0]) * frac) * gain;
		out[i * 2 + 1] += (a[1] + (b[1] - a[1]) * frac) * gain;
		track.pcm_pos += step;
	}
	Music_Unlock();
}

int music_init(void)
{
	music_lock = 0;
	music_initialized = true;
	return 1;
}

void music_deinit(void)
{
	if (!music_initialized)
		return;
	music_stop();
	music_initialized = false;
}

// 1 playing, 2 file not found, 0 other failure (as music_sdl.c).
int music_start_play(char *fname, int pos)
{
	FILE *f;
	byte *data;
	long size;

	(void)pos;
	if (!music_initialized)
		return 0;
	{
		char stage[96];
		snprintf(stage, sizeof(stage), "music start %s", fname);
		PS3_Stage(stage);
	}
	music_stop();

	f = fopen(fname, "rb");
	if (!f)
		return 2;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = size > 0 ? malloc(size) : NULL;
	if (!data || fread(data, 1, size, f) != (size_t)size) {
		fclose(f);
		free(data);
		Con_Printf("music: could not read %s\n", fname);
		return 0;
	}
	fclose(f);

	Music_Lock();
	memset(&track, 0, sizeof(track));
	track.file = data;
	track.file_size = (int)size;
	mp3dec_init(&track.dec);
	if (!Track_Decode()) {
		Track_Free();
		Music_Unlock();
		Con_Printf("music: %s is not an MP3 minimp3 can play\n", fname);
		return 0;
	}
	music_job_started = 1;
	music_paused = false;
	Music_Unlock();
	return 1;
}

void music_pause(void) { music_paused = true; }
void music_resume(void) { music_paused = false; }

void music_stop(void)
{
	if (!music_initialized)
		return;
	PS3_Stage("music stop");
	Music_Lock();
	Track_Free();
	music_job_started = 0;
	music_paused = false;
	Music_Unlock();
}

void music_update(void) {}
