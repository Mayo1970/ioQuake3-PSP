/*
===========================================================================
PSP port - code/psp/psp_input.c

Session 7 (pulled forward from Session 9). sceCtrl polling translated into
modern ioquake3 events.

TWO references, and they contribute different halves:

  Quake3PSP-mirror/unix/psp_input.c  - the PSP half. sceCtrlSetSamplingCycle,
      PSP_CTRL_MODE_ANALOG, sceCtrlPeekBufferPositive, the nub deadzone, and
      the menu button layout, which is this port's menu layout too. It is
      id 1.32 though: it calls Sys_QueEvent and Sys_SendKeyEvents, neither of
      which exists in this engine, so none of its plumbing transfers.

  Ioquake3-PS3/code/input/ps3_input.c - the structural half, and the one that
      matters. Same modern ioq3 base, same problem (a console pad and no
      keyboard), already solved: Com_QueueEvent, a menu-vs-game split driven
      by Key_GetCatcher(), and default binds installed through
      Key_SetBinding rather than through the boot command line.

That last point is not a style choice. Sys_PSP_BuildBootCommandLine
(code/sys/sys_psp.c) already spends 10 of the 31 lines MAX_CONSOLE_LINES
allows, and the Wii port hit that ceiling twice. Binding through
Key_SetBinding costs zero lines.

===========================================================================
*/

#include "../client/client.h"
#include <pspctrl.h>

/*
=================================================================
Control scheme

In game, per the port owner:

  Nub          aim - yaw/pitch                   (analog, mouse-style deltas)
  Triangle     forward                           K_JOY4  -> +forward
  Cross        backward                          K_JOY1  -> +back
  Square       strafe left                       K_JOY3  -> +moveleft
  Circle       strafe right                      K_JOY2  -> +moveright
  D-pad Left   previous weapon                   K_JOY13 -> weapprev
  D-pad Right  next weapon                       K_JOY14 -> weapnext
  D-pad Down   crouch                            K_JOY15 -> +movedown
  D-pad Up     reserved, deliberately unbound    K_JOY16
  L            jump                              K_JOY5  -> +moveup
  R            attack                            K_JOY6  -> +attack
  Start        pause                             K_JOY7  -> pause
  Select       scoreboard                        K_JOY8  -> +scores

In menus and the console, Crow_bar's layout (mirror psp_input.c:333-340):

  Nub          moves the q3_ui cursor            SE_MOUSE
  D-pad        arrow keys
  Cross        K_ENTER
  Start        K_ESCAPE
  Select       chord modifier only
  L / R        K_MWHEELDOWN / K_MWHEELUP         (list scrolling)

Two deliberate deviations from the mirror there:

  - Circle is K_ESCAPE, not the mirror's K_INS. Menu_DefaultKey
    (code/q3_ui/ui_qmenu.c:1677-1733) does nothing at all with K_INS; a
    dedicated back button is the single biggest usability win available and
    it costs nothing.
  - Select is reserved as the modifier for the console chord below.
    It emits no standalone menu key.

  - Square and Triangle emit nothing in menus. The mirror maps them to K_TAB
    and K_DEL, but those come from its in-game keyboard-emulation scheme, not
    from a menu design, and neither key does anything in Menu_DefaultKey.
=================================================================
*/

#define PSP_NUM_BUTTONS		12

/* Indices into every parallel table below. */
#define PSP_BTN_CROSS		0
#define PSP_BTN_CIRCLE		1
#define PSP_BTN_SQUARE		2
#define PSP_BTN_TRIANGLE	3
#define PSP_BTN_L		4
#define PSP_BTN_R		5
#define PSP_BTN_START		6
#define PSP_BTN_SELECT		7
#define PSP_BTN_UP		8
#define PSP_BTN_DOWN		9
#define PSP_BTN_LEFT		10
#define PSP_BTN_RIGHT		11

static const unsigned int	pspButtonMask[ PSP_NUM_BUTTONS ] =
{
	PSP_CTRL_CROSS,
	PSP_CTRL_CIRCLE,
	PSP_CTRL_SQUARE,
	PSP_CTRL_TRIANGLE,
	PSP_CTRL_LTRIGGER,
	PSP_CTRL_RTRIGGER,
	PSP_CTRL_START,
	PSP_CTRL_SELECT,
	PSP_CTRL_UP,
	PSP_CTRL_DOWN,
	PSP_CTRL_LEFT,
	PSP_CTRL_RIGHT
};

static const int		pspGameKey[ PSP_NUM_BUTTONS ] =
{
	K_JOY1,		// Cross     backward
	K_JOY2,		// Circle    strafe right
	K_JOY3,		// Square    strafe left
	K_JOY4,		// Triangle  forward
	K_JOY5,		// L         jump
	K_JOY6,		// R         attack
	/*
	 Start is K_ESCAPE in game as well as in menus, and that is the "pause"
	 the control scheme asks for.

	 There is NO "pause" command in ioquake3 - binding one produced
	 "unknown cmd pause" on hardware. cl_paused is CVAR_ROM
	 (qcommon/common.c:2875) and the engine drives it: CL_Frame pauses
	 single player while a menu is up. So opening the menu IS the pause,
	 and Start toggling it is both correct and what every console port does.
	*/
	K_ESCAPE,	// Start     pause == open the menu
	K_JOY8,		// Select    scoreboard
	K_JOY16,	// D-up      reserved
	K_JOY15,	// D-down    crouch
	K_JOY13,	// D-left    weapprev
	K_JOY14		// D-right   weapnext
};

// 0 means "emit nothing for this button while a key catcher is active".
static const int		pspMenuKey[ PSP_NUM_BUTTONS ] =
{
	K_ENTER,
	K_ESCAPE,
	0,
	0,
	K_MWHEELDOWN,
	K_MWHEELUP,
	K_ESCAPE,
	0,		// Select     chord modifier only
	K_UPARROW,
	K_DOWNARROW,
	K_LEFTARROW,
	K_RIGHTARROW
};

/*
 The keycode actually emitted when this button went down, or 0 if it is up.

 Storing it, rather than recomputing the table lookup at release time, is what
 stops a key sticking down forever when the catcher changes mid-hold: press R
 in game (K_JOY6 down -> +attack), open the menu, release R. Recomputing would
 send K_MWHEELUP up and leave +attack latched on. Releasing the key that was
 actually pressed cannot get this wrong.
*/
static int			pspHeldKey[ PSP_NUM_BUTTONS ];
static unsigned int	pspChordHeldMask;

static cvar_t			*in_pspDeadzone;
static cvar_t			*in_pspMenuSpeed;
static cvar_t			*in_pspLookSpeed;

static float			pspCursorAccumX;
static float			pspCursorAccumY;
static qboolean			pspInMenuPrev;
static qboolean			pspInputReady = qfalse;

/*
===============
PSP_SetDefaultBind

Only binds a key that has no binding yet.

default.cfg is exec'd from Com_Init and a user q3config.cfg right after it,
both long before IN_Init runs (cl_main.c calls it from CL_Init). Overwriting
here would silently discard whatever the player configured last session.
===============
*/
static void PSP_SetDefaultBind( int keynum, const char *binding )
{
	const char	*existing = Key_GetBinding( keynum );

	if( !existing || !existing[ 0 ] )
		Key_SetBinding( keynum, binding );
}

/*
===============
PSP_ReleaseAll

Send a key-up for everything currently held. Called from IN_Shutdown and
IN_Restart so a pad state cannot survive a vid_restart.
===============
*/
static void PSP_ReleaseAll( void )
{
	int	i;

	for( i = 0; i < PSP_NUM_BUTTONS; i++ )
	{
		if( pspHeldKey[ i ] )
		{
			Com_QueueEvent( 0, SE_KEY, pspHeldKey[ i ], qfalse, 0, NULL );
			pspHeldKey[ i ] = 0;
		}
	}
}

/*
===============
PSP_ZeroAxes

The nub now only ever emits SE_MOUSE deltas (in menu for the cursor, in game
for aim), which are relative and never latch, so there is nothing left to
re-zero on a game <-> menu transition. Kept as a stub - and still called from
the transition point and from IN_Shutdown - so a future latched axis source
has an obvious place to hook back in.
===============
*/
static void PSP_ZeroAxes( void )
{
}

/*
===============
PSP_NubAxis

Raw nub byte (0..255, nominal centre 128) -> a signed value with the deadzone
removed and the remaining travel rescaled so full deflection is exactly
+/-PSP_AXIS_FULL.

Deadzone: PSP nub centring is sloppy enough that a resting nub reads several
units off 128 on most units, and without a deadzone the player walks on their
own. The mirror's shipping default is in_psp_tolerance 0.25 of full travel
(unix/psp_input.c:163); 20% is the same idea with a little less dead travel.

PSP_AXIS_FULL is 512 and that number is not arbitrary. CL_JoystickMove does

    forward = j_forward->value * cl.joystickAxis[j_forward_axis->integer];
    cmd->forwardmove = ClampChar( cmd->forwardmove + (int)forward );

with j_forward defaulting to -0.25 (cl_main.c:3636). ClampChar saturates at
127, so full analog travel is reached at |axis| == 127 / 0.25 == 508. Upstream's
SDL backend emits the raw +/-32767 of an SDL axis, which multiplies out to
+/-8191 and therefore saturates at about 1.5% deflection - analog in name, a
digital switch in practice. Scaling to 512 here is what makes the nub actually
analog, and it is a deliberate departure from sdl_input.c.
===============
*/
#define PSP_AXIS_FULL	512

static int PSP_NubAxis( int raw )
{
	int	dead = in_pspDeadzone ? in_pspDeadzone->integer : 26;
	int	value = raw - 128;
	int	sign = 1;
	int	range;

	if( dead < 0 )
		dead = 0;
	if( dead > 120 )
		dead = 120;

	if( value < 0 )
	{
		sign  = -1;
		value = -value;
	}

	if( value <= dead )
		return 0;

	// 127 is the largest magnitude the byte can express on the negative side.
	range = 127 - dead;
	value -= dead;

	if( value > range )
		value = range;

	return sign * ( value * PSP_AXIS_FULL ) / range;
}

/*
===============
IN_Init
===============
*/
void IN_Init( void *windowData )
{
	Com_Printf( "\n------- PSP Input Initialization -------\n" );

	/*
	 Sampling cycle 0 syncs sampling to vblank, which is what every PSP title
	 does; a non-zero value asks for a fixed microsecond period instead.
	 ANALOG mode is what makes Lx/Ly report anything at all - without it the
	 nub reads a constant 128,128 and nothing moves.
	*/
	sceCtrlSetSamplingCycle( 0 );
	sceCtrlSetSamplingMode( PSP_CTRL_MODE_ANALOG );

	Com_Memset( pspHeldKey, 0, sizeof( pspHeldKey ) );
	pspChordHeldMask = 0;

	pspCursorAccumX = 0.0f;
	pspCursorAccumY = 0.0f;
	pspInMenuPrev   = qfalse;

	in_pspDeadzone  = Cvar_Get( "in_pspDeadzone",  "26",   CVAR_ARCHIVE );
	in_pspMenuSpeed = Cvar_Get( "in_pspMenuSpeed", "5",    CVAR_ARCHIVE );
	in_pspLookSpeed = Cvar_Get( "in_pspLookSpeed", "1.0",  CVAR_ARCHIVE );

	// Move, on the four face buttons.
	PSP_SetDefaultBind( K_JOY4,  "+forward" );	// Triangle
	PSP_SetDefaultBind( K_JOY1,  "+back" );		// Cross
	PSP_SetDefaultBind( K_JOY3,  "+moveleft" );	// Square
	PSP_SetDefaultBind( K_JOY2,  "+moveright" );	// Circle

	// Shoulders.
	PSP_SetDefaultBind( K_JOY5,  "+moveup" );	// L, jump
	PSP_SetDefaultBind( K_JOY6,  "+attack" );	// R

	// Select. Start needs no bind - it emits K_ESCAPE directly.
	PSP_SetDefaultBind( K_JOY8,  "+scores" );

	// D-pad. Up is deliberately left unbound - reserved.
	PSP_SetDefaultBind( K_JOY13, "weapprev" );
	PSP_SetDefaultBind( K_JOY14, "weapnext" );
	PSP_SetDefaultBind( K_JOY15, "+movedown" );

	/*
	 The nub no longer drives movement - it drives aim, emitted as SE_MOUSE
	 deltas exactly like the menu cursor below. cl_input.c's joystick-axis
	 path (j_side_axis/j_forward_axis/j_pitch_axis/j_yaw_axis) is left at
	 engine defaults and simply unused: nothing calls Com_QueueEvent with
	 SE_JOYSTICK_AXIS from this file any more, so those cvars are inert
	 rather than fought with.
	*/

	pspInputReady = qtrue;

	Com_Printf( "nub deadzone %d/128, menu cursor speed %d\n",
		in_pspDeadzone->integer, in_pspMenuSpeed->integer );
	Com_Printf( "----------------------------------------\n" );
}

/*
===============
IN_Frame

Called from Com_Frame (qcommon/common.c:3269), once per engine frame.
===============
*/
void IN_Frame( void )
{
	SceCtrlData	pad;
	qboolean	inMenu;
	int		i;
	qboolean	triangleConsumed = qfalse;
	qboolean	selectConsumed = qfalse;

	if( !pspInputReady )
		return;

	/*
	 Peek, not Read. sceCtrlReadBufferPositive BLOCKS until a fresh sample,
	 which would pace the entire engine off the controller sampling clock -
	 IN_Frame runs inside Com_Frame, not on a thread of its own. The mirror
	 uses Peek for the same reason (unix/psp_input.c:197,289).
	*/
	sceCtrlPeekBufferPositive( &pad, 1 );

	for( i = 0; i < PSP_NUM_BUTTONS; i++ )
	{
		if( !( pad.Buttons & pspButtonMask[ i ] ) )
			pspChordHeldMask &= ~( 1u << i );
	}

	/*
	 Any catcher means "a UI owns the input": the menus, the console, the chat
	 line, and cgame's own catchers (the scoreboard, the team-order menu).
	 Q3's own precedence order is in q_shared.h:1041-1045.
	*/
	inMenu = ( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE |
	                                KEYCATCH_MESSAGE | KEYCATCH_CGAME ) ) ? qtrue : qfalse;

	if( inMenu != pspInMenuPrev )
	{
		PSP_ZeroAxes();

		pspCursorAccumX = 0.0f;
		pspCursorAccumY = 0.0f;
		pspInMenuPrev   = inMenu;
	}

	{
		qboolean	selectHeld = ( pad.Buttons & PSP_CTRL_SELECT ) ? qtrue : qfalse;
		qboolean	trianglePressed = ( ( pad.Buttons & PSP_CTRL_TRIANGLE ) &&
			!( pspChordHeldMask & ( 1u << PSP_BTN_TRIANGLE ) ) ) ? qtrue : qfalse;

		triangleConsumed = ( pspChordHeldMask & ( 1u << PSP_BTN_TRIANGLE ) ) ? qtrue : qfalse;
		selectConsumed = ( selectHeld &&
			( pspChordHeldMask & ( 1u << PSP_BTN_SELECT ) ) ) ? qtrue : qfalse;

		/* SELECT + TRIANGLE: toggle the Quake console. */
		if( selectHeld && trianglePressed )
		{
			Com_QueueEvent( 0, SE_KEY, K_CONSOLE, qtrue, 0, NULL );
			Com_QueueEvent( 0, SE_KEY, K_CONSOLE, qfalse, 0, NULL );
			triangleConsumed = qtrue;
			selectConsumed = qtrue;
			pspChordHeldMask |= ( 1u << PSP_BTN_TRIANGLE ) |
				( 1u << PSP_BTN_SELECT );
		}

		/* A chord is a modifier gesture; do not also open the scoreboard. */
		if( selectConsumed && pspHeldKey[ PSP_BTN_SELECT ] )
		{
			Com_QueueEvent( 0, SE_KEY, pspHeldKey[ PSP_BTN_SELECT ], qfalse, 0, NULL );
			pspHeldKey[ PSP_BTN_SELECT ] = 0;
		}

	for( i = 0; i < PSP_NUM_BUTTONS; i++ )
	{
		qboolean	down = ( pad.Buttons & pspButtonMask[ i ] ) ? qtrue : qfalse;

		if( down )
		{
			int	key;

			if( pspHeldKey[ i ] )
				continue;		// already down, and Q3 wants edges only

			if( ( ( pspChordHeldMask & ( 1u << i ) ) && down ) ||
				( i == PSP_BTN_TRIANGLE && triangleConsumed ) ||
				( i == PSP_BTN_SELECT && selectConsumed ) )
			{
				continue;
			}

			key = inMenu ? pspMenuKey[ i ] : pspGameKey[ i ];

			if( !key )
				continue;		// this button does nothing in this context

			pspHeldKey[ i ] = key;
			Com_QueueEvent( 0, SE_KEY, key, qtrue, 0, NULL );
		}
		else if( pspHeldKey[ i ] )
		{
			Com_QueueEvent( 0, SE_KEY, pspHeldKey[ i ], qfalse, 0, NULL );
			pspHeldKey[ i ] = 0;
		}
	}
	}

	{
		int	x = PSP_NubAxis( (int)pad.Lx );
		int	y = PSP_NubAxis( (int)pad.Ly );

		/*
		 In-game aim uses the same linear -1..1 response as the menu cursor,
		 but its SE_MOUSE output must be converted from a degrees-per-frame
		 target. CL_AdjustAngles turns a held digital look key by

		     0.001 * cls.frametime * cl_yawspeed/cl_pitchspeed

		 while CL_MouseMove turns SE_MOUSE movement by

		     m_yaw/m_pitch * sensitivity * cl.cgameSensitivity * delta

		 (with the default cl_mouseAccel of 0). The scale below compensates
		 for that mouse-look gain, so in_pspLookSpeed is a multiplier of the
		 digital look rate rather than a mouse-unit rate. Its default of 1.0
		 therefore makes full nub deflection match the default 140 degrees/sec
		 digital look rate, independently of the player's mouse sensitivity,
		 m_yaw, and m_pitch settings.
		*/
		if( inMenu )
		{
			/*
			 The q3_ui cursor is driven by mouse deltas, and SE_MOUSE carries
			 integers. A sub-pixel deflection therefore truncates to zero and
			 the cursor stutters or stalls; the fractional part has to be
			 carried into the next frame.

			 Linear response only. A squared curve - the obvious "make small
			 movements finer" idea - drops the small end below one pixel per
			 frame and the accumulator never fills, which is a bug the PS3
			 port hit and documented (ps3_input.c:667).
			*/
			float	speed = in_pspMenuSpeed->value;

			if( x || y )
			{
				int	dx, dy;

				pspCursorAccumX += ( (float)x / (float)PSP_AXIS_FULL ) * speed;
				pspCursorAccumY += ( (float)y / (float)PSP_AXIS_FULL ) * speed;

				dx = (int)pspCursorAccumX;
				dy = (int)pspCursorAccumY;

				pspCursorAccumX -= (float)dx;
				pspCursorAccumY -= (float)dy;

				if( dx || dy )
					Com_QueueEvent( 0, SE_MOUSE, dx, dy, 0, NULL );
			}
		}
		else
		{
			float	frameScale = in_pspLookSpeed->value * 0.001f *
				(float)cls.frametime;
			float	yawGain = cl_sensitivity->value * m_yaw->value *
				cl.cgameSensitivity;
			float	pitchGain = cl_sensitivity->value * m_pitch->value *
				cl.cgameSensitivity;

			if( x || y )
			{
				int	dx, dy;

				if( yawGain != 0.0f )
					pspCursorAccumX += ( (float)x / (float)PSP_AXIS_FULL ) *
						cl_yawspeed->value * frameScale / yawGain;
				if( pitchGain != 0.0f )
					pspCursorAccumY += ( (float)y / (float)PSP_AXIS_FULL ) *
						cl_pitchspeed->value * frameScale / pitchGain;

				dx = (int)pspCursorAccumX;
				dy = (int)pspCursorAccumY;

				pspCursorAccumX -= (float)dx;
				pspCursorAccumY -= (float)dy;

				if( dx || dy )
					Com_QueueEvent( 0, SE_MOUSE, dx, dy, 0, NULL );
			}
		}
	}
}

/*
===============
IN_Shutdown
===============
*/
void IN_Shutdown( void )
{
	if( !pspInputReady )
		return;

	PSP_ReleaseAll();
	PSP_ZeroAxes();

	pspInputReady = qfalse;
}

/*
===============
IN_Restart
===============
*/
void IN_Restart( void )
{
	IN_Shutdown();
	IN_Init( NULL );
}
