/*
===========================================================================
PSP port - code/psp/psp_static_world.h

Static-world cache support: classify, lazily pack, and submit eligible world
faces through the PSP GE path. The existing renderer remains the fallback.
===========================================================================
*/

#ifndef __PSP_STATIC_WORLD_H__
#define __PSP_STATIC_WORLD_H__

#include "../renderercommon/tr_common.h"

void PSP_StaticWorld_RegisterCvar( void );
void PSP_StaticWorld_ClassifySurface( const void *surface, const void *shader,
	int fogNum );
qboolean PSP_StaticWorld_DrawSurface( const void *surface, const void *shader,
	int fogNum );
void PSP_StaticWorld_Flush( void );
void PSP_StaticWorld_BindImage( const void *bundle );
void PSP_StaticWorld_Reset( void );
void PSP_StaticWorld_Report( void );

#endif
