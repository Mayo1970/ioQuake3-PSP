/*
===========================================================================
PSP port - code/psp/psp_gu.h

The GU side of the renderer split. code/psp/psp_gl.h is the GL side: it
supplies the desktop-GL vocabulary code/renderercommon/qgl.h needs to
declare the ~68 qgl* entry points. This header is what the two PSP
translation units share on the way down to sceGu.

  psp_glimp.c  owns the display: VRAM buffers, sceGuInit, the display list,
               swap. It exports the state below.
  psp_qgl.c    owns the qgl* vtable: GL enums/state -> sceGu calls appended
               to the display list psp_glimp.c keeps open.

renderergl1/ is untouched upstream ioquake3 and never sees any of this.
===========================================================================
*/

#ifndef __PSP_GU_H__
#define __PSP_GU_H__

#include "../qcommon/q_shared.h"

#include <pspgu.h>
#include <pspgum.h>

// The panel. Framebuffer stride is 512 and never 480 - the visible width
// and the buffer width are different numbers on this hardware, and using
// 480 as a stride is the "garbage band down the right edge" bug.
#define PSP_SCREEN_WIDTH    480
#define PSP_SCREEN_HEIGHT   272
#define PSP_BUFFER_STRIDE   512

/*
 Bounded one-shot dump of what the shim actually hands the GE: the ortho
 the engine asked for, the first few texture uploads and binds, and the
 first few draw calls with their real vertex data.

 It exists because "the text is big and stretched" is an adjective and the
 log is this port's only observation channel. Every line is emitted once,
 within the first frames, and the counters never reset - CON_Print reopens
 the log per line (~3-8 ms), so an unbounded dump would itself change the
 behaviour being measured.

 It also carries GLimp_DebugProbe (psp_glimp.c): two rectangles drawn at
 the end of every frame, one through GU_TRANSFORM_2D with no matrices and
 one through sceGumOrtho, both untextured. That pair is what proved the
 submission and projection paths were sound when the 2D pass looked dead,
 so it is kept rather than deleted - re-enabling it is one digit.

 0 = off, which is the shipping state.
*/
#define PSP_GFX_DEBUG       0

/*
===========================================================================
Owned by psp_glimp.c
===========================================================================
*/

// qtrue once sceGuInit has run and a display list is open. Every qgl* body
// must check this: tr_init.c touches GL state before GLimp_Init returns,
// and appending to a list that does not exist yet corrupts the GE.
extern qboolean	pspGuReady;

// psp_qgl.c suppresses redundant state words in the open display list. The
// cache must be invalidated whenever sceGuInit recreates the GU state.
void PSP_QGL_ResetStateCache( void );

/*
===========================================================================
Owned by code/psp/con_psp.c

pspDebugScreen renders straight into VRAM at offset 0, which is inside the
range vramalloc hands out. Once the GU owns the display the two fight over
the same memory, so CON_Print stops calling pspDebugScreenPrintf and writes
only to the Memory Stick log - the channel that actually reports hardware
runs (see HEAP-HANDOFF.md).
===========================================================================
*/
void CON_SetDisplayOwnedByGU( qboolean owned );

#endif // __PSP_GU_H__
