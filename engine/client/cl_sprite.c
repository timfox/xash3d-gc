/*
cl_sprite.c - sprite loading
Copyright (C) 2010 Uncle Mike

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "common.h"
#include "client.h"
#include "sprite.h"
#include "ref_common.h"

#if XASH_GAMECUBE
#include "platform/platform.h"
#include "platform/gamecube/mem_gamecube.h"
#ifndef XASH_GC_MOHAA
#define XASH_GC_MOHAA 0
#endif
/* Client-side sprite loading cannot access the renderer's GC_GetVisualQuality()
 * because r_local.h is a renderer-local header. Use a compile-time consistent
 * low-memory guard instead. Runtime quality adjustments for sprites are handled
 * in the renderer (ref/gx/r_sprite.c). */
#endif

static char  sprite_name[MAX_QPATH];
static char  group_suffix[8];
static uint  r_texFlags = 0;
static int   sprite_version;
#if XASH_GAMECUBE
static qboolean sprite_masked;	/* SPR_ALPHTEST: index 255 is the hole */
static const byte *sprite_pal;	/* the sprite's palette as stored (RGB) */
static int sprite_pal_count;
static int sprite_tex_format;
/* Set by cl_tent.c around effect sprite loads (muzzle flashes, explosions,
 * smoke, blood). Their frames are drawn small and briefly, but at full size
 * ten of them took ~70 KiB of MEM1 and starved the crosshair and viewmodel
 * skins (38 texture OOMs in a charlie match). */
qboolean gc_sprite_effect_load;
static int sprite_fx_cap;
/* Frames of the sprite being loaded were already shrunk by this factor when
 * read from disc (CL_GCLoadEffectSpriteShrunk, cl_game.c): they are drawn at
 * their source size. */
int gc_sprite_fx_prescale = 1;
int R_GXHudHiRegister( const char *texname, int w, int h, const byte *indices, const byte *rgba_palette );
int R_GXHudHiRegisterRGBA( const char *texname, int w, int h, const byte *rgba_pixels );
/* Half-Life's 640 HUD keeps every sheet in ARAM with a token resident copy;
 * Day of Defeat keeps its own sizes (64 px fallback copies). */
#if XASH_GC_MOHAA || !( defined( XASH_DODGC_NETWORK ) && XASH_DODGC_NETWORK )
#define GC_HUD_SHEETS_IN_ARAM 1
#else
#define GC_HUD_SHEETS_IN_ARAM 0
#endif
void R_GXHudHiSetTexnum( int handle, int texnum );

/* The palette the image loader would install for this sprite (see
 * Image_SetPalette), as RGBA, for the renderer's full-resolution HUD cells. */
static qboolean Mod_SpriteHudPalette( byte *rgba )
{
	int i;

	if( !sprite_pal || sprite_pal_count < 1 || sprite_pal_count > 256 )
		return false;
	memset( rgba, 0, 256 * 4 );
	for( i = 0; i < sprite_pal_count; i++ )
	{
		const byte *c = sprite_pal + i * 3;
		byte *d = rgba + i * 4;

		switch( sprite_tex_format )
		{
		case SPR_INDEXALPHA:
		{
			const byte *last = sprite_pal + ( sprite_pal_count - 1 ) * 3;

			d[0] = last[0];
			d[1] = last[1];
			d[2] = last[2];
			d[3] = (byte)i;
			break;
		}
		case SPR_ALPHTEST:
			d[0] = c[0];
			d[1] = c[1];
			d[2] = c[2];
			d[3] = i == 255 ? 0 : 255;
			break;
		default:
			d[0] = TextureToGamma( c[0] );
			d[1] = TextureToGamma( c[1] );
			d[2] = TextureToGamma( c[2] );
			d[3] = 255;
			break;
		}
	}
	if( sprite_tex_format == SPR_INDEXALPHA )
	{
		for( ; i < 256; i++ )
		{
			memcpy( rgba + i * 4, rgba + ( sprite_pal_count - 1 ) * 4, 3 );
			rgba[i * 4 + 3] = (byte)i;
		}
	}
	return true;
}
#endif

/*
================
R_GetSpriteFrame

assume pModel is valid
================
*/
mspriteframe_t *R_GetSpriteFrame( const model_t *pModel, int frame, float yaw )
{
	msprite_t      *psprite;
	mspriteframe_t *pspriteframe = NULL;

	if( !pModel )
		return NULL;

	psprite = pModel->cache.data;
	if( !psprite )
		return NULL;

	if( frame < 0 )
	{
		frame = 0;
	}
	else if( frame >= psprite->numframes )
	{
		if( frame > psprite->numframes )
			Con_Printf( S_WARN "%s: no such frame %d (%s)\n", __func__, frame, pModel->name );
		frame = psprite->numframes - 1;
	}

	if( psprite->frames[frame].type == SPR_SINGLE )
	{
		pspriteframe = psprite->frames[frame].frameptr;
	}
	else if( psprite->frames[frame].type == SPR_GROUP )
	{
		mspritegroup_t *pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		float          *pintervals = pspritegroup->intervals;
		int             numframes = pspritegroup->numframes;
		float           fullinterval = pintervals[numframes - 1];

		// when loading in Mod_LoadSpriteGroup, we guaranteed all interval values
		// are positive, so we don't have to worry about division by zero
		float targettime = cl.time - ((int)( cl.time / fullinterval )) * fullinterval;
		int i;

		for( i = 0; i < ( numframes - 1 ); i++ )
		{
			if( pintervals[i] > targettime )
				break;
		}
		pspriteframe = pspritegroup->frames[i];
	}
	else if( psprite->frames[frame].type == FRAME_ANGLED )
	{
		int angleframe = (int)( Q_rint(( refState.viewangles[1] - yaw + 45.0f ) / 360 * 8 ) - 4 ) & 7;

		// e.g. doom-style sprite monsters
		mspritegroup_t *pspritegroup = (mspritegroup_t *)psprite->frames[frame].frameptr;
		pspriteframe = pspritegroup->frames[angleframe];
	}

	return pspriteframe;
}

void R_GetSpriteParms( int *frameWidth, int *frameHeight, int *numFrames, int currentFrame, const model_t *pSprite )
{
	mspriteframe_t *pFrame;

	if( !pSprite || pSprite->type != mod_sprite || !pSprite->cache.data )
		return;

	pFrame = R_GetSpriteFrame( pSprite, currentFrame, 0.0f );
	/* Lean/GameCube HUD may leave HSPRITE handles whose frames never loaded. */
	if( !pFrame )
		return;

	if( frameWidth )
		*frameWidth = pFrame->width;
	if( frameHeight )
		*frameHeight = pFrame->height;
	if( numFrames )
		*numFrames = pSprite->numframes;
}

int R_GetSpriteTexture( const model_t *m_pSpriteModel, int frame )
{
	mspriteframe_t *pFrame;

	if( !m_pSpriteModel || m_pSpriteModel->type != mod_sprite || !m_pSpriteModel->cache.data )
		return 0;

	pFrame = R_GetSpriteFrame( m_pSpriteModel, frame, 0.0f );
	if( !pFrame )
		return 0;

	return pFrame->gl_texturenum;
}

static void Mod_SpriteTextureReplacementReport( const char *modelname, int gl_texturenum, const char *foundpath )
{
	if( host_allow_materials.value != 2.0f )
		return;

	if( gl_texturenum > 0 )
		Con_Printf( "Looking for %s tex replacement..." S_GREEN "OK (%s)\n", modelname, foundpath );
	else if( gl_texturenum < 0 )
		Con_Printf( "Looking for %s tex replacement..." S_YELLOW "MISS (%s)\n", modelname, foundpath );
	else
		Con_Printf( "Looking for %s tex replacement..." S_RED "FAIL (%s)\n", modelname, foundpath );
}

static qboolean Mod_SpriteSearchForTextureReplacement( char *out, size_t size, const char *modelname, const char *fmt, ... )
{
	va_list ap;
	int ret;

	va_start( ap, fmt );
	ret = Q_vsnprintf( out, size, fmt, ap );
	va_end( ap );

	if( ret < 0 )
	{
		Mod_SpriteTextureReplacementReport( modelname, -1, "overflow" );
		return false;
	}

	if( FS_FileExists( out, false ))
		return true;

	Mod_SpriteTextureReplacementReport( modelname, -1, out );
	return false;
}

#if XASH_GAMECUBE
/* Shrink an indexed frame in place by f (a power of two). Each texel covers
 * an f x f block; in alphatest sprites keep the opaque texel nearest the
 * block centre when the block holds at least f opaque texels, so thin art
 * survives. Writes land at or before the block being read. Returns the new
 * frame header (also written over the old one). */
static dspriteframe_t Mod_GCSpriteShrinkFrame( const void *pin, dspriteframe_t pinframe, int f )
{
	dspriteframe_t small = pinframe;
	byte *px = (byte *)pin + sizeof( dspriteframe_t );
	int x, y;

	small.width = pinframe.width / f;
	small.height = pinframe.height / f;
	for( y = 0; y < small.height; y++ )
	{
		for( x = 0; x < small.width; x++ )
		{
			const byte *blk = px + ( y * f ) * pinframe.width + x * f;
			int bx, by, opaque = 0, best = -1, best_d = 0x7fffffff;
			byte pick = 255;

			for( by = 0; by < f; by++ )
			{
				for( bx = 0; bx < f; bx++ )
				{
					const byte c = blk[by * pinframe.width + bx];
					int d;

					if( c == 255 )
						continue;
					opaque++;
					d = ( 2 * bx + 1 - f ) * ( 2 * bx + 1 - f ) + ( 2 * by + 1 - f ) * ( 2 * by + 1 - f );
					if( d < best_d )
					{
						best_d = d;
						best = c;
					}
				}
			}
			if( !sprite_masked )
				pick = blk[0];
			else if( best >= 0 && opaque >= f )
				pick = (byte)best;
			px[y * small.width + x] = pick;
		}
	}
	memcpy( (void *)pin, &small, sizeof( small ));
	return small;
}

/* Box-filter a direct RGBA HUD frame in place. Premultiplying RGB by alpha
 * during the average avoids coloured fringes around transparent compass art. */
static dspriteframe_t Mod_GCSpriteShrinkRGBA( const void *pin, dspriteframe_t pinframe, int f )
{
	dspriteframe_t small = pinframe;
	byte *px = (byte *)pin + sizeof( dspriteframe_t );
	int x, y;

	small.width = pinframe.width / f;
	small.height = pinframe.height / f;
	for( y = 0; y < small.height; y++ )
	{
		for( x = 0; x < small.width; x++ )
		{
			uint32_t rgb[3] = { 0, 0, 0 }, alpha = 0;
			int bx, by;
			byte *dst = px + ( y * small.width + x ) * 4;

			for( by = 0; by < f; by++ )
			{
				for( bx = 0; bx < f; bx++ )
				{
					const byte *src = px + (( y * f + by ) * pinframe.width + x * f + bx ) * 4;
					const uint32_t a = src[3];

					alpha += a;
					rgb[0] += src[0] * a;
					rgb[1] += src[1] * a;
					rgb[2] += src[2] * a;
				}
			}
			{
				const uint32_t count = (uint32_t)f * (uint32_t)f;
				int c;

				for( c = 0; c < 3; c++ )
					dst[c] = alpha ? (byte)( rgb[c] / alpha ) : 0;
				dst[3] = (byte)( alpha / count );
			}
		}
	}
	memcpy( (void *)pin, &small, sizeof( small ));
	return small;
}
#endif

static const byte *Mod_SpriteLoadFrame( model_t *mod, const void *pin, mspriteframe_t **ppframe, int num )
{
	dspriteframe_t pinframe;
	mspriteframe_t *pspriteframe;
	int gl_texturenum = 0;
	char texname[128];
	int bytes = 1;

	memcpy( &pinframe, pin, sizeof( dspriteframe_t ));

	if( sprite_version == SPRITE_VERSION_32 )
		bytes = 4;

	// build unique frame name
	if( FBitSet( mod->flags, MODEL_CLIENT )) // it's a HUD sprite
	{
		size_t load_bytes = pinframe.width * pinframe.height * bytes;
#if XASH_GAMECUBE
		int hud_hires = -1;
#endif

		Q_snprintf( texname, sizeof( texname ), "#HUD/%s(%s:%i%i).spr", sprite_name, group_suffix, num / 10, num % 10 );
#if XASH_GAMECUBE
		/* DoD's clip sheets are 256x256 and load mid-match, when MEM1 can no
		 * longer hold their RGBA expansion (texture=0, never drawn). The HUD
		 * is laid out at 640 and drawn at 320, so a sheet past 128 px is
		 * shown at half size anyway: shrink its indexed pixels in place
		 * (nearest) before the upload. The frame keeps its source size, so
		 * SPR_DrawHoles rectangles map onto the smaller texture unchanged. */
		/* A 128 px sheet (DoD's crosshair) needs 32 KiB of contiguous heap
		 * mid-match and failed with ~28 KiB free, and again with 130 KiB
		 * free but in pieces: it always takes the same ARAM full-resolution
		 * route with a small resident copy. */
		/* Half-Life's 640 HUD (640hud1-9 256x256/256x128/256x64, the 128x48
		 * pain arrows, 96x96 train dial, 128x128 crosshairs) is drawn 1:1 on
		 * the 640x480 EFB: every sheet from 64 px up keeps its full-resolution
		 * indices in ARAM and is drawn from cells built on demand, so MEM1
		 * holds only a token resident copy of it. */
		if( bytes == 1 && ( gc_sprite_effect_load || !GC_HUD_SHEETS_IN_ARAM
			? GC_MapLoadMemoryOpt() && ( pinframe.width >= 128 || pinframe.height >= 128 )
			: ( ( XASH_GC_MOHAA && Q_stristr( texname, "sprites/mohaa/" ))
				|| pinframe.width >= 64 || pinframe.height >= 64 )))
		{
			/* 128 keeps the art at the size it is drawn, but its texture
			 * needs contiguous heap: on dod_caen 101 KiB free still could not
			 * hold the 16 KiB pixels and the sheet fell to a 2x2 fallback.
			 * Below 256 KiB free, 64 fits the small HUD slots. */
			/* Full-resolution copy in ARAM first: HUD draws then use cells
			 * built from it at full size, and the resident shrunk copy is
			 * only the fallback, so it can be the small one. */
			byte hud_rgba[256 * 4];
			const qboolean hires = Mod_SpriteHudPalette( hud_rgba )
				&& ( hud_hires = R_GXHudHiRegister( texname, pinframe.width, pinframe.height,
					(const byte *)pin + sizeof( dspriteframe_t ), hud_rgba )) >= 0;
			/* With the ARAM copy the resident texture is never sampled
			 * (R_GXHudHiBind draws cells), so it only needs to exist. */
			/* Day of Defeat: its HUD sheets are drawn from full-size cells
			 * too (no cell was refused in any measured match), and five
			 * 64 px fallback copies held 41 KiB of heap for nothing. 32 px
			 * (2 KiB each) still reads as the icon if a cell is ever
			 * refused. The muzzle flash is drawn in the world from its
			 * resident copy and keeps 64. */
			const int cap = ( hires && GC_HUD_SHEETS_IN_ARAM ) ? 8
				: ( hires && !gc_sprite_effect_load && !Q_stristr( texname, "muzzleflash" )) ? 32
				: ( hires || Sys_CheckParm( "-gchudspr64" ) || GC_MemHeapFreeBytes() < 256 * 1024 ) ? 64 : 128;
			int f = 1;

			while( pinframe.width / ( f * 2 ) >= 1 && pinframe.height / ( f * 2 ) >= 1
				&& ( pinframe.width / f > cap || pinframe.height / f > cap ))
				f *= 2;
			if( f > 1 )
			{
				const dspriteframe_t small = Mod_GCSpriteShrinkFrame( pin, pinframe, f );

				load_bytes = small.width * small.height;
				Con_Reportf( "Xash3D GameCube: HUD sprite %s %dx%d shrunk to %dx%d for upload (heap %s)\n",
					texname, pinframe.width, pinframe.height, small.width, small.height,
					Q_memprint( GC_MemHeapFreeBytes( )));
			}
		}
		else if( bytes == 4 && XASH_GC_MOHAA && Q_stristr( texname, "sprites/mohaa/" ))
		{
			static int mohaa_rgba_logs;
			const qboolean hires = ( hud_hires = R_GXHudHiRegisterRGBA( texname, pinframe.width,
				pinframe.height, (const byte *)pin + sizeof( dspriteframe_t ))) >= 0;
			const int cap = hires ? 8 : 32;
			int f = 1;

			while( pinframe.width / ( f * 2 ) >= 1 && pinframe.height / ( f * 2 ) >= 1
				&& ( pinframe.width / f > cap || pinframe.height / f > cap ))
				f *= 2;
			if( f > 1 )
			{
				const dspriteframe_t small = Mod_GCSpriteShrinkRGBA( pin, pinframe, f );

				load_bytes = small.width * small.height * 4;
				if( mohaa_rgba_logs++ < 8 )
					Con_Reportf( "Xash3D GameCube: MOHAA RGBA HUD %s %dx%d stored=%s upload=%dx%d (heap %s)\n",
						texname, pinframe.width, pinframe.height, hires ? "ARAM" : "resident fallback",
						small.width, small.height, Q_memprint( GC_MemHeapFreeBytes( )));
			}
		}
		else if( bytes == 1 && sprite_fx_cap > 0 && ( pinframe.width > sprite_fx_cap || pinframe.height > sprite_fx_cap ))
		{
			static int fx_logs;
			int f = 1;

			while( pinframe.width / ( f * 2 ) >= 1 && pinframe.height / ( f * 2 ) >= 1
				&& ( pinframe.width / f > sprite_fx_cap || pinframe.height / f > sprite_fx_cap ))
				f *= 2;
			if( f > 1 )
			{
				const dspriteframe_t small = Mod_GCSpriteShrinkFrame( pin, pinframe, f );

				load_bytes = small.width * small.height;
				if( fx_logs++ < 12 )
					Con_Reportf( "Xash3D GameCube: effect sprite %s %dx%d shrunk to %dx%d (heap %s)\n",
						texname, pinframe.width, pinframe.height, small.width, small.height,
						Q_memprint( GC_MemHeapFreeBytes( )));
			}
		}
#endif
		gl_texturenum = ref.dllFuncs.GL_LoadTexture( texname, pin, load_bytes, r_texFlags );
#if XASH_GAMECUBE
		if( gl_texturenum && hud_hires >= 0 )
			R_GXHudHiSetTexnum( hud_hires, gl_texturenum );
		if( !gl_texturenum )
			Con_Reportf( S_WARN "Xash3D GameCube: HUD sprite %s texture failed (%dx%d)\n",
				texname, pinframe.width, pinframe.height );
#endif
	}
	else
	{
		// partial HD-textures support
		// Runtime quality-aware adjustments are handled in the renderer.
		if( Mod_AllowMaterials( )) // All platforms: allow materials if configured
		{
			if( Mod_SpriteSearchForTextureReplacement( texname, sizeof( texname ), sprite_name, "materials/%s/%s%i%i.tga", sprite_name, group_suffix, num / 10, num % 10 ))
			{
				gl_texturenum = ref.dllFuncs.GL_LoadTexture( texname, NULL, 0, r_texFlags );
				Mod_SpriteTextureReplacementReport( sprite_name, gl_texturenum, texname );
			}
		}

		if( gl_texturenum == 0 )
		{
			Q_snprintf( texname, sizeof( texname ), "#%s(%s:%i%i).spr", sprite_name, group_suffix, num / 10, num % 10 );
			gl_texturenum = ref.dllFuncs.GL_LoadTexture( texname, pin, pinframe.width * pinframe.height * bytes, r_texFlags );
		}
	}

	// setup frame description
	pspriteframe = Mem_Malloc( mod->mempool, sizeof( mspriteframe_t ));
	pspriteframe->width = pinframe.width;
	pspriteframe->height = pinframe.height;
	pspriteframe->up = pinframe.origin[1];
	pspriteframe->left = pinframe.origin[0];
	pspriteframe->down = pinframe.origin[1] - pinframe.height;
	pspriteframe->right = pinframe.width + pinframe.origin[0];
#if XASH_GAMECUBE
	if( gc_sprite_fx_prescale > 1 )
	{
		pspriteframe->width = pinframe.width * gc_sprite_fx_prescale;
		pspriteframe->height = pinframe.height * gc_sprite_fx_prescale;
		pspriteframe->down = pinframe.origin[1] - pspriteframe->height;
		pspriteframe->right = pspriteframe->width + pinframe.origin[0];
	}
#endif
	pspriteframe->gl_texturenum = gl_texturenum;
	*ppframe = pspriteframe;

	return (const byte *)pin + sizeof( dspriteframe_t ) + pinframe.width * pinframe.height * bytes;
}

static const byte *Mod_SpriteLoadGroup( model_t *mod, const void *pin, mspriteframe_t **ppframe, int framenum )
{
	const dspritegroup_t    *pingroup = (const dspritegroup_t *)pin;
	mspritegroup_t          *pspritegroup;
	const dspriteinterval_t *pin_intervals;
	float      *poutintervals;
	int         groupsize, numframes = pingroup->numframes;
	const void *ptemp;

	groupsize = sizeof( mspritegroup_t ) + ( numframes - 1 ) * sizeof( pspritegroup->frames[0] );
	pspritegroup = Mem_Calloc( mod->mempool, groupsize );
	pspritegroup->numframes = numframes;

	*ppframe = (mspriteframe_t *)pspritegroup;
	pin_intervals = (const dspriteinterval_t *)( pingroup + 1 );
	poutintervals = Mem_Calloc( mod->mempool, numframes * sizeof( float ));
	pspritegroup->intervals = poutintervals;

	for( int i = 0; i < numframes; i++ )
	{
		*poutintervals = pin_intervals->interval;
		if( *poutintervals <= 0.0f )
			*poutintervals = 1.0f; // set error value
		poutintervals++;
		pin_intervals++;
	}

	ptemp = (const void *)pin_intervals;
	for( int i = 0; i < numframes; i++ )
	{
		ptemp = Mod_SpriteLoadFrame( mod, ptemp, &pspritegroup->frames[i], framenum * 10 + i );
	}

	return ptemp;
}

void Mod_SpriteLoadTextures( model_t *mod, const void *buffer )
{
	const dsprite_t *pin = buffer;
	const short     *numi = NULL;
	const byte      *pframetype;
	msprite_t       *psprite = mod->cache.data;
	int i;

	if( pin->version == SPRITE_VERSION_Q1 || pin->version == SPRITE_VERSION_32 )
		numi = NULL;
	else if( pin->version == SPRITE_VERSION_HL )
		numi = (const short *)((const byte *)buffer + sizeof( dsprite_hl_t ));

	r_texFlags = mod->numtexinfo;
	sprite_version = pin->version;
#if XASH_GAMECUBE
	sprite_masked = false;
	sprite_pal = NULL;
	sprite_pal_count = 0;
	sprite_tex_format = psprite->texFormat;
	/* 64 px per frame: the renderer stores opaque and cut-out frames as CMPR
	 * (ref/gx/r_image.c). Animations with a real alpha channel (indexalpha
	 * smoke) stay RGB565 and keep 32 px. CL_GCLoadEffectSpriteShrunk
	 * (cl_game.c) follows the same rule. */
	sprite_fx_cap = gc_sprite_effect_load
		? (( mod->numframes <= 4 || psprite->texFormat != SPR_INDEXALPHA ) ? 64 : 32 ) : 0;
#endif
	Q_strncpy( sprite_name, mod->name, sizeof( sprite_name ));
	COM_StripExtension( sprite_name );

	if( numi == NULL )
	{
		rgbdata_t *pal;

		pal = FS_LoadImage( "#id.pal", (byte *)&i, 768 );
		pframetype = (const byte *)buffer + sizeof( dsprite_q1_t );
		FS_FreeImage( pal );
	}
	else if( *numi <= 256 )
	{
		const byte *src = (const byte *)( numi + 1 );
		rgbdata_t  *pal;
		size_t      pal_bytes = *numi * 3;

#if XASH_GAMECUBE
		sprite_masked = psprite->texFormat == SPR_ALPHTEST;
		sprite_pal = src;
		sprite_pal_count = *numi;
#endif
		// install palette
		switch( psprite->texFormat )
		{
		case SPR_INDEXALPHA:
			pal = FS_LoadImage( "#gradient.pal", src, pal_bytes );
			break;
		case SPR_ALPHTEST:
			pal = FS_LoadImage( "#masked.pal", src, pal_bytes );
			break;
		default:
			pal = FS_LoadImage( "#texgamma.pal", src, pal_bytes );
			break;
		}

		pframetype = (const byte *)( src + pal_bytes );
		FS_FreeImage( pal );
	}
	else
	{
		Con_DPrintf( S_ERROR "%s has wrong number of palette colors %i (should be less or equal than 256)\n", mod->name, *numi );
		return;
	}

	if( mod->numframes < 1 )
		return;

	for( i = 0; i < mod->numframes; i++ )
	{
		frametype_t  frametype;
		dframetype_t dframetype;

		memcpy( &dframetype, pframetype, sizeof( dframetype ));
		frametype = dframetype.type;
		psprite->frames[i].type = (spriteframetype_t)frametype;

		switch( frametype )
		{
		case FRAME_SINGLE:
			Q_strncpy( group_suffix, "frame", sizeof( group_suffix ));
			pframetype = Mod_SpriteLoadFrame( mod, pframetype + sizeof( dframetype_t ), &psprite->frames[i].frameptr, i );
			break;
		case FRAME_GROUP:
			Q_strncpy( group_suffix, "group", sizeof( group_suffix ));
			pframetype = Mod_SpriteLoadGroup( mod, pframetype + sizeof( dframetype_t ), &psprite->frames[i].frameptr, i );
			break;
		case FRAME_ANGLED:
			Q_strncpy( group_suffix, "angle", sizeof( group_suffix ));
			pframetype = Mod_SpriteLoadGroup( mod, pframetype + sizeof( dframetype_t ), &psprite->frames[i].frameptr, i );
			break;
		}
		if( pframetype == NULL )
			break;
	}
}

void Mod_SpriteUnloadTextures( void *data )
{
	msprite_t *psprite = data;

	if( !data )
		return;

	// release all textures
	for( int i = 0; i < psprite->numframes; i++ )
	{
		if( !psprite->frames[i].frameptr )
			continue;

		if( psprite->frames[i].type == SPR_SINGLE )
		{
			ref.dllFuncs.GL_FreeTexture( psprite->frames[i].frameptr->gl_texturenum );
		}
		else
		{
			mspritegroup_t *pspritegroup = (mspritegroup_t *)psprite->frames[i].frameptr;

			for( int j = 0; j < pspritegroup->numframes; j++ )
			{
				if( pspritegroup->frames[j] )
					ref.dllFuncs.GL_FreeTexture( pspritegroup->frames[j]->gl_texturenum );
			}
		}
	}
}
