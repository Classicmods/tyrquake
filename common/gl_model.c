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
// models.c -- model loading and caching

// models are the only shared resource between a client and server running
// on the same machine.

#include <float.h>
#include <stdint.h>

#include "console.h"
#include "crc.h"
#include "glquake.h"
#include "quakedef.h"
#include "sys.h"
#include "vid.h"

static model_t *loadmodel;
static char loadname[MAX_QPATH];	/* for hunk tags */

static void Mod_LoadBrushModel(model_t *mod, void *buffer, unsigned long size);
static void Mod_LoadAliasModel(model_t *mod, void *buffer);
static model_t *Mod_LoadModel(model_t *mod, qboolean crash);

static byte mod_novis[MAX_MAP_LEAFS / 8];

#define	MAX_MOD_KNOWN 512
static model_t mod_known[MAX_MOD_KNOWN];
static int mod_numknown;

cvar_t gl_subdivide_size = { "gl_subdivide_size", "128", true };

/*
===============
Mod_Init
===============
*/
void
Mod_Init(void)
{
    Cvar_RegisterVariable(&gl_subdivide_size);
    memset(mod_novis, 0xff, sizeof(mod_novis));
}

/*
===============
Mod_Extradata

Caches the data if needed
===============
*/
void *
Mod_Extradata(model_t *mod)
{
    void *r;

    r = Cache_Check(&mod->cache);
    if (r)
	return r;

    Mod_LoadModel(mod, true);

    if (!mod->cache.data)
	Sys_Error("%s: caching failed", __func__);
    return mod->cache.data;
}

/*
===============
Mod_PointInLeaf
===============
*/
mleaf_t *
Mod_PointInLeaf(vec3_t p, model_t *model)
{
    mnode_t *node;
    float d;
    mplane_t *plane;

    if (!model || !model->nodes)
	Sys_Error("%s: bad model", __func__);

    node = model->nodes;
    while (1) {
	if (node->contents < 0)
	    return (mleaf_t *)node;
	plane = node->plane;
	d = DotProduct(p, plane->normal) - plane->dist;
	if (d > 0)
	    node = node->children[0];
	else
	    node = node->children[1];
    }

    return NULL;		// never reached
}


/*
===================
Mod_DecompressVis
===================
*/
static byte *
Mod_DecompressVis(byte *in, model_t *model)
{
    static byte decompressed[MAX_MAP_LEAFS / 8];
    int c;
    byte *out;
    int row;

    row = (model->numleafs + 7) >> 3;
    out = decompressed;

    if (!in) {			// no vis info, so make all visible
	while (row) {
	    *out++ = 0xff;
	    row--;
	}
	return decompressed;
    }

    do {
	if (*in) {
	    *out++ = *in++;
	    continue;
	}

	c = in[1];
	in += 2;
	while (c) {
	    *out++ = 0;
	    c--;
	}
    } while (out - decompressed < row);

    return decompressed;
}

byte *
Mod_LeafPVS(mleaf_t *leaf, model_t *model)
{
    if (leaf == model->leafs)
	return mod_novis;
    return Mod_DecompressVis(leaf->compressed_vis, model);
}

/*
===================
Mod_ClearAll
===================
*/
void
Mod_ClearAll(void)
{
    int i;
    model_t *mod;

    for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++) {
	if (mod->type != mod_alias)
	    mod->needload = true;
	/*
	 * FIXME: sprites use the cache data pointer for their own purposes,
	 *        bypassing the Cache_Alloc/Free functions.
	 */
	if (mod->type == mod_sprite)
	    mod->cache.data = NULL;
    }
}

/*
==================
Mod_FindName

==================
*/
static model_t *
Mod_FindName(char *name)
{
    int i;
    model_t *mod;

    if (!name[0])
	Sys_Error("%s: NULL name", __func__);

//
// search the currently loaded models
//
    for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++)
	if (!strcmp(mod->name, name))
	    break;

    if (i == mod_numknown) {
	if (mod_numknown == MAX_MOD_KNOWN)
	    Sys_Error("mod_numknown == MAX_MOD_KNOWN");
	strncpy(mod->name, name, MAX_QPATH - 1);
	mod->name[MAX_QPATH - 1] = 0;
	mod->needload = true;
	mod_numknown++;
    }

    return mod;
}

/*
==================
Mod_TouchModel

==================
*/
void
Mod_TouchModel(char *name)
{
    model_t *mod;

    mod = Mod_FindName(name);

    if (!mod->needload) {
	if (mod->type == mod_alias)
	    Cache_Check(&mod->cache);
    }
}

/*
==================
Mod_LoadModel

Loads a model into the cache
==================
*/
static model_t *
Mod_LoadModel(model_t *mod, qboolean crash)
{
    unsigned *buf;
    byte stackbuf[1024];	// avoid dirtying the cache heap
    unsigned long size;

    if (!mod->needload) {
	if (mod->type == mod_alias) {
	    if (Cache_Check(&mod->cache))
		return mod;
	} else
	    return mod;		// not cached at all
    }

//
// because the world is so huge, load it one piece at a time
//

//
// load the file
//
    buf = (unsigned *)COM_LoadStackFile(mod->name, stackbuf, sizeof(stackbuf),
					&size);
    if (!buf) {
	if (crash)
	    Sys_Error("%s: %s not found", __func__, mod->name);
	return NULL;
    }
//
// allocate a new model
//
    COM_FileBase(mod->name, loadname);

    loadmodel = mod;

//
// fill it in
//

// call the apropriate loader
    mod->needload = false;

    switch (LittleLong(*(unsigned *)buf)) {
    case IDPOLYHEADER:
	Mod_LoadAliasModel(mod, buf);
	break;

    case IDSPRITEHEADER:
	Mod_LoadSpriteModel(mod, buf, loadname);
	break;

    default:
	Mod_LoadBrushModel(mod, buf, size);
	break;
    }

    return mod;
}

/*
==================
Mod_ForName

Loads in a model for the given name
==================
*/
model_t *
Mod_ForName(char *name, qboolean crash)
{
    model_t *mod;

    mod = Mod_FindName(name);

    return Mod_LoadModel(mod, crash);
}


/*
===============================================================================

					BRUSHMODEL LOADING

===============================================================================
*/

static byte *mod_base;


/*
=================
Mod_LoadTextures
=================
*/
static void
Mod_LoadTextures(lump_t *l)
{
    int i, j, pixels, num, max, altmax;
    miptex_t *mt;
    texture_t *tx, *tx2;
    texture_t *anims[10];
    texture_t *altanims[10];
    dmiptexlump_t *m;

    if (!l->filelen) {
	loadmodel->textures = NULL;
	return;
    }
    m = (dmiptexlump_t *)(mod_base + l->fileofs);

    m->nummiptex = LittleLong(m->nummiptex);

    loadmodel->numtextures = m->nummiptex;
    loadmodel->textures =
	Hunk_AllocName(m->nummiptex * sizeof(*loadmodel->textures), loadname);

    for (i = 0; i < m->nummiptex; i++) {
	m->dataofs[i] = LittleLong(m->dataofs[i]);
	if (m->dataofs[i] == -1)
	    continue;
	mt = (miptex_t *)((byte *)m + m->dataofs[i]);
	mt->width = (uint32_t)LittleLong(mt->width);
	mt->height = (uint32_t)LittleLong(mt->height);
	for (j = 0; j < MIPLEVELS; j++)
	    mt->offsets[j] = (uint32_t)LittleLong(mt->offsets[j]);

	if ((mt->width & 15) || (mt->height & 15))
	    Sys_Error("Texture %s is not 16 aligned", mt->name);
	pixels = mt->width * mt->height / 64 * 85;
	tx = Hunk_AllocName(sizeof(texture_t) + pixels, loadname);
	loadmodel->textures[i] = tx;

	memcpy(tx->name, mt->name, sizeof(tx->name));
	tx->width = mt->width;
	tx->height = mt->height;
	for (j = 0; j < MIPLEVELS; j++)
	    tx->offsets[j] =
		mt->offsets[j] + sizeof(texture_t) - sizeof(miptex_t);
	// the pixels immediately follow the structures
	memcpy(tx + 1, mt + 1, pixels);


	if (!strncmp(mt->name, "sky", 3))
	    R_InitSky(tx);
	else {
	    texture_mode = GL_LINEAR_MIPMAP_NEAREST;	//_LINEAR;
	    tx->gl_texturenum =
		GL_LoadTexture(mt->name, tx->width, tx->height,
			       (byte *)(tx + 1), true, false);
	    texture_mode = GL_LINEAR;
	}
    }

//
// sequence the animations
//
    for (i = 0; i < m->nummiptex; i++) {
	tx = loadmodel->textures[i];
	if (!tx || tx->name[0] != '+')
	    continue;
	if (tx->anim_next)
	    continue;		// allready sequenced

	// find the number of frames in the animation
	memset(anims, 0, sizeof(anims));
	memset(altanims, 0, sizeof(altanims));

	max = tx->name[1];
	if (max >= 'a' && max <= 'z')
	    max -= 'a' - 'A';
	if (max >= '0' && max <= '9') {
	    max -= '0';
	    altmax = 0;
	    anims[max] = tx;
	    max++;
	} else if (max >= 'A' && max <= 'J') {
	    altmax = max - 'A';
	    max = 0;
	    altanims[altmax] = tx;
	    altmax++;
	} else
	    Sys_Error("Bad animating texture %s", tx->name);

	for (j = i + 1; j < m->nummiptex; j++) {
	    tx2 = loadmodel->textures[j];
	    if (!tx2 || tx2->name[0] != '+')
		continue;
	    if (strcmp(tx2->name + 2, tx->name + 2))
		continue;

	    num = tx2->name[1];
	    if (num >= 'a' && num <= 'z')
		num -= 'a' - 'A';
	    if (num >= '0' && num <= '9') {
		num -= '0';
		anims[num] = tx2;
		if (num + 1 > max)
		    max = num + 1;
	    } else if (num >= 'A' && num <= 'J') {
		num = num - 'A';
		altanims[num] = tx2;
		if (num + 1 > altmax)
		    altmax = num + 1;
	    } else
		Sys_Error("Bad animating texture %s", tx->name);
	}

#define	ANIM_CYCLE	2
	// link them all together
	for (j = 0; j < max; j++) {
	    tx2 = anims[j];
	    if (!tx2)
		Sys_Error("Missing frame %i of %s", j, tx->name);
	    tx2->anim_total = max * ANIM_CYCLE;
	    tx2->anim_min = j * ANIM_CYCLE;
	    tx2->anim_max = (j + 1) * ANIM_CYCLE;
	    tx2->anim_next = anims[(j + 1) % max];
	    if (altmax)
		tx2->alternate_anims = altanims[0];
	}
	for (j = 0; j < altmax; j++) {
	    tx2 = altanims[j];
	    if (!tx2)
		Sys_Error("Missing frame %i of %s", j, tx->name);
	    tx2->anim_total = altmax * ANIM_CYCLE;
	    tx2->anim_min = j * ANIM_CYCLE;
	    tx2->anim_max = (j + 1) * ANIM_CYCLE;
	    tx2->anim_next = altanims[(j + 1) % altmax];
	    if (max)
		tx2->alternate_anims = anims[0];
	}
    }
}

/*
=================
Mod_LoadLighting
=================
*/
static void
Mod_LoadLighting(lump_t *l)
{
    if (!l->filelen) {
	loadmodel->lightdata = NULL;
	return;
    }
    loadmodel->lightdata = Hunk_AllocName(l->filelen, loadname);
    memcpy(loadmodel->lightdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVisibility
=================
*/
static void
Mod_LoadVisibility(lump_t *l)
{
    if (!l->filelen) {
	loadmodel->visdata = NULL;
	return;
    }
    loadmodel->visdata = Hunk_AllocName(l->filelen, loadname);
    memcpy(loadmodel->visdata, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadEntities
=================
*/
static void
Mod_LoadEntities(lump_t *l)
{
    if (!l->filelen) {
	loadmodel->entities = NULL;
	return;
    }
    loadmodel->entities = Hunk_AllocName(l->filelen, loadname);
    memcpy(loadmodel->entities, mod_base + l->fileofs, l->filelen);
}


/*
=================
Mod_LoadVertexes
=================
*/
static void
Mod_LoadVertexes(lump_t *l)
{
    dvertex_t *in;
    mvertex_t *out;
    int i, count;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->vertexes = out;
    loadmodel->numvertexes = count;

    for (i = 0; i < count; i++, in++, out++) {
	out->position[0] = LittleFloat(in->point[0]);
	out->position[1] = LittleFloat(in->point[1]);
	out->position[2] = LittleFloat(in->point[2]);
    }
}

/*
=================
Mod_LoadSubmodels
=================
*/
static void
Mod_LoadSubmodels(lump_t *l)
{
    dmodel_t *in;
    dmodel_t *out;
    int i, j, count;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->submodels = out;
    loadmodel->numsubmodels = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {	// spread the mins / maxs by a pixel
	    out->mins[j] = LittleFloat(in->mins[j]) - 1;
	    out->maxs[j] = LittleFloat(in->maxs[j]) + 1;
	    out->origin[j] = LittleFloat(in->origin[j]);
	}
	for (j = 0; j < MAX_MAP_HULLS; j++)
	    out->headnode[j] = LittleLong(in->headnode[j]);
	out->visleafs = LittleLong(in->visleafs);
	out->firstface = LittleLong(in->firstface);
	out->numfaces = LittleLong(in->numfaces);
    }
}

/*
=================
Mod_LoadEdges
=================
*/
static void
Mod_LoadEdges(lump_t *l)
{
    dedge_t *in;
    medge_t *out;
    int i, count;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName((count + 1) * sizeof(*out), loadname);

    loadmodel->edges = out;
    loadmodel->numedges = count;

    for (i = 0; i < count; i++, in++, out++) {
	out->v[0] = (uint16_t)LittleShort(in->v[0]);
	out->v[1] = (uint16_t)LittleShort(in->v[1]);
    }
}

/*
=================
Mod_LoadTexinfo
=================
*/
static void
Mod_LoadTexinfo(lump_t *l)
{
    texinfo_t *in;
    mtexinfo_t *out;
    int i, j, count;
    int miptex;
    float len1, len2;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->texinfo = out;
    loadmodel->numtexinfo = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 8; j++)
	    out->vecs[0][j] = LittleFloat(in->vecs[0][j]);
	len1 = Length(out->vecs[0]);
	len2 = Length(out->vecs[1]);
	len1 = (len1 + len2) / 2;
	if (len1 < 0.32)
	    out->mipadjust = 4;
	else if (len1 < 0.49)
	    out->mipadjust = 3;
	else if (len1 < 0.99)
	    out->mipadjust = 2;
	else
	    out->mipadjust = 1;
#if 0
	if (len1 + len2 < 0.001)
	    out->mipadjust = 1;	// don't crash
	else
	    out->mipadjust = 1 / floor((len1 + len2) / 2 + 0.1);
#endif

	miptex = LittleLong(in->miptex);
	out->flags = LittleLong(in->flags);

	if (!loadmodel->textures) {
	    out->texture = r_notexture_mip;	// checkerboard texture
	    out->flags = 0;
	} else {
	    if (miptex >= loadmodel->numtextures)
		Sys_Error("miptex >= loadmodel->numtextures");
	    out->texture = loadmodel->textures[miptex];
	    if (!out->texture) {
		out->texture = r_notexture_mip;	// texture not found
		out->flags = 0;
	    }
	}
    }
}

/*
================
CalcSurfaceExtents

Fills in s->texturemins[] and s->extents[]
================
*/
static void
CalcSurfaceExtents(msurface_t *s)
{
    float mins[2], maxs[2], val;
    int i, j, e;
    mvertex_t *v;
    mtexinfo_t *tex;
    int bmins[2], bmaxs[2];

    mins[0] = mins[1] = FLT_MAX;
    maxs[0] = maxs[1] = -FLT_MAX;

    tex = s->texinfo;

    for (i = 0; i < s->numedges; i++) {
	e = loadmodel->surfedges[s->firstedge + i];
	if (e >= 0)
	    v = &loadmodel->vertexes[loadmodel->edges[e].v[0]];
	else
	    v = &loadmodel->vertexes[loadmodel->edges[-e].v[1]];

	for (j = 0; j < 2; j++) {
	    val = v->position[0] * tex->vecs[j][0] +
		v->position[1] * tex->vecs[j][1] +
		v->position[2] * tex->vecs[j][2] + tex->vecs[j][3];
	    if (val < mins[j])
		mins[j] = val;
	    if (val > maxs[j])
		maxs[j] = val;
	}
    }

    for (i = 0; i < 2; i++) {
	bmins[i] = floor(mins[i] / 16);
	bmaxs[i] = ceil(maxs[i] / 16);

	s->texturemins[i] = bmins[i] * 16;
	s->extents[i] = (bmaxs[i] - bmins[i]) * 16;

	if (!(tex->flags & TEX_SPECIAL) && s->extents[i] > 256)
	    Sys_Error("Bad surface extents");
    }
}


/*
=================
Mod_LoadFaces
=================
*/
static void
Mod_LoadFaces(lump_t *l)
{
    dface_t *in;
    msurface_t *out;
    int i, count, surfnum;
    int planenum, side;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->surfaces = out;
    loadmodel->numsurfaces = count;

    for (surfnum = 0; surfnum < count; surfnum++, in++, out++) {
	out->firstedge = LittleLong(in->firstedge);
	out->numedges = LittleShort(in->numedges);
	out->flags = 0;

	planenum = LittleShort(in->planenum);
	side = LittleShort(in->side);
	if (side)
	    out->flags |= SURF_PLANEBACK;

	out->plane = loadmodel->planes + planenum;
	out->texinfo = loadmodel->texinfo + LittleShort(in->texinfo);

	CalcSurfaceExtents(out);

	// lighting info

	for (i = 0; i < MAXLIGHTMAPS; i++)
	    out->styles[i] = in->styles[i];
	i = LittleLong(in->lightofs);
	if (i == -1)
	    out->samples = NULL;
	else
	    out->samples = loadmodel->lightdata + i;

	/* set the surface drawing flags */
	if (!strncmp(out->texinfo->texture->name, "sky", 3)) {
	    out->flags |= (SURF_DRAWSKY | SURF_DRAWTILED);
	    GL_SubdivideSurface(loadmodel, out);
	} else if (!strncmp(out->texinfo->texture->name, "*", 1)) {
	    out->flags |= (SURF_DRAWTURB | SURF_DRAWTILED);
	    for (i = 0; i < 2; i++) {
		out->extents[i] = 16384;
		out->texturemins[i] = -8192;
	    }
	    GL_SubdivideSurface(loadmodel, out);
	}
    }
}


/*
=================
Mod_SetParent
=================
*/
static void
Mod_SetParent(mnode_t *node, mnode_t *parent)
{
    node->parent = parent;
    if (node->contents < 0)
	return;
    Mod_SetParent(node->children[0], node);
    Mod_SetParent(node->children[1], node);
}

/*
=================
Mod_LoadNodes
=================
*/
static void
Mod_LoadNodes(lump_t *l)
{
    int i, j, count, p;
    dnode_t *in;
    mnode_t *out;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->nodes = out;
    loadmodel->numnodes = count;

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {
	    out->minmaxs[j] = LittleShort(in->mins[j]);
	    out->minmaxs[3 + j] = LittleShort(in->maxs[j]);
	}

	p = LittleLong(in->planenum);
	out->plane = loadmodel->planes + p;

	out->firstsurface = (uint16_t)LittleShort(in->firstface);
	out->numsurfaces = (uint16_t)LittleShort(in->numfaces);

	for (j = 0; j < 2; j++) {
	    p = LittleShort(in->children[j]);
	    if (p >= 0)
		out->children[j] = loadmodel->nodes + p;
	    else
		out->children[j] = (mnode_t *)(loadmodel->leafs + (-1 - p));
	}
    }

    Mod_SetParent(loadmodel->nodes, NULL);	// sets nodes and leafs
}

/*
=================
Mod_LoadLeafs
=================
*/
static void
Mod_LoadLeafs(lump_t *l)
{
    dleaf_t *in;
    mleaf_t *out;
    int i, j, count, p;
    qboolean isnotmap = true;
#ifdef QW_HACK
    char s[80];			// FIXME - buffer overflows?
#endif

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);

    /* FIXME - fail gracefully */
    if (count > MAX_MAP_LEAFS)
	Sys_Error("%s: model->numleafs > MAX_MAP_LEAFS\n", __func__);

    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->leafs = out;
    loadmodel->numleafs = count;

#ifdef QW_HACK
    snprintf(s, sizeof(s), "maps/%s.bsp",
	     Info_ValueForKey(cl.serverinfo, "map"));
    if (!strcmp(s, loadmodel->name))
	isnotmap = false;
#endif

    for (i = 0; i < count; i++, in++, out++) {
	for (j = 0; j < 3; j++) {
	    out->minmaxs[j] = LittleShort(in->mins[j]);
	    out->minmaxs[3 + j] = LittleShort(in->maxs[j]);
	}

	p = LittleLong(in->contents);
	out->contents = p;
	out->firstmarksurface = loadmodel->marksurfaces +
	    (uint16_t)LittleShort(in->firstmarksurface);
	out->nummarksurfaces =
	    (uint16_t)LittleShort(in->nummarksurfaces);

	p = LittleLong(in->visofs);
	if (p == -1)
	    out->compressed_vis = NULL;
	else
	    out->compressed_vis = loadmodel->visdata + p;
	out->efrags = NULL;

	for (j = 0; j < 4; j++)
	    out->ambient_sound_level[j] = in->ambient_level[j];

	// gl underwater warp
	if (out->contents != CONTENTS_EMPTY) {
	    for (j = 0; j < out->nummarksurfaces; j++)
		out->firstmarksurface[j]->flags |= SURF_UNDERWATER;
	}

	// FIXME - no warping surfaces on non-map objects?
	if (isnotmap) {
	    for (j = 0; j < out->nummarksurfaces; j++)
		out->firstmarksurface[j]->flags |= SURF_DONTWARP;
	}
    }
}

/*
=================
Mod_LoadClipnodes
=================
*/
static void
Mod_LoadClipnodes(lump_t *l)
{
    dclipnode_t *in, *out;
    int i, count;
    hull_t *hull;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->clipnodes = out;
    loadmodel->numclipnodes = count;

    hull = &loadmodel->hulls[1];
    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = loadmodel->planes;
    hull->clip_mins[0] = -16;
    hull->clip_mins[1] = -16;
    hull->clip_mins[2] = -24;
    hull->clip_maxs[0] = 16;
    hull->clip_maxs[1] = 16;
    hull->clip_maxs[2] = 32;

    hull = &loadmodel->hulls[2];
    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = loadmodel->planes;
    hull->clip_mins[0] = -32;
    hull->clip_mins[1] = -32;
    hull->clip_mins[2] = -24;
    hull->clip_maxs[0] = 32;
    hull->clip_maxs[1] = 32;
    hull->clip_maxs[2] = 64;

    for (i = 0; i < count; i++, out++, in++) {
	out->planenum = LittleLong(in->planenum);
	out->children[0] = LittleShort(in->children[0]);
	out->children[1] = LittleShort(in->children[1]);
    }
}

/*
=================
Mod_MakeHull0

Duplicate the drawing hull structure as a clipping hull
=================
*/
static void
Mod_MakeHull0(void)
{
    mnode_t *in, *child;
    dclipnode_t *out;
    int i, j, count;
    hull_t *hull;

    hull = &loadmodel->hulls[0];

    in = loadmodel->nodes;
    count = loadmodel->numnodes;
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    hull->clipnodes = out;
    hull->firstclipnode = 0;
    hull->lastclipnode = count - 1;
    hull->planes = loadmodel->planes;

    for (i = 0; i < count; i++, out++, in++) {
	out->planenum = in->plane - loadmodel->planes;
	for (j = 0; j < 2; j++) {
	    child = in->children[j];
	    if (child->contents < 0)
		out->children[j] = child->contents;
	    else
		out->children[j] = child - loadmodel->nodes;
	}
    }
}

/*
=================
Mod_LoadMarksurfaces
=================
*/
static void
Mod_LoadMarksurfaces(lump_t *l)
{
    int i, j, count;
    unsigned short *in;
    msurface_t **out;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->marksurfaces = out;
    loadmodel->nummarksurfaces = count;

    for (i = 0; i < count; i++) {
	j = (uint16_t)LittleShort(in[i]);
	if (j >= loadmodel->numsurfaces)
	    Sys_Error("%s: bad surface number", __func__);
	out[i] = loadmodel->surfaces + j;
    }
}

/*
=================
Mod_LoadSurfedges
=================
*/
static void
Mod_LoadSurfedges(lump_t *l)
{
    int i, count;
    int *in, *out;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * sizeof(*out), loadname);

    loadmodel->surfedges = out;
    loadmodel->numsurfedges = count;

    for (i = 0; i < count; i++)
	out[i] = LittleLong(in[i]);
}


/*
=================
Mod_LoadPlanes
=================
*/
static void
Mod_LoadPlanes(lump_t *l)
{
    int i, j;
    mplane_t *out;
    dplane_t *in;
    int count;
    int bits;

    in = (void *)(mod_base + l->fileofs);
    if (l->filelen % sizeof(*in))
	Sys_Error("%s: funny lump size in %s", __func__, loadmodel->name);
    count = l->filelen / sizeof(*in);
    out = Hunk_AllocName(count * 2 * sizeof(*out), loadname);

    loadmodel->planes = out;
    loadmodel->numplanes = count;

    for (i = 0; i < count; i++, in++, out++) {
	bits = 0;
	for (j = 0; j < 3; j++) {
	    out->normal[j] = LittleFloat(in->normal[j]);
	    if (out->normal[j] < 0)
		bits |= 1 << j;
	}

	out->dist = LittleFloat(in->dist);
	out->type = LittleLong(in->type);
	out->signbits = bits;
    }
}

/*
=================
RadiusFromBounds
=================
*/
static float
RadiusFromBounds(vec3_t mins, vec3_t maxs)
{
    int i;
    vec3_t corner;

    for (i = 0; i < 3; i++) {
	corner[i] =
	    fabs(mins[i]) > fabs(maxs[i]) ? fabs(mins[i]) : fabs(maxs[i]);
    }

    return Length(corner);
}

/*
=================
Mod_LoadBrushModel
=================
*/
static void
Mod_LoadBrushModel(model_t *mod, void *buffer, unsigned long size)
{
    int i, j;
    dheader_t *header;
    dmodel_t *bm;

    loadmodel->type = mod_brush;
    header = (dheader_t *)buffer;

    /* swap all the header entries */
    header->version = LittleLong(header->version);
    for (i = 0; i < HEADER_LUMPS; ++i) {
	header->lumps[i].fileofs = LittleLong(header->lumps[i].fileofs);
	header->lumps[i].filelen = LittleLong(header->lumps[i].filelen);
    }

    if (header->version != BSPVERSION)
	Sys_Error("%s: %s has wrong version number (%i should be %i)",
		  __func__, mod->name, header->version, BSPVERSION);

    mod_base = (byte *)header;

    /*
     * Check the lump extents
     * FIXME - do this more generally... cleanly...?
     */
    for (i = 0; i < HEADER_LUMPS; ++i) {
	int b1 = header->lumps[i].fileofs;
	int e1 = b1 + header->lumps[i].filelen;

	/*
	 * Sanity checks
	 * - begin and end >= 0 (end might overflow).
	 * - end > begin (again, overflow reqd.)
	 * - end < size of file.
	 */
	if (b1 > e1 || e1 > size || b1 < 0 || e1 < 0)
	    Sys_Error("%s: bad lump extents in %s", __func__,
		      loadmodel->name);

	/* Now, check that it doesn't overlap any other lumps */
	for (j = 0; j < HEADER_LUMPS; ++j) {
	    int b2 = header->lumps[j].fileofs;
	    int e2 = b2 + header->lumps[j].filelen;

	    if ((b1 < b2 && e1 > b2) || (b2 < b1 && e2 > b1))
		Sys_Error("%s: overlapping lumps in %s", __func__,
			  loadmodel->name);
	}
    }

#ifdef QW_HACK
    /* checksum all of the map, except for entities - QW only */
    mod->checksum = 0;
    mod->checksum2 = 0;

    for (i = 0; i < HEADER_LUMPS; i++) {
	const lump_t *l = &header->lumps[i];
	unsigned int checksum;

	if (i == LUMP_ENTITIES)
	    continue;
	checksum = Com_BlockChecksum(mod_base + l->fileofs, l->filelen);
	mod->checksum ^= checksum;
	if (i == LUMP_VISIBILITY || i == LUMP_LEAFS || i == LUMP_NODES)
	    continue;
	mod->checksum2 ^= checksum;
    }
    mod->checksum = LittleLong(mod->checksum);
    mod->checksum2 = LittleLong(mod->checksum2);
#endif

    /* load into heap */
    Mod_LoadVertexes(&header->lumps[LUMP_VERTEXES]);
    Mod_LoadEdges(&header->lumps[LUMP_EDGES]);
    Mod_LoadSurfedges(&header->lumps[LUMP_SURFEDGES]);
    Mod_LoadTextures(&header->lumps[LUMP_TEXTURES]);
    Mod_LoadLighting(&header->lumps[LUMP_LIGHTING]);
    Mod_LoadPlanes(&header->lumps[LUMP_PLANES]);
    Mod_LoadTexinfo(&header->lumps[LUMP_TEXINFO]);
    Mod_LoadFaces(&header->lumps[LUMP_FACES]);
    Mod_LoadMarksurfaces(&header->lumps[LUMP_MARKSURFACES]);
    Mod_LoadVisibility(&header->lumps[LUMP_VISIBILITY]);
    Mod_LoadLeafs(&header->lumps[LUMP_LEAFS]);
    Mod_LoadNodes(&header->lumps[LUMP_NODES]);
    Mod_LoadClipnodes(&header->lumps[LUMP_CLIPNODES]);
    Mod_LoadEntities(&header->lumps[LUMP_ENTITIES]);
    Mod_LoadSubmodels(&header->lumps[LUMP_MODELS]);

    Mod_MakeHull0();

    mod->numframes = 2;		// regular and alternate animation

//
// set up the submodels (FIXME: this is confusing)
//
    for (i = 0; i < mod->numsubmodels; i++) {
	bm = &mod->submodels[i];

	mod->hulls[0].firstclipnode = bm->headnode[0];
	for (j = 1; j < MAX_MAP_HULLS; j++) {
	    mod->hulls[j].firstclipnode = bm->headnode[j];
	    mod->hulls[j].lastclipnode = mod->numclipnodes - 1;
	}

	mod->firstmodelsurface = bm->firstface;
	mod->nummodelsurfaces = bm->numfaces;

	VectorCopy(bm->maxs, mod->maxs);
	VectorCopy(bm->mins, mod->mins);

	mod->radius = RadiusFromBounds(mod->mins, mod->maxs);
	mod->numleafs = bm->visleafs;

	/* duplicate the basic information */
	if (i < mod->numsubmodels - 1) {
	    char name[10];

	    snprintf(name, sizeof(name), "*%i", i + 1);
	    loadmodel = Mod_FindName(name);
	    *loadmodel = *mod;
	    strcpy(loadmodel->name, name);
	    mod = loadmodel;
	}
    }
}

/*
==============================================================================

ALIAS MODELS

==============================================================================
*/

static aliashdr_t *pheader;

static stvert_t stverts[MAXALIASVERTS];
static mtriangle_t triangles[MAXALIASTRIS];

// a pose is a single set of vertexes.  a frame may be
// an animating sequence of poses
static const trivertx_t *poseverts[MAXALIASFRAMES];
static float poseintervals[MAXALIASFRAMES];
static int posenum;

#ifdef QW_HACK
byte player_8bit_texels[320 * 200];
#endif

/*
=================
Mod_LoadAliasFrame
=================
*/
static void
Mod_LoadAliasFrame(const daliasframe_t *in, maliasframedesc_t *frame)
{
    int i;

    strncpy(frame->name, in->name, sizeof(frame->name));
    frame->name[sizeof(frame->name) - 1] = 0;
    frame->firstpose = posenum;
    frame->numposes = 1;

    for (i = 0; i < 3; i++) {
	// these are byte values, so we don't have to worry about
	// endianness
	frame->bboxmin.v[i] = in->bboxmin.v[i];
	frame->bboxmax.v[i] = in->bboxmax.v[i];
    }

    poseverts[posenum] = in->verts;
    poseintervals[posenum] = 999.0f; /* unused, but make problems obvious */
    posenum++;
}


/*
=================
Mod_LoadAliasGroup

returns a pointer to the memory location following this frame group
=================
*/
static daliasframetype_t *
Mod_LoadAliasGroup(const daliasgroup_t *in, maliasframedesc_t *frame,
		   const char *loadname)
{
    int i, numframes;
    daliasframe_t *dframe;

    numframes = LittleLong(in->numframes);
    frame->firstpose = posenum;
    frame->numposes = numframes;

    for (i = 0; i < 3; i++) {
	// these are byte values, so we don't have to worry about endianness
	frame->bboxmin.v[i] = in->bboxmin.v[i];
	frame->bboxmax.v[i] = in->bboxmax.v[i];
    }

    dframe = (daliasframe_t *)&in->intervals[numframes];
    strncpy(frame->name, dframe->name, sizeof(frame->name));
    frame->name[sizeof(frame->name) - 1] = 0;
    for (i = 0; i < numframes; i++) {
	poseverts[posenum] = dframe->verts;
	poseintervals[posenum] = LittleFloat(in->intervals[i].interval);
	if (poseintervals[posenum] <= 0)
	    Sys_Error("%s: interval <= 0", __func__);
	posenum++;
	dframe = (daliasframe_t *)&dframe->verts[pheader->numverts];
    }

    return (daliasframetype_t *)dframe;
}

//=========================================================

/*
=================
Mod_FloodFillSkin

Fill background pixels so mipmapping doesn't have haloes - Ed
=================
*/

typedef struct {
    short x, y;
} floodfill_t;

// must be a power of 2
#define FLOODFILL_FIFO_SIZE 0x1000
#define FLOODFILL_FIFO_MASK (FLOODFILL_FIFO_SIZE - 1)

#define FLOODFILL_STEP( off, dx, dy ) \
{ \
	if (pos[off] == fillcolor) \
	{ \
		pos[off] = 255; \
		fifo[inpt].x = x + (dx), fifo[inpt].y = y + (dy); \
		inpt = (inpt + 1) & FLOODFILL_FIFO_MASK; \
	} \
	else if (pos[off] != 255) fdc = pos[off]; \
}

static void
Mod_FloodFillSkin(byte *skin, int skinwidth, int skinheight)
{
    byte fillcolor = *skin;	// assume this is the pixel to fill
    floodfill_t fifo[FLOODFILL_FIFO_SIZE];
    int inpt = 0, outpt = 0;
    int filledcolor = -1;
    int i;

    if (filledcolor == -1) {
	filledcolor = 0;
	// attempt to find opaque black
	for (i = 0; i < 256; ++i)
	    if (d_8to24table[i] == (255 << 0))	// alpha 1.0
	    {
		filledcolor = i;
		break;
	    }
    }
    // can't fill to filled color or to transparent color (used as visited marker)
    if ((fillcolor == filledcolor) || (fillcolor == 255)) {
	//printf( "not filling skin from %d to %d\n", fillcolor, filledcolor );
	return;
    }

    fifo[inpt].x = 0, fifo[inpt].y = 0;
    inpt = (inpt + 1) & FLOODFILL_FIFO_MASK;

    while (outpt != inpt) {
	int x = fifo[outpt].x, y = fifo[outpt].y;
	int fdc = filledcolor;
	byte *pos = &skin[x + skinwidth * y];

	outpt = (outpt + 1) & FLOODFILL_FIFO_MASK;

	if (x > 0)
	    FLOODFILL_STEP(-1, -1, 0);
	if (x < skinwidth - 1)
	    FLOODFILL_STEP(1, 1, 0);
	if (y > 0)
	    FLOODFILL_STEP(-skinwidth, 0, -1);
	if (y < skinheight - 1)
	    FLOODFILL_STEP(skinwidth, 0, 1);
	skin[x + skinwidth * y] = fdc;
    }
}

/*
===============
Mod_LoadAllSkins
===============
*/
static void *
Mod_LoadAllSkins(int numskins, daliasskintype_t *pskintype)
{
    int i, j, k;
    char name[32];
    int s;
    byte *skin;
#ifdef NQ_HACK
    byte *texels;
#endif
    daliasskingroup_t *pinskingroup;
    int groupskins;
    daliasskininterval_t *pinskinintervals;

    skin = (byte *)(pskintype + 1);

    if (numskins < 1 || numskins > MAX_SKINS)
	Sys_Error("%s: Invalid # of skins: %d", __func__, numskins);

    s = pheader->skinwidth * pheader->skinheight;

    for (i = 0; i < numskins; i++) {
	if (LittleLong(pskintype->type) == ALIAS_SKIN_SINGLE) {
	    Mod_FloodFillSkin(skin, pheader->skinwidth, pheader->skinheight);

#ifdef NQ_HACK
	    // save 8 bit texels for the player model to remap
	    //              if (!strcmp(loadmodel->name,"progs/player.mdl")) {
	    texels = Hunk_AllocName(s, loadname);
	    GL_Aliashdr(pheader)->texels[i] = texels - (byte *)pheader;
	    memcpy(texels, (byte *)(pskintype + 1), s);
	    //              }
#endif
#ifdef QW_HACK
	    // save 8 bit texels for the player model to remap
	    if (!strcmp(loadmodel->name, "progs/player.mdl")) {
		if (s > sizeof(player_8bit_texels))
		    Sys_Error("Player skin too large");
		memcpy(player_8bit_texels, (byte *)(pskintype + 1), s);
	    }
#endif
	    snprintf(name, sizeof(name), "%s_%i", loadmodel->name, i);
	    GL_Aliashdr(pheader)->gl_texturenum[i][0] =
		GL_Aliashdr(pheader)->gl_texturenum[i][1] =
		GL_Aliashdr(pheader)->gl_texturenum[i][2] =
		GL_Aliashdr(pheader)->gl_texturenum[i][3] =
		GL_LoadTexture(name, pheader->skinwidth,
			       pheader->skinheight, (byte *)(pskintype + 1),
			       true, false);
	    pskintype = (daliasskintype_t *)((byte *)(pskintype + 1) + s);
	} else {
	    // animating skin group.  yuck.
	    pskintype++;
	    pinskingroup = (daliasskingroup_t *)pskintype;
	    groupskins = LittleLong(pinskingroup->numskins);
	    pinskinintervals = (daliasskininterval_t *)(pinskingroup + 1);
	    pskintype = (daliasskintype_t *)(pinskinintervals + groupskins);

	    for (j = 0; j < groupskins; j++) {
		Mod_FloodFillSkin(skin, pheader->skinwidth,
				  pheader->skinheight);
#ifdef NQ_HACK
		if (j == 0) {
		    texels = Hunk_AllocName(s, loadname);
		    GL_Aliashdr(pheader)->texels[i] = texels - (byte *)pheader;
		    memcpy(texels, (byte *)(pskintype), s);
		}
#endif
		snprintf(name, sizeof(name), "%s_%i_%i", loadmodel->name, i, j);
		GL_Aliashdr(pheader)->gl_texturenum[i][j & 3] =
		    GL_LoadTexture(name, pheader->skinwidth,
				   pheader->skinheight, (byte *)(pskintype),
				   true, false);
		pskintype = (daliasskintype_t *)((byte *)(pskintype) + s);
	    }
	    k = j;
	    for ( /* */ ; j < 4; j++)
		GL_Aliashdr(pheader)->gl_texturenum[i][j & 3] =
		    GL_Aliashdr(pheader)->gl_texturenum[i][j - k];
	}
    }

    return pskintype;
}

//=========================================================================

/*
=================
Mod_LoadAliasModel
=================
*/
static void
Mod_LoadAliasModel(model_t *mod, void *buffer)
{
    byte *container;
    int i, j, pad;
    mdl_t *pinmodel;
    stvert_t *pinstverts;
    dtriangle_t *pintriangles;
    int version, numframes;
    int size;
    daliasframetype_t *pframetype;
    daliasframe_t *frame;
    daliasgroup_t *group;
    daliasskintype_t *pskintype;
    int start, end, total;
    float *intervals;

#ifdef QW_HACK
    /* Checksumming models - QW only... */
    if (!strcmp(loadmodel->name, "progs/player.mdl") ||
	!strcmp(loadmodel->name, "progs/eyes.mdl")) {
	unsigned short crc;
	byte *p;
	int len;
	char st[40];

	CRC_Init(&crc);
	for (len = com_filesize, p = buffer; len; len--, p++)
	    CRC_ProcessByte(&crc, *p);

	snprintf(st, sizeof(st), "%d", (int)crc);
	Info_SetValueForKey(cls.userinfo,
			    !strcmp(loadmodel->name, "progs/player.mdl") ?
			    pmodel_name : emodel_name, st, MAX_INFO_STRING);

	if (cls.state >= ca_connected) {
	    MSG_WriteByte(&cls.netchan.message, clc_stringcmd);
	    snprintf(st, sizeof(st), "setinfo %s %d",
		     !strcmp(loadmodel->name, "progs/player.mdl") ?
		     pmodel_name : emodel_name, (int)crc);
	    SZ_Print(&cls.netchan.message, st);
	}
    }
#endif

    start = Hunk_LowMark();

    pinmodel = (mdl_t *)buffer;

    version = LittleLong(pinmodel->version);
    if (version != ALIAS_VERSION)
	Sys_Error("%s has wrong version number (%i should be %i)",
		  mod->name, version, ALIAS_VERSION);

//
// allocate space for a working header, plus all the data except the frames,
// skin and group info
//
    pad = offsetof(gl_aliashdr_t, ahdr);
    size = pad + sizeof(aliashdr_t) +
	LittleLong(pinmodel->numframes) * sizeof(pheader->frames[0]);

    container = Hunk_AllocName(size, loadname);
    pheader = (aliashdr_t *)(container + pad);

    mod->flags = LittleLong(pinmodel->flags);

//
// endian-adjust and copy the data, starting with the alias model header
//
    pheader->numskins = LittleLong(pinmodel->numskins);
    pheader->skinwidth = LittleLong(pinmodel->skinwidth);
    pheader->skinheight = LittleLong(pinmodel->skinheight);

    if (pheader->skinheight > MAX_LBM_HEIGHT)
	Sys_Error("model %s has a skin taller than %d", mod->name,
		  MAX_LBM_HEIGHT);

    pheader->numverts = LittleLong(pinmodel->numverts);

    if (pheader->numverts <= 0)
	Sys_Error("model %s has no vertices", mod->name);

    if (pheader->numverts > MAXALIASVERTS)
	Sys_Error("model %s has too many vertices", mod->name);

    pheader->numtris = LittleLong(pinmodel->numtris);

    if (pheader->numtris <= 0)
	Sys_Error("model %s has no triangles", mod->name);

    pheader->numframes = LittleLong(pinmodel->numframes);
    numframes = pheader->numframes;
    if (numframes < 1)
	Sys_Error("%s: Invalid # of frames: %d", __func__, numframes);

    pheader->size = LittleFloat(pinmodel->size) * ALIAS_BASE_SIZE_RATIO;
    mod->synctype = LittleLong(pinmodel->synctype);
    mod->numframes = pheader->numframes;

    for (i = 0; i < 3; i++) {
	pheader->scale[i] = LittleFloat(pinmodel->scale[i]);
	pheader->scale_origin[i] = LittleFloat(pinmodel->scale_origin[i]);
    }

//
// load the skins
//
    pskintype = (daliasskintype_t *)&pinmodel[1];
    pskintype = Mod_LoadAllSkins(pheader->numskins, pskintype);

//
// load base s and t vertices
//
    pinstverts = (stvert_t *)pskintype;

    for (i = 0; i < pheader->numverts; i++) {
	stverts[i].onseam = LittleLong(pinstverts[i].onseam);
	stverts[i].s = LittleLong(pinstverts[i].s);
	stverts[i].t = LittleLong(pinstverts[i].t);
    }

//
// load triangle lists
//
    pintriangles = (dtriangle_t *)&pinstverts[pheader->numverts];

    for (i = 0; i < pheader->numtris; i++) {
	triangles[i].facesfront = LittleLong(pintriangles[i].facesfront);

	for (j = 0; j < 3; j++) {
	    triangles[i].vertindex[j] =
		LittleLong(pintriangles[i].vertindex[j]);
	}
    }

//
// load the frames
//
    posenum = 0;
    pframetype = (daliasframetype_t *)&pintriangles[pheader->numtris];

    for (i = 0; i < numframes; i++) {
	if (LittleLong(pframetype->type) == ALIAS_SINGLE) {
	    frame = (daliasframe_t *)(pframetype + 1);
	    Mod_LoadAliasFrame(frame, &pheader->frames[i]);
	    pframetype = (daliasframetype_t *)&frame->verts[pheader->numverts];
	} else {
	    group = (daliasgroup_t *)(pframetype + 1);
	    pframetype = Mod_LoadAliasGroup(group, &pheader->frames[i],
					    loadname);
	}
    }
    pheader->numposes = posenum;
    mod->type = mod_alias;

// FIXME: do this right
    mod->mins[0] = mod->mins[1] = mod->mins[2] = -16;
    mod->maxs[0] = mod->maxs[1] = mod->maxs[2] = 16;

    /*
     * Save the frame intervals
     */
    intervals = Hunk_Alloc(pheader->numposes * sizeof(float));
    pheader->poseintervals = (byte *)intervals - (byte *)pheader;
    for (i = 0; i < pheader->numposes; i++)
	intervals[i] = poseintervals[i];

    //
    // build the draw lists
    //
    GL_MakeAliasModelDisplayLists(mod, pheader, triangles, stverts, poseverts);

//
// move the complete, relocatable alias model to the cache
//
    end = Hunk_LowMark();
    total = end - start;

    Cache_AllocPadded(&mod->cache, pad, total - pad, loadname);
    if (!mod->cache.data)
	return;

    memcpy((byte *)mod->cache.data - pad, container, total);

    Hunk_FreeToLowMark(start);
}

//=============================================================================

/*
================
Mod_Print
================
*/
void
Mod_Print(void)
{
    int i;
    model_t *mod;

    Con_Printf("Cached models:\n");
    for (i = 0, mod = mod_known; i < mod_numknown; i++, mod++) {
	Con_Printf("%8p : %s\n", mod->cache.data, mod->name);
    }
}
