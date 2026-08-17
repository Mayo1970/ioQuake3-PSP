/*
===========================================================================
PSP port - code/psp/psp_gamma.c

Replaces the pruned code/sdl/sdl_gamma.c. The GE has no hardware gamma
ramp path wired up yet (Session 5 territory); no-op until then.
===========================================================================
*/

#include "../renderercommon/tr_common.h"

/*
===============
GLimp_SetGamma
===============
*/
void GLimp_SetGamma( unsigned char red[256], unsigned char green[256], unsigned char blue[256] )
{
	// NOP - no hardware gamma ramp on PSP GE. Session 5.
}
