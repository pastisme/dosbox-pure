/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */


#include <sys/types.h>
#include <assert.h>
#include <math.h>
#include <fstream>
#include <sstream>
#include <stdlib.h>

#include "dosbox.h"
#include "video.h"
#include "render.h"
#include "setup.h"
#include "control.h"
#include "mapper.h"
#include "cross.h"
#include "hardware.h"
#include "support.h"
#include "shell.h"

#include "render_scalers.h"
#include "render_glsl.h"

Render_t render;
ScalerLineHandler_t RENDER_DrawLine;

static void RENDER_CallBack( GFX_CallBackFunctions_t function );

static void Check_Palette(void) {
	/* Clean up any previous changed palette data */
	if (render.pal.changed) {
		memset(render.pal.modified, 0, sizeof(render.pal.modified));
		render.pal.changed = false;
	}
	if (render.pal.first>render.pal.last) 
		return;
	Bitu i;
	switch (render.scale.outMode) {
	case scalerMode8:
		GFX_SetPalette(render.pal.first,render.pal.last-render.pal.first+1,(GFX_PalEntry *)&render.pal.rgb[render.pal.first]);
		break;
	case scalerMode15:
	case scalerMode16:
		for (i=render.pal.first;i<=render.pal.last;i++) {
			Bit8u r=render.pal.rgb[i].red;
			Bit8u g=render.pal.rgb[i].green;
			Bit8u b=render.pal.rgb[i].blue;
			Bit16u newPal = GFX_GetRGB(r,g,b);
			if (newPal != render.pal.lut.b16[i]) {
				render.pal.changed = true;
				render.pal.modified[i] = 1;
				render.pal.lut.b16[i] = newPal;
			}
		}
		break;
	case scalerMode32:
	default:
		for (i=render.pal.first;i<=render.pal.last;i++) {
			Bit8u r=render.pal.rgb[i].red;
			Bit8u g=render.pal.rgb[i].green;
			Bit8u b=render.pal.rgb[i].blue;
			Bit32u newPal = GFX_GetRGB(r,g,b);
			if (newPal != render.pal.lut.b32[i]) {
				render.pal.changed = true;
				render.pal.modified[i] = 1;
				render.pal.lut.b32[i] = newPal;
			}
		}
		break;
	}
	/* Setup pal index to startup values */
	render.pal.first=256;
	render.pal.last=0;
}

void RENDER_SetPal(Bit8u entry,Bit8u red,Bit8u green,Bit8u blue) {
	render.pal.rgb[entry].red=red;
	render.pal.rgb[entry].green=green;
	render.pal.rgb[entry].blue=blue;
	if (render.pal.first>entry) render.pal.first=entry;
	if (render.pal.last<entry) render.pal.last=entry;
}

static void RENDER_EmptyLineHandler(const void * src) {
}

#ifdef C_DBP_ENABLE_SCALERCACHE
static void RENDER_StartLineHandler(const void * s) {
	if (s) {
		const Bitu *src = (Bitu*)s;
		Bitu *cache = (Bitu*)(render.scale.cacheRead);
		for (Bits x=render.src.start;x>0;) {
			if (GCC_UNLIKELY(src[0] != cache[0])) {
				if (!GFX_StartUpdate( render.scale.outWrite, render.scale.outPitch )) {
					RENDER_DrawLine = RENDER_EmptyLineHandler;
					return;
				}
				render.scale.outWrite += render.scale.outPitch * Scaler_ChangedLines[0];
				RENDER_DrawLine = render.scale.lineHandler;
				RENDER_DrawLine( s );
				return;
			}
			x--; src++; cache++;
		}
	}
	render.scale.cacheRead += render.scale.cachePitch;
	Scaler_ChangedLines[0] += Scaler_Aspect[ render.scale.inLine ];
	render.scale.inLine++;
	render.scale.outLine++;
}

static void RENDER_FinishLineHandler(const void * s) {
	if (s) {
		const Bitu *src = (Bitu*)s;
		Bitu *cache = (Bitu*)(render.scale.cacheRead);
		for (Bits x=render.src.start;x>0;) {
			cache[0] = src[0];
			x--; src++; cache++;
		}
	}
	render.scale.cacheRead += render.scale.cachePitch;
}


static void RENDER_ClearCacheHandler(const void * src) {
	Bitu x, width;
	Bit32u *srcLine, *cacheLine;
	srcLine = (Bit32u *)src;
	cacheLine = (Bit32u *)render.scale.cacheRead;
	width = render.scale.cachePitch / 4;
	for (x=0;x<width;x++)
		cacheLine[x] = ~srcLine[x];
	render.scale.lineHandler( src );
}
#endif

bool RENDER_StartUpdate(void) {
	if (GCC_UNLIKELY(render.updating))
		return false;
	if (GCC_UNLIKELY(!render.active))
		return false;
	if (GCC_UNLIKELY(render.frameskip.count<render.frameskip.max)) {
		render.frameskip.count++;
		return false;
	}
	render.frameskip.count=0;
	if (render.scale.inMode == scalerMode8) {
		Check_Palette();
	}
	render.scale.inLine = 0;
	render.scale.outLine = 0;
#ifdef C_DBP_ENABLE_SCALERCACHE
	render.scale.cacheRead = (Bit8u*)&scalerSourceCache;
#endif
	render.scale.outWrite = 0;
	render.scale.outPitch = 0;
#ifndef C_DBP_ENABLE_SCALERCACHE
	if (GCC_UNLIKELY(!GFX_StartUpdate( render.scale.outWrite, render.scale.outPitch )))
		return false;
	RENDER_DrawLine = render.scale.lineHandler;
#else
	Scaler_ChangedLines[0] = 0;
	Scaler_ChangedLineIndex = 0;
	/* Clearing the cache will first process the line to make sure it's never the same */
	if (GCC_UNLIKELY( render.scale.clearCache) ) {
//		LOG_MSG("Clearing cache");
		//Will always have to update the screen with this one anyway, so let's update already
		if (GCC_UNLIKELY(!GFX_StartUpdate( render.scale.outWrite, render.scale.outPitch )))
			return false;
#ifdef VGA_KEEP_CHANGES
		render.fullFrame = true;
#endif
		render.scale.clearCache = false;
		RENDER_DrawLine = RENDER_ClearCacheHandler;
	} else {
		if (render.pal.changed) {
			/* Assume pal changes always do a full screen update anyway */
			if (GCC_UNLIKELY(!GFX_StartUpdate( render.scale.outWrite, render.scale.outPitch )))
				return false;
			RENDER_DrawLine = render.scale.linePalHandler;
#ifdef VGA_KEEP_CHANGES
			render.fullFrame = true;
#endif
		} else {
			RENDER_DrawLine = RENDER_StartLineHandler;
#ifdef C_DBP_ENABLE_CAPTURE
			if (GCC_UNLIKELY(CaptureState & (CAPTURE_IMAGE|CAPTURE_VIDEO))) 
#ifdef VGA_KEEP_CHANGES
				render.fullFrame = true;
#endif
			else
#endif
#ifdef VGA_KEEP_CHANGES
				render.fullFrame = false;
#endif
		}
	}
#endif
	render.updating = true;
	return true;
}

static void RENDER_Halt( void ) {
	RENDER_DrawLine = RENDER_EmptyLineHandler;
	GFX_EndUpdate( 0 );
	render.updating=false;
	render.active=false;
}

extern Bitu PIC_Ticks;
void RENDER_EndUpdate( bool abort ) {
	if (GCC_UNLIKELY(!render.updating))
		return;
	RENDER_DrawLine = RENDER_EmptyLineHandler;
#ifdef C_DBP_ENABLE_CAPTURE
	if (GCC_UNLIKELY(CaptureState & (CAPTURE_IMAGE|CAPTURE_VIDEO))) {
		Bitu pitch, flags;
		flags = 0;
		if (render.src.dblw != render.src.dblh) {
			if (render.src.dblw) flags|=CAPTURE_FLAG_DBLW;
			if (render.src.dblh) flags|=CAPTURE_FLAG_DBLH;
		}
		if (render.scale.outWrite==NULL) flags|=CAPTURE_FLAG_DUPLICATE;
		float fps = render.src.fps;
		pitch = render.scale.cachePitch;
		if (render.frameskip.max)
			fps /= 1+render.frameskip.max;
		CAPTURE_AddImage( render.src.width, render.src.height, render.src.bpp, pitch,
			flags, fps, (Bit8u *)&scalerSourceCache, (Bit8u*)&render.pal.rgb );
	}
#endif
	if ( render.scale.outWrite ) {
#ifndef C_DBP_ENABLE_SCALERCACHE
		GFX_EndUpdate( abort? NULL : (const Bit16u*)(size_t)1 );
#else
		GFX_EndUpdate( abort? NULL : Scaler_ChangedLines );
#endif
#if 0
		render.frameskip.hadSkip[render.frameskip.index] = 0;
#endif
	} else {
#if 0
		Bitu total = 0, i;
		render.frameskip.hadSkip[render.frameskip.index] = 1;
		for (i = 0;i<RENDER_SKIP_CACHE;i++) 
			total += render.frameskip.hadSkip[i];
		LOG_MSG( "Skipped frame %d %d", PIC_Ticks, (total * 100) / RENDER_SKIP_CACHE );
		if (RENDER_GetForceUpdate()) GFX_EndUpdate(0);
#endif
	}
#if 0
	render.frameskip.index = (render.frameskip.index + 1) & (RENDER_SKIP_CACHE - 1);
#endif
	render.updating=false;
}

static Bitu MakeAspectTable(Bitu skip,Bitu height,double scaley,Bitu miny) {
	Bitu i;
	double lines=0;
	Bitu linesadded=0;
	for (i=0;i<skip;i++)
		Scaler_Aspect[i] = 0;

	height += skip;
	for (i=skip;i<height;i++) {
		lines += scaley;
		if (lines >= miny) {
			Bitu templines = (Bitu)lines;
			lines -= templines;
			linesadded += templines;
			Scaler_Aspect[i] = templines;
		} else {
			Scaler_Aspect[i] = 0;
		}
	}
	return linesadded;
}


static void RENDER_Reset( void ) {
	Bitu width=render.src.width;
	Bitu height=render.src.height;
	bool dblw=render.src.dblw;
	bool dblh=render.src.dblh;

	double gfx_scalew;
	double gfx_scaleh;
	
	Bitu gfx_flags, xscale, yscale;
	ScalerSimpleBlock_t		*simpleBlock = &ScaleNormal1x;
	ScalerComplexBlock_t	*complexBlock = 0;
	if (render.aspect) {
		if (render.src.ratio>1.0) {
			gfx_scalew = 1;
			gfx_scaleh = render.src.ratio;
		} else {
			gfx_scalew = (1/render.src.ratio);
			gfx_scaleh = 1;
		}
	} else {
		gfx_scalew = 1;
		gfx_scaleh = 1;
	}

#ifdef C_DBP_ENABLE_SCALERS
	/* Don't do software scaler sizes larger than 4k */
	Bitu maxsize_current_input = SCALER_MAXLINE_WIDTH/width;
	if (render.scale.size > maxsize_current_input) render.scale.size = maxsize_current_input;

	if ((dblh && dblw) || (render.scale.forced && !dblh && !dblw)) {
		/* Initialize always working defaults */
		if (render.scale.size == 2)
			simpleBlock = &ScaleNormal2x;
		else if (render.scale.size == 3)
			simpleBlock = &ScaleNormal3x;
		else
			simpleBlock = &ScaleNormal1x;
		/* Maybe override them */
#if RENDER_USE_ADVANCED_SCALERS>0
		switch (render.scale.op) {
#if RENDER_USE_ADVANCED_SCALERS>2
		case scalerOpAdvInterp:
			if (render.scale.size == 2)
				complexBlock = &ScaleAdvInterp2x;
			else if (render.scale.size == 3)
				complexBlock = &ScaleAdvInterp3x;
			break;
		case scalerOpAdvMame:
			if (render.scale.size == 2)
				complexBlock = &ScaleAdvMame2x;
			else if (render.scale.size == 3)
				complexBlock = &ScaleAdvMame3x;
			break;
		case scalerOpHQ:
			if (render.scale.size == 2)
				complexBlock = &ScaleHQ2x;
			else if (render.scale.size == 3)
				complexBlock = &ScaleHQ3x;
			break;
		case scalerOpSuperSaI:
			if (render.scale.size == 2)
				complexBlock = &ScaleSuper2xSaI;
			break;
		case scalerOpSuperEagle:
			if (render.scale.size == 2)
				complexBlock = &ScaleSuperEagle;
			break;
		case scalerOpSaI:
			if (render.scale.size == 2)
				complexBlock = &Scale2xSaI;
			break;
#endif
		case scalerOpTV:
			if (render.scale.size == 2)
				simpleBlock = &ScaleTV2x;
			else if (render.scale.size == 3)
				simpleBlock = &ScaleTV3x;
			break;
		case scalerOpRGB:
			if (render.scale.size == 2)
				simpleBlock = &ScaleRGB2x;
			else if (render.scale.size == 3)
				simpleBlock = &ScaleRGB3x;
			break;
		case scalerOpScan:
			if (render.scale.size == 2)
				simpleBlock = &ScaleScan2x;
			else if (render.scale.size == 3)
				simpleBlock = &ScaleScan3x;
			break;
		default:
			break;
		}
#endif
	} else if (dblw) {
		simpleBlock = &ScaleNormalDw;
		if (width * simpleBlock->xscale > SCALER_MAXLINE_WIDTH) {
			// This should only happen if you pick really bad values... but might be worth adding selecting a scaler that fits
			simpleBlock = &ScaleNormal1x;
		}
	} else if (dblh) {
		simpleBlock = &ScaleNormalDh;
	} else  {
forcenormal:
		complexBlock = 0;
		simpleBlock = &ScaleNormal1x;
	}
	if (complexBlock) {
#if RENDER_USE_ADVANCED_SCALERS>1
		if ((width >= SCALER_COMPLEXWIDTH - 16) || height >= SCALER_COMPLEXHEIGHT - 16) {
			LOG_MSG("Scaler can't handle this resolution, going back to normal");
			goto forcenormal;
		}
#else
		goto forcenormal;
#endif
		gfx_flags = complexBlock->gfxFlags;
		xscale = complexBlock->xscale;	
		yscale = complexBlock->yscale;
//		LOG_MSG("Scaler:%s",complexBlock->name);
	} else {
		gfx_flags = simpleBlock->gfxFlags;
		xscale = simpleBlock->xscale;	
		yscale = simpleBlock->yscale;
//		LOG_MSG("Scaler:%s",simpleBlock->name);
	}
#else // C_DBP_ENABLE_SCALERS
forcenormal:
	complexBlock = 0;
	simpleBlock = &ScaleNormal1x;
	gfx_flags = simpleBlock->gfxFlags;
	xscale = simpleBlock->xscale;	
	yscale = simpleBlock->yscale;
#endif // C_DBP_ENABLE_SCALERS
	switch (render.src.bpp) {
	case 8:
			render.src.start = ( render.src.width * 1) / sizeof(Bitu);
			if (gfx_flags & GFX_CAN_8)
				gfx_flags |= GFX_LOVE_8;
			else
				gfx_flags |= GFX_LOVE_32;
			break;
	case 15:
			render.src.start = ( render.src.width * 2) / sizeof(Bitu);
			gfx_flags |= GFX_LOVE_15;
			gfx_flags = (gfx_flags & ~GFX_CAN_8) | GFX_RGBONLY;
			break;
	case 16:
			render.src.start = ( render.src.width * 2) / sizeof(Bitu);
			gfx_flags |= GFX_LOVE_16;
			gfx_flags = (gfx_flags & ~GFX_CAN_8) | GFX_RGBONLY;
			break;
	case 32:
			render.src.start = ( render.src.width * 4) / sizeof(Bitu);
			gfx_flags |= GFX_LOVE_32;
			gfx_flags = (gfx_flags & ~GFX_CAN_8) | GFX_RGBONLY;
			break;
	}
	gfx_flags=GFX_GetBestMode(gfx_flags);
	if (!gfx_flags) {
		if (!complexBlock && simpleBlock == &ScaleNormal1x) 
			E_Exit("Failed to create a rendering output");
		else 
			goto forcenormal;
	}
	width *= xscale;
	Bitu skip = complexBlock ? 1 : 0;
	if (gfx_flags & GFX_SCALING) {
		height = MakeAspectTable(skip, render.src.height, (double)yscale, yscale );
	} else {
		if ((gfx_flags & GFX_CAN_RANDOM) && gfx_scaleh > 1) {
			gfx_scaleh *= yscale;
			height = MakeAspectTable( skip, render.src.height, gfx_scaleh, yscale );
		} else {
			gfx_flags &= ~GFX_CAN_RANDOM;		//Hardware surface when possible
			height = MakeAspectTable( skip, render.src.height, (double)yscale, yscale);
		}
	}
/* Setup the scaler variables */
#if C_OPENGL
	GFX_SetShader(render.shader_src);
#endif
	gfx_flags=GFX_SetSize(width,height,gfx_flags,gfx_scalew,gfx_scaleh,&RENDER_CallBack);
#ifdef C_DBP_ENABLE_SCALERS
	if (gfx_flags & GFX_CAN_8)
		render.scale.outMode = scalerMode8;
	else if (gfx_flags & GFX_CAN_15)
		render.scale.outMode = scalerMode15;
	else if (gfx_flags & GFX_CAN_16)
		render.scale.outMode = scalerMode16;
	else
#endif
	if (gfx_flags & GFX_CAN_32)
		render.scale.outMode = scalerMode32;
	else 
		E_Exit("Failed to create a rendering output");
	ScalerLineBlock_t *lineBlock;
	if (gfx_flags & GFX_HARDWARE) {
#if RENDER_USE_ADVANCED_SCALERS>1
		if (complexBlock) {
			lineBlock = &ScalerCache;
			render.scale.complexHandler = complexBlock->Linear[ render.scale.outMode ];
		} else
#endif
		{
			render.scale.complexHandler = 0;
			lineBlock = &simpleBlock->Linear;
		}
	} else {
#if RENDER_USE_ADVANCED_SCALERS>1
		if (complexBlock) {
			lineBlock = &ScalerCache;
			render.scale.complexHandler = complexBlock->Random[ render.scale.outMode ];
		} else
#endif
		{
			render.scale.complexHandler = 0;
			lineBlock = &simpleBlock->Random;
		}
	}
	switch (render.src.bpp) {
	case 8:
		render.scale.lineHandler = (*lineBlock)[0][render.scale.outMode];
		render.scale.linePalHandler = (*lineBlock)[4][render.scale.outMode];
		render.scale.inMode = scalerMode8;
#ifdef C_DBP_ENABLE_SCALERCACHE
		render.scale.cachePitch = render.src.width * 1;
#endif
		break;
	case 15:
		render.scale.lineHandler = (*lineBlock)[1][render.scale.outMode];
		render.scale.linePalHandler = 0;
		render.scale.inMode = scalerMode15;
#ifdef C_DBP_ENABLE_SCALERCACHE
		render.scale.cachePitch = render.src.width * 2;
#endif
		break;
	case 16:
		render.scale.lineHandler = (*lineBlock)[2][render.scale.outMode];
		render.scale.linePalHandler = 0;
		render.scale.inMode = scalerMode16;
#ifdef C_DBP_ENABLE_SCALERCACHE
		render.scale.cachePitch = render.src.width * 2;
#endif
		break;
	case 32:
		render.scale.lineHandler = (*lineBlock)[3][render.scale.outMode];
		render.scale.linePalHandler = 0;
		render.scale.inMode = scalerMode32;
#ifdef C_DBP_ENABLE_SCALERCACHE
		render.scale.cachePitch = render.src.width * 4;
#endif
		break;
	default:
		E_Exit("RENDER:Wrong source bpp %" sBitfs(d), render.src.bpp );
	}
	render.scale.blocks = render.src.width / SCALER_BLOCKSIZE;
	render.scale.lastBlock = render.src.width % SCALER_BLOCKSIZE;
	render.scale.inHeight = render.src.height;
	/* Reset the palette change detection to it's initial value */
	render.pal.first= 0;
	render.pal.last = 255;
	render.pal.changed = false;
	memset(render.pal.modified, 0, sizeof(render.pal.modified));
	//Finish this frame using a copy only handler
#ifdef C_DBP_ENABLE_SCALERCACHE
	RENDER_DrawLine = RENDER_FinishLineHandler;
#else
	RENDER_DrawLine = RENDER_EmptyLineHandler;
#endif
	render.scale.outWrite = 0;
#ifdef C_DBP_ENABLE_SCALERCACHE
	/* Signal the next frame to first reinit the cache */
	render.scale.clearCache = true;
#endif
	render.active=true;
}

static void RENDER_CallBack( GFX_CallBackFunctions_t function ) {
	if (function == GFX_CallBackStop) {
		RENDER_Halt( );	
		return;
	} else if (function == GFX_CallBackRedraw) {
#ifdef C_DBP_ENABLE_SCALERCACHE
		render.scale.clearCache = true;
#endif
		return;
	} else if ( function == GFX_CallBackReset) {
		GFX_EndUpdate( 0 );	
		RENDER_Reset();
	} else {
		E_Exit("Unhandled GFX_CallBackReset %d", function );
	}
}

void RENDER_SetSize(Bitu width,Bitu height,Bitu bpp,float fps,double ratio,bool dblw,bool dblh) {
	DBP_ASSERT(fps > 1);
	RENDER_Halt( );
	if (!width || !height || width > SCALER_MAXWIDTH || height > SCALER_MAXHEIGHT) { 
		return;	
	}
	if ( ratio > 1 ) {
		double target = height * ratio + 0.025;
		ratio = target / height;
	} else {
		//This would alter the width of the screen, we don't care about rounding errors here
	}
	render.src.width=width;
	render.src.height=height;
	render.src.bpp=bpp;
	render.src.dblw=dblw;
	render.src.dblh=dblh;
	render.src.fps=fps;
	render.src.ratio=ratio;
	RENDER_Reset( );
}

#ifdef C_DBP_ENABLE_MAPPER
extern void GFX_SetTitle(Bit32s cycles, int frameskip,bool paused);
static void IncreaseFrameSkip(bool pressed) {
	if (!pressed)
		return;
	if (render.frameskip.max<10) render.frameskip.max++;
	LOG_MSG("Frame Skip at %d",render.frameskip.max);
	GFX_SetTitle(-1,render.frameskip.max,false);
}

static void DecreaseFrameSkip(bool pressed) {
	if (!pressed)
		return;
	if (render.frameskip.max>0) render.frameskip.max--;
	LOG_MSG("Frame Skip at %d",render.frameskip.max);
	GFX_SetTitle(-1,render.frameskip.max,false);
}
/* Disabled as I don't want to waste a keybind for that. Might be used in the future (Qbix)
static void ChangeScaler(bool pressed) {
	if (!pressed)
		return;
	render.scale.op = (scalerOperation)((int)render.scale.op+1);
	if((render.scale.op) >= scalerLast || render.scale.size == 1) {
		render.scale.op = (scalerOperation)0;
		if(++render.scale.size > 3)
			render.scale.size = 1;
	}
	RENDER_CallBack( GFX_CallBackReset );
} */
#endif

#if 0
bool RENDER_GetForceUpdate(void) {
	return render.forceUpdate;
}

void RENDER_SetForceUpdate(bool f) {
	render.forceUpdate = f;
}
#endif

#if C_OPENGL
static bool RENDER_GetShader(std::string& shader_path, char *old_src) {
	char* src;
	std::stringstream buf;
	std::ifstream fshader(shader_path.c_str(), std::ios_base::binary);
	if (!fshader.is_open()) fshader.open((shader_path + ".glsl").c_str(), std::ios_base::binary);
	if (fshader.is_open()) {
		buf << fshader.rdbuf();
		fshader.close();
	}
	else if (shader_path == "advinterp2x") buf << advinterp2x_glsl;
	else if (shader_path == "advinterp3x") buf << advinterp3x_glsl;
	else if (shader_path == "advmame2x")   buf << advmame2x_glsl;
	else if (shader_path == "advmame3x")   buf << advmame3x_glsl;
	else if (shader_path == "rgb2x")       buf << rgb2x_glsl;
	else if (shader_path == "rgb3x")       buf << rgb3x_glsl;
	else if (shader_path == "scan2x")      buf << scan2x_glsl;
	else if (shader_path == "scan3x")      buf << scan3x_glsl;
	else if (shader_path == "tv2x")        buf << tv2x_glsl;
	else if (shader_path == "tv3x")        buf << tv3x_glsl;
	else if (shader_path == "sharp")       buf << sharp_glsl;

	if (!buf.str().empty()) {
		std::string s = buf.str() + '\n';
		if (first_shell) {
			std::string pre_defs;
			Bitu count = first_shell->GetEnvCount();
			for (Bitu i=0; i < count; i++) {
				std::string env;
				if (!first_shell->GetEnvNum(i, env))
					continue;
				if (env.compare(0, 9, "GLSHADER_")==0) {
					size_t brk = env.find('=');
					if (brk == std::string::npos) continue;
					env[brk] = ' ';
					pre_defs += "#define " + env.substr(9) + '\n';
				}
			}
			if (!pre_defs.empty()) {
				// if "#version" occurs it must be before anything except comments and whitespace
				size_t pos = s.find("#version ");
				if (pos != std::string::npos) pos = s.find('\n', pos+9);
				if (pos == std::string::npos) pos = 0;
				else ++pos;
				s = s.insert(pos, pre_defs);
			}
		}
		// keep the same buffer if contents aren't different
		if (old_src==NULL || s != old_src) {
			src = strdup(s.c_str());
			if (src==NULL) LOG_MSG("WARNING: Couldn't copy shader source");
		} else src = old_src;
	} else src = NULL;
	render.shader_src = src;
	return src != NULL;
}
#endif

void RENDER_Init(Section * sec) {
	Section_prop * section=static_cast<Section_prop *>(sec);

	//For restarting the renderer.
	static bool running = false;
	bool aspect = render.aspect;
#ifdef C_DBP_ENABLE_SCALERS
	Bitu scalersize = render.scale.size;
	bool scalerforced = render.scale.forced;
	scalerOperation_t scaleOp = render.scale.op;
#endif

	render.pal.first=256;
	render.pal.last=0;
	render.aspect=section->Get_bool("aspect");
	render.frameskip.max=section->Get_int("frameskip");
	render.frameskip.count=0;
#ifdef C_DBP_ENABLE_SCALERS
	std::string cline;
	std::string scaler;
	//Check for commandline paramters and parse them through the configclass so they get checked against allowed values
	if (control->cmdline->FindString("-scaler",cline,true)) {
		section->HandleInputline(std::string("scaler=") + cline);
	} else if (control->cmdline->FindString("-forcescaler",cline,true)) {
		section->HandleInputline(std::string("scaler=") + cline + " forced");
	}
	   
	Prop_multival* prop = section->Get_multival("scaler");
	scaler = prop->GetSection()->Get_string("type");
	std::string f = prop->GetSection()->Get_string("force");
	render.scale.forced = false;
	if(f == "forced") render.scale.forced = true;
   
	if (scaler == "none") { render.scale.op = scalerOpNormal;render.scale.size = 1; }
	else if (scaler == "normal2x") { render.scale.op = scalerOpNormal;render.scale.size = 2; }
	else if (scaler == "normal3x") { render.scale.op = scalerOpNormal;render.scale.size = 3; }
#if RENDER_USE_ADVANCED_SCALERS>2
	else if (scaler == "advmame2x") { render.scale.op = scalerOpAdvMame;render.scale.size = 2; }
	else if (scaler == "advmame3x") { render.scale.op = scalerOpAdvMame;render.scale.size = 3; }
	else if (scaler == "advinterp2x") { render.scale.op = scalerOpAdvInterp;render.scale.size = 2; }
	else if (scaler == "advinterp3x") { render.scale.op = scalerOpAdvInterp;render.scale.size = 3; }
	else if (scaler == "hq2x") { render.scale.op = scalerOpHQ;render.scale.size = 2; }
	else if (scaler == "hq3x") { render.scale.op = scalerOpHQ;render.scale.size = 3; }
	else if (scaler == "2xsai") { render.scale.op = scalerOpSaI;render.scale.size = 2; }
	else if (scaler == "super2xsai") { render.scale.op = scalerOpSuperSaI;render.scale.size = 2; }
	else if (scaler == "supereagle") { render.scale.op = scalerOpSuperEagle;render.scale.size = 2; }
#endif
#if RENDER_USE_ADVANCED_SCALERS>0
	else if (scaler == "tv2x") { render.scale.op = scalerOpTV;render.scale.size = 2; }
	else if (scaler == "tv3x") { render.scale.op = scalerOpTV;render.scale.size = 3; }
	else if (scaler == "rgb2x"){ render.scale.op = scalerOpRGB;render.scale.size = 2; }
	else if (scaler == "rgb3x"){ render.scale.op = scalerOpRGB;render.scale.size = 3; }
	else if (scaler == "scan2x"){ render.scale.op = scalerOpScan;render.scale.size = 2; }
	else if (scaler == "scan3x"){ render.scale.op = scalerOpScan;render.scale.size = 3; }
#endif
#endif
#if C_OPENGL
	char* shader_src = render.shader_src;
	Prop_path *sh = section->Get_path("glshader");
	f = (std::string)sh->GetValue();
	if (f.empty() || f=="none") render.shader_src = NULL;
	else if (!RENDER_GetShader(sh->realpath,shader_src)) {
		std::string path;
		Cross::GetPlatformConfigDir(path);
		path = path + "glshaders" + CROSS_FILESPLIT + f;
		if (!RENDER_GetShader(path,shader_src) && (sh->realpath==f || !RENDER_GetShader(f,shader_src))) {
			sh->SetValue("none");
			LOG_MSG("Shader file \"%s\" not found", f.c_str());
		}
	}
	if (shader_src!=render.shader_src) free(shader_src);
#endif

	//If something changed that needs a ReInit
	// Only ReInit when there is a src.bpp (fixes crashes on startup and directly changing the scaler without a screen specified yet)
	if(running && render.src.bpp && ((render.aspect != aspect)
#ifdef C_DBP_ENABLE_SCALERS
				|| (render.scale.op != scaleOp) || (render.scale.size != scalersize) || (render.scale.forced != scalerforced)
#endif
#if C_OPENGL
				|| (render.shader_src != shader_src)
#endif
#ifdef C_DBP_ENABLE_SCALERS
				|| render.scale.forced
#endif
				))
		RENDER_CallBack( GFX_CallBackReset );

	if(!running) render.updating=true;
	running = true;

#ifdef C_DBP_ENABLE_MAPPER
	MAPPER_AddHandler(DecreaseFrameSkip,MK_f7,MMOD1,"decfskip","Dec Fskip");
	MAPPER_AddHandler(IncreaseFrameSkip,MK_f8,MMOD1,"incfskip","Inc Fskip");
	GFX_SetTitle(-1,render.frameskip.max,false);
#endif
}

#include <dbp_serialize.h>

void DBPSerialize_Render(DBPArchive& ar)
{
	extern Bit8u* GFX_GetPixels(Bitu& pitch);
	Bitu current_pitch;
	Bit8u* current_pixels = GFX_GetPixels(current_pitch);
	Bit32u render_offset = (render.scale.outWrite > current_pixels && render.scale.outWrite < current_pixels + current_pitch * render.src.height ? (render.scale.outWrite - current_pixels) : 0);
	Bit8u loaded_src[sizeof(render.src)];

	ar
		.SerializeBytes((ar.mode == DBPArchive::MODE_LOAD ? loaded_src : (Bit8u*)&render.src), sizeof(render.src))
		.Serialize(render.pal)
		.Serialize(render.updating)
		.Serialize(render.active)
		.Serialize(render.scale.inLine).Serialize(render.scale.outLine);

#ifndef C_DBP_ENABLE_SCALERCACHE
	if (ar.version < 5) { Bitu old; ar.Serialize(old); }
	ar.Serialize(render_offset);
	if (ar.version >= 2 && ar.version < 5) { Bit32u old; ar.Serialize(old); }
#else
	ar.Serialize(Scaler_ChangedLineIndex)
	ar.Serialize(render_offset);
	Bit32u cache_offset = (render.scale.cacheRead - (Bit8u*)&scalerSourceCache);
	if (ar.version >= 2) ar.Serialize(cache_offset); else cache_offset = 0;
	if (ar.mode == DBPArchive::MODE_LOAD)
	{
		render.scale.clearCache = true;
		render.scale.cacheRead = (Bit8u*)&scalerSourceCache + cache_offset;
	}
#endif

	if (ar.mode == DBPArchive::MODE_LOAD)
	{
		if (memcmp(&render.src, loaded_src, sizeof(render.src)))
		{
			memcpy(&render.src, loaded_src, sizeof(render.src));
			RENDER_Reset();
		}
		else if (current_pixels && render_offset && render_offset < render.src.width * 4 * render.src.height)
		{
			render.scale.outWrite = current_pixels + render_offset;
		}
		else
		{
			RENDER_Reset();
		}
	}
}
