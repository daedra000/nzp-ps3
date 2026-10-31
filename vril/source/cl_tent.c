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
// cl_tent.c -- client side temporary entities

#include "nzportable_def.h"

static	vec3_t	playerbeam_end;		// added by joe

int			num_temp_entities;
entity_t	cl_temp_entities[MAX_TEMP_ENTITIES];
beam_t		cl_beams[MAX_BEAMS];

sfx_t			*cl_sfx_r_exp3;

extern sfx_t			*cl_sfx_step[4];
/*
=================
CL_ParseTEnt
=================
*/
void CL_InitTEnts (void)
{
	cl_sfx_r_exp3 = S_PrecacheSound ("sounds/weapons/r_exp3.wav");
	cl_sfx_step[0] = S_PrecacheSound ("sounds/player/footstep1.wav");
	cl_sfx_step[1] = S_PrecacheSound ("sounds/player/footstep2.wav");
	cl_sfx_step[2] = S_PrecacheSound ("sounds/player/footstep3.wav");
	cl_sfx_step[3] = S_PrecacheSound ("sounds/player/footstep4.wav");
}

/*
=================
CL_ClearTEnts
=================
*/
void CL_ClearTEnts (void)
{
	//cl_bolt1_mod = cl_bolt2_mod = cl_bolt3_mod = cl_beam_mod = NULL;

	memset (&cl_beams, 0, sizeof(cl_beams));
}

/*
=================
CL_ParseBeam
=================
*/
void CL_ParseBeam (model_t *m)
{
	int		ent;
	vec3_t	start, end;
	beam_t	*b;
	int		i;

	ent = MSG_ReadShort ();

	start[0] = MSG_ReadCoord ();
	start[1] = MSG_ReadCoord ();
	start[2] = MSG_ReadCoord ();

	end[0] = MSG_ReadCoord ();
	end[1] = MSG_ReadCoord ();
	end[2] = MSG_ReadCoord ();

    if (ent == cl.viewentity)
		VectorCopy(end, playerbeam_end);	// for cl_truelightning

// override any beam with the same entity
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
		if (b->entity == ent)
		{
			b->entity = ent;
			b->model = m;
			b->endtime = cl.time + 0.2;
			VectorCopy (start, b->start);
			VectorCopy (end, b->end);
			return;
		}

// find a free beam
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
	{
		if (!b->model || b->endtime < (float)cl.time)
		{
			b->entity = ent;
			b->model = m;
			b->endtime = cl.time + 0.2;
			VectorCopy (start, b->start);
			VectorCopy (end, b->end);
			return;
		}
	}
	Con_Printf ("beam list overflow!\n");
}

enum
{
	DECAL_NONE,
	DECAL_BULLTMRK,
	DECAL_BLOODMRK1,
	DECAL_BLOODMRK2,
	DECAL_BLOODMRK3,
	DECAL_EXPLOMRK
};
//==============================================================================
extern cvar_t	r_decal_bullets;
extern cvar_t   r_decal_explosions;

extern int decal_blood1, decal_blood2, decal_blood3, decal_burn, decal_mark, decal_glow;
//==============================================================================
/*
=================
CL_ParseTEnt
=================
*/
void CL_ParseTEnt (void)
{
	int		type;
	vec3_t	pos, impact_dir;
	dlight_t	*dl;
	//int		rnd;
	int		colorStart, colorLength;

	type = MSG_ReadByte ();
	switch (type)
	{
	case TE_WIZSPIKE:			// spike hitting wall
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		R_RunParticleEffect (pos, vec3_origin, 20, 30);
		//S_StartSound (-1, 0, cl_sfx_wizhit, pos, 1, 1);
		break;

	case TE_KNIGHTSPIKE:			// spike hitting wall
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		R_RunParticleEffect (pos, vec3_origin, 226, 20);
		//S_StartSound (-1, 0, cl_sfx_knighthit, pos, 1, 1);
		break;

	case TE_SPIKE:			// spike hitting wall
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		//R00k--start
		if (r_decal_bullets.value)
		{
			R_SpawnDecalStatic(pos, decal_mark, 8);
		}
		//R00k--end

		R_RunParticleEffect (pos, vec3_origin, 0, 10);

		break;
	case TE_SUPERSPIKE:			// super spike hitting wall
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		R_RunParticleEffect (pos, vec3_origin, 0, 20);

		//R00k--start
		if (r_decal_bullets.value)
		{
			R_SpawnDecalStatic(pos, decal_mark, 10);
		}
		//R00k--end

		break;

	case TE_GUNSHOT:			// bullet hitting wall
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();

		//R00k--start
		if (r_decal_bullets.value)
		{
			R_SpawnDecalStatic(pos, decal_mark, 8);
		}
		//R00k--end
		R_RunParticleEffect (pos, vec3_origin, 0, 20);
		break;

	case TE_EXPLOSION:			// rocket explosion
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		R_ParticleExplosion (pos);
        if (r_decal_explosions.value)
		{
			R_SpawnDecalStatic(pos, decal_burn, 100);
		}
		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 350;
		dl->die = cl.time + 0.5;
		dl->decay = 300;
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 0.5);
		break;
	case TE_RAYSPLASHGREEN:
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 65;
		dl->die = cl.time + 0.3;
		dl->decay = 300;
		dl->color[0] = 0.24f;
		dl->color[1] = 2.0f;
		dl->color[2] = 0.48f;
		VectorSubtract(r_refdef.vieworg, pos, impact_dir);
		R_RunParticleEffect (pos, impact_dir, 0, 256);
		//S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 0.5); // NZPFIXME - add raygun hum
		break;
	case TE_RAYSPLASHRED:
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 65;
		dl->die = cl.time + 0.3;
		dl->decay = 300;
		dl->color[0] = 2.0f;
		dl->color[1] = 0.20f;
		dl->color[2] = 0.62f;
		VectorSubtract(r_refdef.vieworg, pos, impact_dir);
		R_RunParticleEffect (pos, impact_dir, 0, 512);
		//S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 0.5); // NZPFIXME - add raygun hum
		break;
	case TE_TAREXPLOSION:			// tarbaby explosion
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		R_BlobExplosion (pos);
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;

	case TE_LIGHTNING1:				// lightning bolts
		CL_ParseBeam (Mod_ForName("models/misc/bolt.mdl", true));
		break;

	case TE_LIGHTNING2:				// lightning bolts
		CL_ParseBeam (Mod_ForName("models/misc/bolt2.mdl", true));
		break;

	case TE_LIGHTNING3:				// lightning bolts
		CL_ParseBeam (Mod_ForName("progs/bolt3.mdl", true));
		break;

// PGM 01/21/97
	case TE_BEAM:				// grappling hook beam
		CL_ParseBeam (Mod_ForName("progs/beam.mdl", true));
		break;
// PGM 01/21/97

	case TE_LAVASPLASH:
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		R_LavaSplash (pos);
        dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 150;
		dl->die = cl.time + 0.75;
		dl->decay = 200;
		break;

	case TE_TELEPORT:
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		R_TeleportSplash (pos);
		break;

	case TE_EXPLOSION2:				// color mapped explosion
		pos[0] = MSG_ReadCoord ();
		pos[1] = MSG_ReadCoord ();
		pos[2] = MSG_ReadCoord ();
		colorStart = MSG_ReadByte ();
		colorLength = MSG_ReadByte ();
		R_ParticleExplosion2 (pos, colorStart, colorLength);

		if (r_decal_explosions.value)
		{
			R_SpawnDecalStatic(pos, decal_burn, 100);
		}

		dl = CL_AllocDlight (0);
		VectorCopy (pos, dl->origin);
		dl->radius = 350;
		dl->die = cl.time + 0.5;
		dl->decay = 300;
		S_StartSound (-1, 0, cl_sfx_r_exp3, pos, 1, 1);
		break;

	default:
		Sys_Error ("bad type");
	}
}


/*
=================
CL_NewTempEntity
=================
*/
entity_t *CL_NewTempEntity (void)
{
	entity_t	*ent;

	if (cl_numvisedicts == MAX_VISEDICTS)
		return NULL;
	if (num_temp_entities == MAX_TEMP_ENTITIES)
		return NULL;
	ent = &cl_temp_entities[num_temp_entities];
	memset (ent, 0, sizeof(*ent));
	num_temp_entities++;
	cl_visedicts[cl_numvisedicts] = ent;
	cl_numvisedicts++;

	ent->colormap = vid.colormap;
	return ent;
}

/*
=================
TraceLineN
=================
*/
qboolean TraceLineN (vec3_t start, vec3_t end, vec3_t impact, vec3_t normal)
{
	trace_t	trace;

	memset (&trace, 0, sizeof(trace));
	if (!SV_RecursiveHullCheck (cl.worldmodel->hulls, 0, start, end, &trace))
	{
		if (trace.fraction < 1)
		{
			VectorCopy (trace.endpos, impact);
			if (normal)
				VectorCopy (trace.plane.normal, normal);

			return true;
		}
	}

	return false;
}

void QMB_Lightning_Splash (vec3_t org);
extern cvar_t scr_ofsy;
/*
=================
CL_UpdateTEnts
=================
*/
void CL_UpdateTEnts (void)
{
	int			i;
	beam_t		*b;
	vec3_t		dist, org, beamstart;
	float		d;
	entity_t	*ent;
	float		yaw, pitch;
	float		forward;

	int			j;
	vec3_t		beamend;
//	qboolean	sparks = false;

	num_temp_entities = 0;

// update lightning
	for (i=0, b=cl_beams ; i< MAX_BEAMS ; i++, b++)
	{
		if (!b->model || b->endtime < (float)cl.time)
			continue;

		// if coming from the player, update the start position
		if (b->entity == cl.viewentity)
		{
			VectorCopy (cl_entities[cl.viewentity].origin, b->start);
#ifdef __PSP__
			b->start[2] += cl.crouch + bound(-7, scr_ofsy.value, 4);
			b->start[2] += bound(0, cl_lightning_zadjust.value, 20);//progs.dat aims from 20 for traceline

			if (cl_truelightning.value)
			{
				vec3_t	forward, v, org, ang;
				float	f, delta;
				trace_t	trace;

				f = fmax(0, fmin(1, cl_truelightning.value));

				VectorSubtract (playerbeam_end, cl_entities[cl.viewentity].origin, v);
				//v[2] -= 22;		// adjust for view height
				v[2] -= cl.crouch; //
				v[2] -= bound(0, cl_lightning_zadjust.value, 20);

				vectoangles (v, ang);

				// lerp pitch
				ang[0] = -ang[0];
				if (ang[0] < -180)
					ang[0] += 360;
				ang[0] += (cl.viewangles[0] - ang[0]) * f;

				// lerp yaw
				delta = cl.viewangles[1] - ang[1];
				if (delta > 180)
					delta -= 360;
				if (delta < -180)
					delta += 360;
				ang[1] += delta * f;
				ang[2] = 0;

				AngleVectors (ang, forward, NULLVEC, NULLVEC);
				VectorScale(forward, 600, forward);
				VectorCopy(cl_entities[cl.viewentity].origin, org);
				org[2] += bound(0, cl_lightning_zadjust.value, 20);//progs.dat aims from 20 for teaceline
				VectorAdd(org, forward, b->end);

				memset (&trace, 0, sizeof(trace_t));
				if (!SV_RecursiveHullCheck(cl.worldmodel->hulls, 0, org, b->end, &trace))
					VectorCopy(trace.endpos, b->end);
			}
#endif // __PSP__

		}
	// calculate pitch and yaw
		VectorSubtract (b->end, b->start, dist);

		if (dist[1] == 0 && dist[0] == 0)
		{
			yaw = 0;
			if (dist[2] > 0)
				pitch = 90;
			else
				pitch = 270;
		}
		else
		{
			yaw = (int) (atan2f(dist[1], dist[0]) * (180 / (float)M_PI));
			if (yaw < 0)
				yaw += 360;

			forward = sqrtf (dist[0]*dist[0] + dist[1]*dist[1]);
			pitch = (int) (atan2f(dist[2], forward) * (180 / (float)M_PI));
			if (pitch < 0)
				pitch += 360;
		}
        // add new entities for the lightning
		VectorCopy(b->start, org);
		VectorCopy(b->start, beamstart);
		d = VectorNormalize (dist);
		VectorScale (dist, 30, dist);

		if (key_dest == key_game)
		{
			for ( ; d > 0 ; d -= 30)
			{
				if ((qmb_initialized && r_part_lightning.value) && (!cl.paused))
				{
					VectorAdd(org, dist, beamend);
					for (j=0 ; j<3 ; j++)
						beamend[j] += ((rand()%10)-5);
					QMB_LightningBeam (beamstart, beamend);
					//if ((r_glowlg.value) && (r_dynamic.value))
					//	CL_NewDlight (i, beamstart, 100, 0.1, lt_blue);
					VectorCopy(beamend, beamstart);
				}
				else
				{
					if (!(ent = CL_NewTempEntity()))
						return;
					VectorCopy(org, ent->origin);
					ent->model = b->model;
					ent->angles[0] = pitch;
					ent->angles[1] = yaw;
					ent->angles[2] = rand() % 360;
				}
				VectorAdd(org, dist, org);
			}
		}
/*
	// add new entities for the lightning
		VectorCopy (b->start, org);
		d = VectorNormalize(dist);
     	while (d > 0)
		{
			ent = CL_NewTempEntity ();
			if (!ent)
				return;
			VectorCopy (org, ent->origin);
			ent->model = b->model;
			ent->angles[0] = pitch;
			ent->angles[1] = yaw;
			ent->angles[2] = rand()%360;

			for (i=0 ; i<3 ; i++)
				org[i] += dist[i]*30;
			d -= 30;
		}
*/
	}

}

/*
=================
Other players' guns (split screen; the QC never gives players a world weapon).

Each player another seat can see gets the world model of the gun they hold
(g_<name>.mdl next to their v_<name>.mdl, as on wall buys and in the box) in
front of them, at their aim. player.mdl has no hand to attach it to, so it's a
fixed place: forward, to the right, below the eyes.
=================
*/
static struct model_s *CL_WorldWeaponModel (const char *vmodel)
{
	char want[MAX_QPATH], dir[MAX_QPATH];
	const char *slash;
	int i, dlen;

	if (!vmodel || !*vmodel)
		return NULL;
	slash = strrchr (vmodel, '/');
	if (!slash || strncmp (slash + 1, "v_", 2))
		return NULL;
	dlen = slash + 1 - vmodel;
	if (dlen >= MAX_QPATH - 4)
		return NULL;
	memcpy (dir, vmodel, dlen);
	dir[dlen] = 0;
	snprintf (want, sizeof(want), "%sg_%s", dir, slash + 3);

	// g_<same name>, or else the only kind of g_ model in that folder (v_karscope -> g_kar...)
	for (i = 1; i < MAX_MODELS && cl.model_precache[i]; i++)
		if (!strcmp (cl.model_precache[i]->name, want))
			return cl.model_precache[i];
	for (i = 1; i < MAX_MODELS && cl.model_precache[i]; i++)
		if (!strncmp (cl.model_precache[i]->name, dir, dlen) && !strncmp (cl.model_precache[i]->name + dlen, "g_", 2))
			return cl.model_precache[i];
	return NULL;
}

void CL_AddPlayerWeapons (void)
{
	int i, n = cl_numvisedicts;

	if (!sv.active || cl.maxclients < 2)
		return;		// the guns are read from the local server's players

	for (i = 0; i < n; i++)
	{
		entity_t	*pl = cl_visedicts[i], *gun;
		int			num = pl - cl_entities;
		edict_t		*ed;
		struct model_s *m;
		vec3_t		aim, fwd, right, up;
		float		height;

		if (num < 1 || num > cl.maxclients || num == cl.viewentity || !pl->model)
			continue;
		ed = EDICT_NUM(num);
		if (ed->free || !ed->v.weaponmodel || ed->v.health <= 0)
			continue;
		{
			// put away while sprinting (the QC's field)
			eval_t *v = GETEDICTFIELDVALUE(ed, eval_sprinting);
			if (v && v->_float)
				continue;
		}
		if (!(m = CL_WorldWeaponModel (PR_GetString (ed->v.weaponmodel))))
			continue;
		if (!(gun = CL_NewTempEntity ()))
			return;

		aim[0] = ed->v.v_angle[0];
		if (aim[0] > 60) aim[0] = 60;
		if (aim[0] < -60) aim[0] = -60;
		aim[1] = pl->angles[1];
		aim[2] = 0;
		AngleVectors (aim, fwd, right, up);

		// a hand's height: below the eyes, which drop when crouched or prone
		// (first look on the console: low, near the belly; the hand is higher and
		// further out to the right)
		// (second look: at the shoulder; third: in the belly, under the arms.
		// player.mdl's hands reach ~27 forward, ~6 right of its middle, so the gun
		// goes out at the hands, between those two heights)
		height = ed->v.view_ofs[2] - 12;
		VectorMA (pl->origin, 24, fwd, gun->origin);
		VectorMA (gun->origin, 5, right, gun->origin);
		gun->origin[2] += height;

		gun->angles[0] = -aim[0];	// models pitch the other way to views
		// the g_ models point along +x like any Quake model (g_colt: grip at -x, muzzle
		// at +x), so no turn; the +90 here had it pointing out of the player's left side
		gun->angles[1] = aim[1];
		gun->angles[2] = 0;
		gun->model = m;
		gun->frame = 0;
		gun->skinnum = 0;
		gun->scale = ENTSCALE_DEFAULT;
	}
}
