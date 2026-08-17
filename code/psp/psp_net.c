/*
===========================================================================
PSP port - code/psp/psp_net.c        Session 9: LAN and internet play

Replaces qcommon/net_ip.c for PSP (swapped into COMMON_SOURCES by
cmake/shared_sources.cmake). net_ip.c is unusable here: it wants net/if.h,
ifaddrs, getaddrinfo/getnameinfo and IPv6, none of which PSPSDK has. This
file is the IPv4-only equivalent written against sceNetInet / sceNetApctl /
sceNetResolver.

WHY sceNetInet* AND NOT THE BSD NAMES
libcglue.a does ship socket/bind/recvfrom/sendto/select/gethostbyname over
sceNetInet* - Quake3PSP-mirror's unix/unix_net.c is written against exactly
those. They are avoided here for two measured reasons:
  - libcglue routes sockets through its own __descriptormap fd table, the
    same table psp_file.c is already rationing against the console's ~10
    open-handle ceiling.
  - libcglue's select.o pulls in clock() and sceKernelDelayThread, i.e. it
    polls in a loop. sceNetInetSelect takes a real timeout, and NET_Sleep
    must not spin: the audio thread runs at priority 0x12 and starving it
    is audible (see psp_snd.c's underrun counter).

WHY NOT pspSdkInetInit()
It lives in libpspsdk.a, which psp-gcc's own spec places AFTER anything in
COMMON_LIBRARIES ("psp-gcc -dumpspecs", *lib:). Its undefined sceNetInit /
sceNetApctlInit would then need libpspnet / libpspnet_apctl scanned later
still, and they are not. The four init calls are made directly from this
file instead - this object is first in link order, so every archive that
follows resolves them.

RECEIVE HAS ONE ENTRY POINT
NET_GetPacket is file-static in net_ip.c and is not declared in qcommon.h.
Everything funnels through NET_Sleep -> NET_Event -> Com_RunAndTimeServerPacket
or CL_PacketEvent, called from Com_Frame (qcommon/common.c:3274-3276). This
file owns all three.

Sys_SendPacket never sees NA_LOOPBACK: NET_SendPacket (qcommon/net_chan.c)
routes loopback to NET_SendLoopPacket before ever reaching the system layer.

HARD PLATFORM CONSTRAINT, DOCUMENT IT RATHER THAN DEBUG IT
The PSP radio is 802.11b with WEP or WPA-PSK (TKIP) only. No WPA2, no WPA3.
A modern router must expose a legacy 2.4 GHz SSID or the console cannot
associate at all.
===========================================================================
*/

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../sys/sys_local.h"

#include <string.h>
#include <stdlib.h>

#include <pspkernel.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_netparam.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspwlan.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define PSP_NET_INVALID_SOCKET	(-1)

/* PSPSDK's netinet/in.h defines INADDR_ANY and INADDR_BROADCAST but not
   INADDR_NONE. sceNetInetInetAddr follows BSD and returns all-ones on a
   malformed dotted quad. */
#ifndef INADDR_NONE
#define INADDR_NONE				( (unsigned int)0xffffffff )
#endif

/* sceNetInit pool. The mirror uses 0x20000 (Makefile-era pspSdkInetInit
   defaults); that is enough for a single UDP socket plus the resolver. */
#define PSP_NET_POOLSIZE		0x20000

/*
 THREAD PRIORITY - 0x18, NOT pspSdkInetInit's 0x20. Read this before changing it.

 sceNetInit spawns two threads: "callout" drives the inet stack's timers, and
 "intr" walks received frames up from the WLAN driver into the socket receive
 queue. A packet sitting at the radio is NOT visible to sceNetInetSelect until
 the intr thread has run.

 pspSdkInetInit creates both at 0x20 - which is exactly the main thread's
 priority (psp_snd.c:40). The PSP scheduler is strict-priority and does not
 rotate equal-priority threads on its own; that is what sceKernelRotateThread-
 ReadyQueue exists for. So two threads at 0x20 only take turns when the running
 one blocks.

 That was harmless until Session 11d. Before it, GLimp_EndFrame's unconditional
 sceDisplayWaitVblankStart blocked the main thread for 7-10 ms of every frame
 and the stack ran in that window. Session 11d made the wait conditional on the
 engine running ahead of the panel, which below 59.94 fps is never - and every
 other block in the gameplay loop is already ~0 (geSync 0 ms, NET_Sleep gets
 msec 0 and selects with a zero timeout, psp_input.c uses Peek not Read). The
 loop stopped yielding, and the stack stopped being scheduled except by
 accident: high ping, multi-second receive gaps, constant "Connection
 Interrupted".

 0x18 puts both threads ABOVE the main thread and below audio's 0x12. They are
 event-driven - callout on a kernel alarm, intr on the WLAN driver - so they
 preempt the game loop the moment they have work and hand the CPU straight
 back, costing the microseconds they actually need rather than a frame. This is
 the same argument psp_snd.c already made for putting audio at 0x12, applied to
 the other latency-sensitive thread the port owns.

 Final order: audio 0x12 -> net 0x18 -> main 0x20 -> apctl 0x42 (association
 only; the connect loop blocks in sceKernelDelayThread throughout, so it does
 not need to preempt anything).

 Stacks go 0x1000 -> 0x2000. Nothing measured demanded it; the threads are now
 preempting a 512 KB-stack main thread and the extra 8 KB is not worth being
 uncertain about.
*/
#define PSP_NET_CALLOUT_PRIO	0x18
#define PSP_NET_CALLOUT_STACK	0x2000
#define PSP_NET_INTR_PRIO		0x18
#define PSP_NET_INTR_STACK		0x2000

/*
 Socket receive space. The socket is drained once per Com_Frame, i.e. every
 60-100 ms at this port's framerate, so it has to hold several frames of burst
 rather than a steady trickle.

 The worst burst is NOT gameplay - it is the server browser. A master server's
 getserversResponse carries up to MAX_SERVERSPERPACKET (256) servers per packet
 (cl_main.c:2493) across several packets back to back, and CL_UpdateVisiblePings
 then has up to MAX_PINGREQUESTS (32) getinfo replies in flight at once.
 Gameplay's worst is ~20 snapshots/s of up to MAX_MSGLEN, fragmented into
 1400-byte pieces by net_chan.c.

 Fragmentation is why an overflow costs more than the bytes lost: Netchan_Process
 discards the WHOLE message when a fragment is missing, so one dropped fragment
 kills an entire snapshot, and an overflow during a burst kills every snapshot in
 it. net_ip.c never sets this because a desktop drains the socket at 125 fps.
*/
#define PSP_NET_RCVBUF			( 64 * 1024 )
#define PSP_NET_SNDBUF			( 32 * 1024 )

/* sceNetApctlConnect is asynchronous; poll to GOT_IP. The mirror allows
   50 ms x 200 = 10 s (unix/unix_net.c:523). 15 s here because that budget
   has to cover a WPA key exchange AND a DHCP lease on an 802.11b link, and
   the cost of being wrong is a failed association rather than a slow one. */
#define PSP_APCTL_POLL_USEC		( 50 * 1000 )
#define PSP_APCTL_POLL_MAX		300

/* How many stored network profiles to probe when net_apctl is 0 (auto).
   The PSP's own Settings UI numbers connections from 1, but the slots are
   persistent: deleting and recreating connections leaves the surviving one
   at an index above 1. */
#define PSP_APCTL_MAX_PROFILE	10

/* Resolver working buffer. 1024 is the size every pspsdk sample uses. */
#define PSP_RESOLVER_BUFSIZE	1024
#define PSP_RESOLVER_TIMEOUT	2		/* seconds per try */
#define PSP_RESOLVER_RETRIES	3

static cvar_t	*net_enabled;
static cvar_t	*net_ip;
static cvar_t	*net_port;
static cvar_t	*net_apctl;
static cvar_t	*net_dropsim;

static int		ip_socket = PSP_NET_INVALID_SOCKET;

static qboolean	networkingEnabled = qfalse;
static qboolean	net_modulesLoaded = qfalse;	/* sceUtilityLoadNetModule done */
static qboolean	net_stackUp       = qfalse;	/* sceNetInit .. sceNetApctlInit done */
static qboolean	net_apConnected   = qfalse;	/* sceNetApctlConnect reached GOT_IP */

/* Filled from sceNetApctlGetInfo once associated; drives Sys_IsLANAddress. */
static qboolean	net_haveLocalIP = qfalse;
static byte		net_localIP[ 4 ];
static byte		net_localMask[ 4 ];

/*
   Association is deliberately driven by NET_Sleep rather than a second
   engine thread.  sceNetApctlConnect already starts the firmware state
   machine asynchronously; the old code made it synchronous only by polling
   here in NET_Init.  Keeping the pump on the main thread avoids races with
   cvars, Com_Printf, the socket handle, and shutdown while still letting the
   menu render immediately.
 */
typedef enum {
	PSP_NET_ASSOC_IDLE,
	PSP_NET_ASSOC_CONNECTING,
	PSP_NET_ASSOC_DISCONNECTING,
	PSP_NET_ASSOC_READY,
	PSP_NET_ASSOC_FAILED
} pspNetAssocState_t;

static pspNetAssocState_t net_assocState = PSP_NET_ASSOC_IDLE;
static int		net_assocPreferred;
static int		net_assocNextProfile;
static int		net_assocProfile;
static int		net_assocLastState;
static int		net_assocPolls;
static unsigned int	net_assocNextPollUs;
static qboolean	net_assocPreferredTried;
static qboolean	net_assocRetryAfterDisconnect;

static void NET_PSP_AssociationPump( void );

/*
===========================================================================
NET_ErrorString

sceNetInetGetErrno returns BSD-shaped errno values, so newlib's strerror
is meaningful. Note this is NOT the C library errno - sceNetInet* does not
touch that, which is exactly why net_ip.c's "socketError" macro cannot be
reused as-is.
===========================================================================
*/
static char *NET_ErrorString( void )
{
	return strerror( sceNetInetGetErrno() );
}

/*
===========================================================================
NetadrToSockadr / SockadrToNetadr

net_ip.c:219-259 with every IPv6 branch removed.
===========================================================================
*/
static void NetadrToSockadr( netadr_t *a, struct sockaddr_in *s )
{
	memset( s, 0, sizeof( *s ) );

	s->sin_family = AF_INET;
	s->sin_port   = a->port;

	if( a->type == NA_BROADCAST )
		s->sin_addr.s_addr = INADDR_BROADCAST;
	else
		memcpy( &s->sin_addr.s_addr, a->ip, 4 );
}

static void SockadrToNetadr( struct sockaddr_in *s, netadr_t *a )
{
	memset( a, 0, sizeof( *a ) );

	a->type = NA_IP;
	memcpy( a->ip, &s->sin_addr.s_addr, 4 );
	a->port = s->sin_port;
}

/*
==================
NET_CompareBaseAdrMask / NET_CompareBaseAdr / NET_CompareAdr / NET_IsLocalAddress

Pure struct comparison, no sockets involved - same logic as net_ip.c.
==================
*/
qboolean NET_CompareBaseAdrMask( netadr_t a, netadr_t b, int netmask )
{
	byte cmpmask, *addra, *addrb;
	int curbyte;

	if( a.type != b.type )
		return qfalse;

	if( a.type == NA_LOOPBACK )
		return qtrue;

	if( a.type == NA_IP )
	{
		addra = (byte *)&a.ip;
		addrb = (byte *)&b.ip;

		if( netmask < 0 || netmask > 32 )
			netmask = 32;
	}
	else if( a.type == NA_IP6 )
	{
		addra = (byte *)&a.ip6;
		addrb = (byte *)&b.ip6;

		if( netmask < 0 || netmask > 128 )
			netmask = 128;
	}
	else
	{
		Com_Printf( "NET_CompareBaseAdr: bad address type\n" );
		return qfalse;
	}

	curbyte = netmask >> 3;

	if( curbyte && memcmp( addra, addrb, curbyte ) )
		return qfalse;

	netmask &= 0x07;
	if( netmask )
	{
		cmpmask = (byte)( ( 1 << netmask ) - 1 );
		cmpmask <<= 8 - netmask;

		if( ( addra[ curbyte ] & cmpmask ) == ( addrb[ curbyte ] & cmpmask ) )
			return qtrue;
	}
	else
		return qtrue;

	return qfalse;
}

qboolean NET_CompareBaseAdr( netadr_t a, netadr_t b )
{
	return NET_CompareBaseAdrMask( a, b, -1 );
}

qboolean NET_CompareAdr( netadr_t a, netadr_t b )
{
	if( !NET_CompareBaseAdr( a, b ) )
		return qfalse;

	if( a.type == NA_IP || a.type == NA_IP6 )
		return ( a.port == b.port ) ? qtrue : qfalse;

	return qtrue;
}

qboolean NET_IsLocalAddress( netadr_t adr )
{
	return adr.type == NA_LOOPBACK;
}

/*
==================
NET_AdrToString / NET_AdrToStringwPort
==================
*/
const char *NET_AdrToString( netadr_t a )
{
	static char s[ NET_ADDRSTRMAXLEN ];

	if( a.type == NA_LOOPBACK )
		Com_sprintf( s, sizeof( s ), "loopback" );
	else if( a.type == NA_BOT )
		Com_sprintf( s, sizeof( s ), "bot" );
	else if( a.type == NA_IP || a.type == NA_BROADCAST )
		Com_sprintf( s, sizeof( s ), "%d.%d.%d.%d", a.ip[ 0 ], a.ip[ 1 ], a.ip[ 2 ], a.ip[ 3 ] );
	else
		Com_sprintf( s, sizeof( s ), "unknown" );

	return s;
}

const char *NET_AdrToStringwPort( netadr_t a )
{
	static char s[ NET_ADDRSTRMAXLEN ];

	if( a.type == NA_LOOPBACK )
		Com_sprintf( s, sizeof( s ), "loopback" );
	else if( a.type == NA_BOT )
		Com_sprintf( s, sizeof( s ), "bot" );
	else if( a.type == NA_IP || a.type == NA_BROADCAST )
		Com_sprintf( s, sizeof( s ), "%s:%hu", NET_AdrToString( a ), BigShort( a.port ) );
	else
		Com_sprintf( s, sizeof( s ), "unknown" );

	return s;
}

/*
===========================================================================
Sys_StringToAdr

Bare host or dotted quad only - NET_StringToAdr (qcommon/net_chan.c:629)
has already split off any ":port" suffix and handles "localhost" itself
before this is ever called.

NA_IP6 is refused rather than faked: there is no IPv6 on this stack, and
returning an IPv4 address for an NA_IP6 request would put a wrong-family
address into cls.updateServer / the master server list.
===========================================================================
*/
qboolean Sys_StringToAdr( const char *s, netadr_t *a, netadrtype_t family )
{
	struct sockaddr_in	sadr;
	struct in_addr		resolved;
	unsigned int		addr;
	int					rid, rc;
	char				*buf;

	if( family == NA_IP6 )
		return qfalse;

	memset( &sadr, 0, sizeof( sadr ) );
	sadr.sin_family = AF_INET;
	sadr.sin_port   = 0;

	if( s[ 0 ] >= '0' && s[ 0 ] <= '9' )
	{
		/* arpa/inet.h:18 #defines inet_addr straight to sceNetInetInetAddr. */
		addr = (unsigned int)inet_addr( s );

		if( addr == INADDR_NONE )
			return qfalse;

		sadr.sin_addr.s_addr = addr;
		SockadrToNetadr( &sadr, a );
		return qtrue;
	}

	/* Name lookup. Needs the stack up - sceNetResolverInit runs in NET_Init. */
	if( !net_stackUp )
	{
		Com_Printf( "Sys_StringToAdr: can't resolve %s, network stack is down\n", s );
		return qfalse;
	}

	buf = malloc( PSP_RESOLVER_BUFSIZE );
	if( !buf )
		return qfalse;

	rc = sceNetResolverCreate( &rid, buf, PSP_RESOLVER_BUFSIZE );
	if( rc < 0 )
	{
		Com_Printf( "Sys_StringToAdr: sceNetResolverCreate failed (0x%08X)\n", rc );
		free( buf );
		return qfalse;
	}

	rc = sceNetResolverStartNtoA( rid, s, &resolved,
		PSP_RESOLVER_TIMEOUT, PSP_RESOLVER_RETRIES );

	sceNetResolverDelete( rid );
	free( buf );

	if( rc < 0 )
	{
		Com_Printf( "Sys_StringToAdr: couldn't resolve %s (0x%08X)\n", s, rc );
		return qfalse;
	}

	sadr.sin_addr.s_addr = resolved.s_addr;
	SockadrToNetadr( &sadr, a );

	return qtrue;
}

/*
===========================================================================
Sys_IsLANAddress

LAN clients have their rate cvar ignored, so a wrong answer here costs
bandwidth on a link that has very little of it (802.11b, ~5 Mbit shared).

net_ip.c walks every interface's address and netmask. There is exactly one
interface here and apctl hands over both, so this is a straight masked
compare with an RFC1918 fallback for the case where the mask never arrived.

Deliberately NOT the mirror's version (unix/unix_net.c:351-392): that one
resolves its own hostname via gethostbyname and then writes localIP[numIP]
*after* incrementing numIP, so it stores the address one slot high and
leaves slot 0 zeroed.
===========================================================================
*/
qboolean Sys_IsLANAddress( netadr_t adr )
{
	int i;

	if( adr.type == NA_LOOPBACK )
		return qtrue;

	if( adr.type != NA_IP )
		return qfalse;

	if( net_haveLocalIP )
	{
		for( i = 0; i < 4; i++ )
		{
			if( ( adr.ip[ i ] & net_localMask[ i ] ) != ( net_localIP[ i ] & net_localMask[ i ] ) )
				break;
		}

		if( i == 4 )
			return qtrue;
	}

	/* RFC1918 private ranges: 10/8, 172.16/12, 192.168/16. */
	if( adr.ip[ 0 ] == 10 )
		return qtrue;
	if( adr.ip[ 0 ] == 172 && ( adr.ip[ 1 ] & 0xf0 ) == 16 )
		return qtrue;
	if( adr.ip[ 0 ] == 192 && adr.ip[ 1 ] == 168 )
		return qtrue;

	return qfalse;
}

/*
===========================================================================
NET_GetLocalAddress

sceNetApctlGetInfo fills a union, so IP and netmask need one call each.
Strings, not binary - "192.168.1.7" style.
===========================================================================
*/
static void NET_GetLocalAddress( void )
{
	union SceNetApctlInfo	info;
	unsigned int			v;

	net_haveLocalIP = qfalse;
	memset( net_localIP,   0, sizeof( net_localIP ) );
	memset( net_localMask, 0, sizeof( net_localMask ) );

	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_IP, &info ) != 0 )
		return;

	v = (unsigned int)inet_addr( info.ip );
	if( v == INADDR_NONE )
		return;

	memcpy( net_localIP, &v, 4 );
	net_haveLocalIP = qtrue;

	/* A missing mask leaves net_localMask zeroed, which makes the masked
	   compare above match everything - so fall back to /24 instead. */
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_SUBNETMASK, &info ) == 0 )
	{
		v = (unsigned int)inet_addr( info.subNetMask );

		if( v != INADDR_NONE && v != 0 )
		{
			memcpy( net_localMask, &v, 4 );
			return;
		}
	}

	net_localMask[ 0 ] = 255;
	net_localMask[ 1 ] = 255;
	net_localMask[ 2 ] = 255;
	net_localMask[ 3 ] = 0;
}

/*
===========================================================================
NET_PSP_IsSocketOpen

Whether the engine currently has a UDP socket bound - i.e. whether anything
at all (server browser, LAN scan, or a live game) needs the network stack to
be serviced promptly.

Exported for code/psp/psp_glimp.c, which uses it to decide whether the frame
still owes the stack a guaranteed yield. Declared in psp_platform.h so the
renderer does not have to include qcommon's networking prototypes.
===========================================================================
*/
int NET_PSP_IsSocketOpen( void )
{
	return ( ip_socket != PSP_NET_INVALID_SOCKET ) ? 1 : 0;
}

/*
===========================================================================
NET_PSP_ReportPowerSave

The PSP's WLAN power-save setting (Settings -> Power Save Settings) makes the
radio sleep between beacons and collect frames with PS-Poll. That adds up to a
beacon interval - typically 100 ms - to every receive, plus loss, and it is
the single largest thing affecting this port's ping that is NOT in this port.

It cannot be changed from user mode (sceUtilitySetSystemParamInt is not a
user-mode call for this id), so this only reads and reports it. Reporting it
is still worth a line: without it, it is an invisible variable in every
latency measurement anyone makes from here on.
===========================================================================
*/
static void NET_PSP_ReportPowerSave( void )
{
	int value = -1;

	if( sceUtilityGetSystemParamInt( PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE, &value ) != 0 )
		return;

	if( value == PSP_SYSTEMPARAM_WLAN_POWERSAVE_ON )
		Com_Printf( "PSP net: WLAN power save is ON - expect ~100 ms of extra "
			"latency and packet loss.\n"
			"Settings -> Power Save Settings -> WLAN Power Save -> Off.\n" );
	else
		Com_Printf( "PSP net: WLAN power save off.\n" );
}

/*
===========================================================================
Sys_ShowIP
===========================================================================
*/
void Sys_ShowIP( void )
{
	union SceNetApctlInfo info;

	if( !net_apConnected )
	{
		Com_Printf( "PSP: not associated with an access point\n" );
		return;
	}

	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_IP, &info ) == 0 )
		Com_Printf( "IP:      %s\n", info.ip );
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_SUBNETMASK, &info ) == 0 )
		Com_Printf( "Netmask: %s\n", info.subNetMask );
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_GATEWAY, &info ) == 0 )
		Com_Printf( "Gateway: %s\n", info.gateway );
	if( sceNetApctlGetInfo( PSP_NET_APCTL_INFO_PRIMDNS, &info ) == 0 )
		Com_Printf( "DNS:     %s\n", info.primaryDns );
}

/*
===========================================================================
Sys_SendPacket

NA_LOOPBACK never reaches here - net_chan.c:NET_SendPacket routes it to
NET_SendLoopPacket first.
===========================================================================
*/
void Sys_SendPacket( int length, const void *data, netadr_t to )
{
	struct sockaddr_in	addr;
	int					ret, err;

	if( to.type != NA_BROADCAST && to.type != NA_IP )
	{
		/* NA_IP6 / NA_MULTICAST6 can still be handed to us by the server
		   browser code paths; drop them rather than Com_Error out of a
		   frame, which is what net_ip.c does for a genuinely bad type. */
		if( to.type == NA_IP6 || to.type == NA_MULTICAST6 )
			return;

		Com_Error( ERR_FATAL, "Sys_SendPacket: bad address type" );
		return;
	}

	if( ip_socket == PSP_NET_INVALID_SOCKET )
		return;

	NetadrToSockadr( &to, &addr );

	ret = (int)sceNetInetSendto( ip_socket, data, length, 0,
		(struct sockaddr *)&addr, sizeof( addr ) );

	if( ret < 0 )
	{
		err = sceNetInetGetErrno();

		/* Would-block is silent, and some links refuse broadcasts. */
		if( err == EAGAIN || err == EWOULDBLOCK )
			return;
		if( err == EADDRNOTAVAIL && to.type == NA_BROADCAST )
			return;

		Com_Printf( "Sys_SendPacket: %s\n", NET_ErrorString() );
	}
}

/*
===========================================================================
NET_GetPacket

One non-blocking read. The socket carries SO_NONBLOCK, so an empty queue
returns EAGAIN rather than parking the main thread.
===========================================================================
*/
static qboolean NET_GetPacket( netadr_t *net_from, msg_t *net_message )
{
	struct sockaddr_in	from;
	socklen_t			fromlen;
	int					ret, err;

	if( ip_socket == PSP_NET_INVALID_SOCKET )
		return qfalse;

	fromlen = sizeof( from );

	ret = (int)sceNetInetRecvfrom( ip_socket, net_message->data, net_message->maxsize, 0,
		(struct sockaddr *)&from, &fromlen );

	if( ret < 0 )
	{
		err = sceNetInetGetErrno();

		if( err != EAGAIN && err != EWOULDBLOCK && err != ECONNRESET && err != ECONNREFUSED )
			Com_Printf( "NET_GetPacket: %s\n", NET_ErrorString() );

		return qfalse;
	}

	SockadrToNetadr( &from, net_from );
	net_message->readcount = 0;

	if( ret >= net_message->maxsize )
	{
		Com_Printf( "Oversize packet from %s\n", NET_AdrToString( *net_from ) );
		return qfalse;
	}

	net_message->cursize = ret;

	return qtrue;
}

/*
===========================================================================
NET_Event

Drain the socket and dispatch. Same body as net_ip.c:1626, minus the fd_set
argument - there is one socket, so "which one fired" is not a question.
===========================================================================
*/
static void NET_Event( void )
{
	byte		bufData[ MAX_MSGLEN + 1 ];
	netadr_t	from;
	msg_t		netmsg;

	while( 1 )
	{
		memset( &from, 0, sizeof( from ) );
		MSG_Init( &netmsg, bufData, sizeof( bufData ) );

		if( !NET_GetPacket( &from, &netmsg ) )
			break;

		if( net_dropsim && net_dropsim->value > 0.0f && net_dropsim->value <= 100.0f )
		{
			if( rand() < (int)( ( (float)RAND_MAX ) / 100.0f * net_dropsim->value ) )
				continue;
		}

		if( com_sv_running->integer )
			Com_RunAndTimeServerPacket( &from, &netmsg );
		else
			CL_PacketEvent( from, &netmsg );
	}
}

/*
===========================================================================
NET_Sleep

Com_Frame calls this every iteration of its pacing loop (common.c:3274-3276),
with msec 0 whenever a frame already overran - which at this port's current
framerate is most of the time.

Blocking in sceNetInetSelect is what makes the msec > 0 case cheap: the
kernel reschedules and the audio thread at priority 0x12 keeps its slot.
Spinning here would be audible. With no socket there is nothing to select
on, so fall back to a plain delay rather than returning immediately and
letting Com_Frame busy-wait.
===========================================================================
*/
void NET_Sleep( int msec )
{
	struct SceNetInetTimeval	timeout;
	fd_set						fdr;
	int							retval;
#ifdef PSP_FILE_TRACE
	unsigned int				traceStart;
#endif

	if( msec < 0 )
		msec = 0;

	/* Advance association without blocking the first frame/menu. */
	NET_PSP_AssociationPump();

#ifdef PSP_FILE_TRACE
	traceStart = Sys_PSP_RenderProfileNow();
#endif
	if( ip_socket == PSP_NET_INVALID_SOCKET )
	{
		if( msec > 0 )
			sceKernelDelayThread( (SceUInt)msec * 1000 );
#ifdef PSP_FILE_TRACE
		Sys_PSP_FileTraceEvent( PSP_TRACE_NET_DELAY,
			Sys_PSP_RenderProfileNow() - traceStart, (unsigned int)msec );
#endif
		return;
	}

	FD_ZERO( &fdr );
	FD_SET( ip_socket, &fdr );

	timeout.tv_sec  = (uint32_t)( msec / 1000 );
	timeout.tv_usec = (uint32_t)( ( msec % 1000 ) * 1000 );

	retval = sceNetInetSelect( ip_socket + 1, &fdr, NULL, NULL, &timeout );
#ifdef PSP_FILE_TRACE
	Sys_PSP_FileTraceEvent( PSP_TRACE_NET_SELECT,
		Sys_PSP_RenderProfileNow() - traceStart, (unsigned int)msec );
#endif

	if( retval < 0 )
		Com_Printf( "Warning: sceNetInetSelect failed: %s\n", NET_ErrorString() );
	else if( retval > 0 )
		NET_Event();
}

/*
===========================================================================
NET_IPSocket

SO_NONBLOCK is a PSP socket option (sys/socket.h:113), not the fcntl dance
net_ip.c does. SO_BROADCAST is what makes the LAN server scan work at all -
CL_LocalServers pings 255.255.255.255 on PORT_SERVER..+3.
===========================================================================
*/
static int NET_IPSocket( const char *net_interface, int port, int *err )
{
	struct sockaddr_in	address;
	int					newsocket;
	int					i = 1;

	*err = 0;

	Com_Printf( "Opening IP socket: %s:%i\n",
		( net_interface && net_interface[ 0 ] ) ? net_interface : "0.0.0.0", port );

	newsocket = sceNetInetSocket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
	if( newsocket < 0 )
	{
		*err = sceNetInetGetErrno();
		Com_Printf( "WARNING: sceNetInetSocket: %s\n", NET_ErrorString() );
		return PSP_NET_INVALID_SOCKET;
	}

	if( sceNetInetSetsockopt( newsocket, SOL_SOCKET, SO_NONBLOCK, &i, sizeof( i ) ) < 0 )
	{
		*err = sceNetInetGetErrno();
		Com_Printf( "WARNING: setsockopt SO_NONBLOCK: %s\n", NET_ErrorString() );
		sceNetInetClose( newsocket );
		return PSP_NET_INVALID_SOCKET;
	}

	if( sceNetInetSetsockopt( newsocket, SOL_SOCKET, SO_BROADCAST, &i, sizeof( i ) ) < 0 )
	{
		*err = sceNetInetGetErrno();
		Com_Printf( "WARNING: setsockopt SO_BROADCAST: %s\n", NET_ErrorString() );
		sceNetInetClose( newsocket );
		return PSP_NET_INVALID_SOCKET;
	}

	/*
	 Buffer sizes are advisory - the stack is free to clamp them, and a refusal
	 is not fatal, it just means bursts have less room. Warn and carry on rather
	 than failing the socket over a tuning hint. See PSP_NET_RCVBUF above for
	 why the browser, not gameplay, sets the size.
	*/
	i = PSP_NET_RCVBUF;
	if( sceNetInetSetsockopt( newsocket, SOL_SOCKET, SO_RCVBUF, &i, sizeof( i ) ) < 0 )
		Com_Printf( "WARNING: setsockopt SO_RCVBUF %d: %s\n", i, NET_ErrorString() );

	i = PSP_NET_SNDBUF;
	if( sceNetInetSetsockopt( newsocket, SOL_SOCKET, SO_SNDBUF, &i, sizeof( i ) ) < 0 )
		Com_Printf( "WARNING: setsockopt SO_SNDBUF %d: %s\n", i, NET_ErrorString() );

	memset( &address, 0, sizeof( address ) );
	address.sin_family = AF_INET;

	if( !net_interface || !net_interface[ 0 ] ||
		!Q_stricmp( net_interface, "localhost" ) || !Q_stricmp( net_interface, "0.0.0.0" ) )
	{
		address.sin_addr.s_addr = INADDR_ANY;
	}
	else
	{
		unsigned int v = (unsigned int)inet_addr( net_interface );

		if( v == INADDR_NONE )
			address.sin_addr.s_addr = INADDR_ANY;
		else
			address.sin_addr.s_addr = v;
	}

	address.sin_port = ( port == PORT_ANY ) ? 0 : htons( (short)port );

	if( sceNetInetBind( newsocket, (struct sockaddr *)&address, sizeof( address ) ) < 0 )
	{
		*err = sceNetInetGetErrno();
		Com_Printf( "WARNING: bind: %s\n", NET_ErrorString() );
		sceNetInetClose( newsocket );
		return PSP_NET_INVALID_SOCKET;
	}

	return newsocket;
}

/*
===========================================================================
NET_OpenIP

Port walk exactly as net_ip.c:1357 - PORT_SERVER first, then nine
alternatives, so a second instance on the LAN does not fail outright.
===========================================================================
*/
static void NET_OpenIP( void )
{
	int port = net_port->integer;
	int err  = 0;
	int i;

	for( i = 0; i < 10; i++ )
	{
		ip_socket = NET_IPSocket( net_ip->string, port + i, &err );

		if( ip_socket != PSP_NET_INVALID_SOCKET )
		{
			Cvar_SetValue( "net_port", port + i );

			/*
			 Clear ->modified for the same reason the association path clears
			 net_apctl's: NET_GetCvars counts it, and an unconsumed flag makes
			 the NEXT NET_Config believe the configuration changed. On a desktop
			 that costs a rebind; here the stop branch also runs
			 NET_PSP_Disconnect, tearing down the AP association and
			 re-associating - seconds of dead link, mid-session, which is
			 indistinguishable from the fault this session is fixing.

			 Cvar_SetValue -> Cvar_Set -> Cvar_Set2(force=qtrue) sets modified
			 even on a CVAR_LATCH cvar, so this is not hypothetical.
			*/
			net_port->modified = qfalse;

			NET_GetLocalAddress();
			Sys_ShowIP();
			return;
		}
	}

	Com_Printf( "WARNING: Couldn't bind to a v4 ip address.\n" );
}

/*
===========================================================================
NET_PSP_SurveyProfiles

Pure diagnostics: dump what the netparam store says about every slot.

Two hardware runs have now disagreed with this store. q3psp10.log:
sceUtilityCheckNetParam(1) rejected profile 1 on a console whose stored
profile works (its browser associates with it). q3psp11.log, after that
check was demoted to advisory: sceNetApctlConnect(1) itself returned
0x80110601 == PSP_NETPARAM_ERROR_BAD_NETCONF, so slot 1 really is empty,
and the two APIs agree after all.

That leaves one question - which slot is the profile in - and this answers
it in the log rather than by guesswork. Slot 0 is included because the
header documents it as "valid but seems to be a copy of the last config
requested" (psputility_netparam.h:66): if 0 reports a name and 1..n do not,
the store is readable and the indexing assumption is what is wrong.
===========================================================================
*/
static void NET_PSP_SurveyProfiles( void )
{
	netData	nd;
	int		i, chk, got;

	Com_Printf( "PSP net: netparam survey (check/name):\n" );

	for( i = 0; i <= PSP_APCTL_MAX_PROFILE; i++ )
	{
		chk = sceUtilityCheckNetParam( i );

		memset( &nd, 0, sizeof( nd ) );
		nd.asString[ sizeof( nd.asString ) - 1 ] = '\0';
		got = sceUtilityGetNetParam( i, PSP_NETPARAM_NAME, &nd );

		if( chk != 0 && got != 0 )
			continue;	/* empty slot, and both APIs agree - say nothing */

		Com_Printf( "PSP net:   [%2d] check 0x%08X  name 0x%08X \"%s\"\n",
			i, chk, got, ( got == 0 ) ? nd.asString : "" );
	}
}

/*
===========================================================================
 NET_PSP_StateName

Translate the firmware's association enum for diagnostics. The numeric
values are not an ordered progress bar: WPA can move from KEY_EXCHANGE (6)
back to GETTING_IP (3) before reaching GOT_IP (4).
===========================================================================
*/
static const char *NET_PSP_StateName( int state )
{
	switch( state )
	{
		case PSP_NET_APCTL_STATE_DISCONNECTED:	return "disconnected";
		case PSP_NET_APCTL_STATE_SCANNING:		return "scanning";
		case PSP_NET_APCTL_STATE_JOINING:		return "joining";
		case PSP_NET_APCTL_STATE_GETTING_IP:	return "getting IP";
		case PSP_NET_APCTL_STATE_GOT_IP:		return "got IP";
		case PSP_NET_APCTL_STATE_EAP_AUTH:		return "EAP auth";
		case PSP_NET_APCTL_STATE_KEY_EXCHANGE:	return "key exchange";
		default:								return "?";
	}
}

/*
===========================================================================
NET_PSP_Disconnect

sceNetApctlDisconnect is asynchronous too, and apctl refuses a new connect
while it is still tearing the old one down - that is the 0x80410A80 every
slot after the failed one returned in q3psp12.log, not eight empty slots.
Wait for DISCONNECTED before letting the caller move on.
===========================================================================
*/
static void NET_PSP_Disconnect( void )
{
	int state, tries;

	sceNetApctlDisconnect();

	for( tries = 0; tries < PSP_APCTL_POLL_MAX; tries++ )
	{
		if( sceNetApctlGetState( &state ) != 0 )
			return;

		if( state == PSP_NET_APCTL_STATE_DISCONNECTED )
			return;

		sceKernelDelayThread( PSP_APCTL_POLL_USEC );
	}
}

/*
===========================================================================
Asynchronous association pump

Try net_apctl first if it names a profile, then every other slot.

Scanning rather than trusting one index is not defensive programming for
its own sake - q3psp11.log had slot 1 rejected outright (0x80110601), so
this console's profile is somewhere else, and the PSP's connection slots
are persistent enough that "somewhere else" is entirely normal after a
connection has been deleted and recreated.

An empty slot costs one immediate error return, so the scan is cheap: only
a slot the firmware accepts gets polled.

Note net_apctl is CVAR_ARCHIVE, so a q3config.cfg written by an earlier run
pins whatever value was in effect then. That is exactly what happened
between runs 10 and 11 - the new default of 0 never took effect because the
config still said 1. Falling through to the scan makes the stale value
harmless instead of decisive.
===========================================================================
*/
static int NET_PSP_NextAssociationProfile( void )
{
	int profile;

	if( net_assocPreferred > 0 && !net_assocPreferredTried )
	{
		net_assocPreferredTried = qtrue;
		return net_assocPreferred;
	}

	while( net_assocNextProfile <= PSP_APCTL_MAX_PROFILE )
	{
		profile = net_assocNextProfile++;
		if( profile != net_assocPreferred )
			return profile;
	}

	return 0;
}

static void NET_PSP_AssociationBeginDisconnect( qboolean retry )
{
	sceNetApctlDisconnect();
	net_assocRetryAfterDisconnect = retry;
	net_assocState = PSP_NET_ASSOC_DISCONNECTING;
	net_assocPolls = 0;
	net_assocNextPollUs = 0;
}

static qboolean NET_PSP_AssociationTryNext( void )
{
	int profile;
	int rc;

	while( ( profile = NET_PSP_NextAssociationProfile() ) != 0 )
	{
		rc = sceNetApctlConnect( profile );
		if( rc != 0 )
		{
			Com_Printf( "PSP net: sceNetApctlConnect(%d) refused (0x%08X)\n",
				profile, rc );
			continue;
		}

		net_assocProfile = profile;
		net_assocLastState = -1;
		net_assocPolls = 0;
		net_assocNextPollUs = 0;
		net_assocState = PSP_NET_ASSOC_CONNECTING;
		Com_Printf( "PSP net: connecting with profile %d (menu remains live)...\n",
			profile );
		return qtrue;
	}

	net_assocState = PSP_NET_ASSOC_FAILED;
	Com_Printf( "PSP net: no stored network profile would connect.\n"
		"Check Settings -> Network Settings has an infrastructure connection, "
		"and that the AP is 802.11b with WEP or WPA-PSK (TKIP) - not WPA2.\n" );
	return qfalse;
}

static void NET_PSP_AssociationBegin( void )
{
	if( !net_stackUp || !net_enabled->integer )
		return;

	if( net_assocState == PSP_NET_ASSOC_CONNECTING ||
		net_assocState == PSP_NET_ASSOC_DISCONNECTING ||
		net_assocState == PSP_NET_ASSOC_READY )
		return;

	net_assocPreferred = net_apctl->integer;
	net_assocNextProfile = 1;
	net_assocProfile = 0;
	net_assocLastState = -1;
	net_assocPolls = 0;
	net_assocNextPollUs = 0;
	net_assocPreferredTried = qfalse;
	net_assocRetryAfterDisconnect = qfalse;

	NET_PSP_SurveyProfiles();
	Com_Printf( "PSP net: WLAN association deferred; continuing to menu.\n" );
	NET_PSP_AssociationTryNext();
}

static void NET_PSP_AssociationPump( void )
{
	int state;
	int previousState;
	int rc;
	unsigned int now;

	if( !net_stackUp )
		return;

	if( net_assocState == PSP_NET_ASSOC_DISCONNECTING )
	{
		now = sceKernelGetSystemTimeLow();
		if( (int)( now - net_assocNextPollUs ) < 0 )
			return;

		rc = sceNetApctlGetState( &state );
		if( rc != 0 )
		{
			Com_Printf( "PSP net: disconnect state query failed (0x%08X)\n", rc );
			net_assocState = PSP_NET_ASSOC_FAILED;
			return;
		}

		if( state == PSP_NET_APCTL_STATE_DISCONNECTED )
		{
			if( net_assocRetryAfterDisconnect )
			{
				net_assocState = PSP_NET_ASSOC_IDLE;
				NET_PSP_AssociationTryNext();
			}
			else
			{
				net_assocState = PSP_NET_ASSOC_FAILED;
			}
			return;
		}

		if( ++net_assocPolls >= PSP_APCTL_POLL_MAX )
		{
			Com_Printf( "PSP net: timed out waiting for disconnect\n" );
			net_assocState = PSP_NET_ASSOC_FAILED;
			return;
		}

		net_assocNextPollUs = now + PSP_APCTL_POLL_USEC;
		return;
	}

	if( net_assocState != PSP_NET_ASSOC_CONNECTING )
		return;

	now = sceKernelGetSystemTimeLow();
	if( (int)( now - net_assocNextPollUs ) < 0 )
		return;

	rc = sceNetApctlGetState( &state );
	if( rc != 0 )
	{
		Com_Printf( "PSP net: sceNetApctlGetState failed (0x%08X)\n", rc );
		NET_PSP_AssociationBeginDisconnect( qtrue );
		return;
	}

	previousState = net_assocLastState;
	if( state != previousState )
	{
		Com_Printf( "PSP net:   state %d (%s)\n",
			state, NET_PSP_StateName( state ) );
		net_assocLastState = state;
	}

	if( state == PSP_NET_APCTL_STATE_GOT_IP )
	{
		/* Persist the profile that actually worked, but consume the cvar
		   modified bit so the next NET_Config does not tear the link down. */
		if( net_assocProfile > 0 )
		{
			Cvar_SetValue( "net_apctl", net_assocProfile );
			net_apctl->modified = qfalse;
		}
		net_apConnected = qtrue;
		net_assocState = PSP_NET_ASSOC_READY;
		Com_Printf( "PSP net: associated.\n" );
		NET_PSP_ReportPowerSave();
		NET_OpenIP();
		return;
	}

	if( state == PSP_NET_APCTL_STATE_DISCONNECTED &&
		previousState > PSP_NET_APCTL_STATE_DISCONNECTED )
	{
		Com_Printf( "PSP net: association dropped (was %s).\n",
			NET_PSP_StateName( previousState ) );
		NET_PSP_AssociationBeginDisconnect( qtrue );
		return;
	}

	if( ++net_assocPolls >= PSP_APCTL_POLL_MAX )
	{
		Com_Printf( "PSP net: timed out waiting for an IP address "
			"(last state %d, %s).\n", net_assocLastState,
			NET_PSP_StateName( net_assocLastState ) );
		NET_PSP_AssociationBeginDisconnect( qtrue );
		return;
	}

	net_assocNextPollUs = now + PSP_APCTL_POLL_USEC;
}

/*
===========================================================================
NET_GetCvars

net_ip.c's version minus IPv6, multicast and SOCKS - none of which this
stack has. net_apctl is the one addition: which of the PSP's own stored
network profiles to associate with.
===========================================================================
*/
static qboolean NET_GetCvars( void )
{
	int modified;

	net_enabled = Cvar_Get( "net_enabled", "1", CVAR_LATCH | CVAR_ARCHIVE );
	modified = net_enabled->modified;
	net_enabled->modified = qfalse;

	net_ip = Cvar_Get( "net_ip", "0.0.0.0", CVAR_LATCH );
	modified += net_ip->modified;
	net_ip->modified = qfalse;

	net_port = Cvar_Get( "net_port", va( "%i", PORT_SERVER ), CVAR_LATCH );
	modified += net_port->modified;
	net_port->modified = qfalse;

	/* 0 == scan every slot, which is the right default: the PSP's connection
	   slots are persistent, so a console whose only connection was created
	   after a delete does not have it at index 1. A non-zero value is a
	   preference, tried first, not an exclusive choice - the asynchronous
	   association pump falls through to the scan and writes back whichever
	   slot works.
	   Deliberately NOT CVAR_LATCH: that write-back has to take effect now,
	   and there is nothing here that needs a restart to change. */
	net_apctl = Cvar_Get( "net_apctl", "0", CVAR_ARCHIVE );
	modified += net_apctl->modified;
	net_apctl->modified = qfalse;

	net_dropsim = Cvar_Get( "net_dropsim", "", CVAR_TEMP );

	return modified ? qtrue : qfalse;
}

/*
===========================================================================
NET_Config

Same start/stop state machine as net_ip.c:1499. The PSP addition is that
"start" also has to bring up the radio, and "stop" has to put it back down:
holding an association costs battery and there is no reason to keep one
while the player is in single player.
===========================================================================
*/
void NET_Config( qboolean enableNetworking )
{
	qboolean	modified;
	qboolean	stop, start;

	modified = NET_GetCvars();

	if( !net_enabled->integer || !net_stackUp )
		enableNetworking = qfalse;

	if( enableNetworking == networkingEnabled && !modified )
	{
		/* A failed asynchronous attempt is retryable through net_restart, and
		   NET_Config(qtrue) is also called after a game restart. */
		if( enableNetworking && !net_apConnected &&
			( net_assocState == PSP_NET_ASSOC_IDLE ||
			  net_assocState == PSP_NET_ASSOC_FAILED ) )
			NET_PSP_AssociationBegin();
		return;
	}

	if( enableNetworking == networkingEnabled )
	{
		stop  = enableNetworking;
		start = enableNetworking;
	}
	else
	{
		stop  = !enableNetworking;
		start = enableNetworking;
		networkingEnabled = enableNetworking;
	}

	if( stop )
	{
		if( ip_socket != PSP_NET_INVALID_SOCKET )
		{
			sceNetInetClose( ip_socket );
			ip_socket = PSP_NET_INVALID_SOCKET;
		}

		if( net_assocState == PSP_NET_ASSOC_CONNECTING ||
			net_assocState == PSP_NET_ASSOC_DISCONNECTING )
		{
			/* Stop/restart is allowed to wait; shutdown must not leave apctl
			   polling after the stack is terminated. */
			NET_PSP_Disconnect();
			net_assocState = PSP_NET_ASSOC_IDLE;
			net_assocRetryAfterDisconnect = qfalse;
		}

		if( net_apConnected )
		{
			NET_PSP_Disconnect();
			net_apConnected = qfalse;
			net_haveLocalIP = qfalse;
			net_assocState = PSP_NET_ASSOC_IDLE;
			Com_Printf( "PSP net: disconnected.\n" );
		}
	}

	if( start )
	{
		if( !net_apConnected )
			NET_PSP_AssociationBegin();

		if( net_apConnected )
		{
			NET_PSP_ReportPowerSave();
			NET_OpenIP();
		}
	}
}

/*
===========================================================================
NET_Init

The WLAN switch is the intent signal. Off means the player is not playing
online, so nothing is loaded and nothing associates. When it is on, the
network stack is initialized here but association is started asynchronously;
the first menu frames are allowed to run while WPA/DHCP completes.

sceUtilityLoadNetModule makes pspnet/pspnet_inet/pspnet_apctl resident.
The stubs are linked unconditionally; the import table is satisfied at
module load, which is why the modules being loaded later is fine - and why
the pspnet* entry in cmake/platforms/psp.cmake was wrong to blame them for
error 8002013C (that was -lpspkernel).
===========================================================================
*/
void NET_Init( void )
{
	int rc;

	Cmd_AddCommand( "net_restart", NET_Restart_f );

	if( !sceWlanGetSwitchState() )
	{
		Com_Printf( "PSP net: WLAN switch is off - loopback only.\n" );
		return;
	}

	if( sceUtilityLoadNetModule( PSP_NET_MODULE_COMMON ) < 0 ||
		sceUtilityLoadNetModule( PSP_NET_MODULE_INET ) < 0 )
	{
		Com_Printf( "PSP net: couldn't load the network modules - loopback only.\n" );
		return;
	}

	net_modulesLoaded = qtrue;

	/* Called individually rather than via pspSdkInetInit() - see the link
	   order note at the top of this file. */
	rc = sceNetInit( PSP_NET_POOLSIZE, PSP_NET_CALLOUT_PRIO, PSP_NET_CALLOUT_STACK,
		PSP_NET_INTR_PRIO, PSP_NET_INTR_STACK );
	if( rc < 0 )
	{
		Com_Printf( "PSP net: sceNetInit failed (0x%08X)\n", rc );
		goto fail;
	}

	rc = sceNetInetInit();
	if( rc < 0 )
	{
		Com_Printf( "PSP net: sceNetInetInit failed (0x%08X)\n", rc );
		sceNetTerm();
		goto fail;
	}

	rc = sceNetResolverInit();
	if( rc < 0 )
	{
		Com_Printf( "PSP net: sceNetResolverInit failed (0x%08X)\n", rc );
		sceNetInetTerm();
		sceNetTerm();
		goto fail;
	}

	/* 0x1600 / 0x42 are pspSdkInetInit's own arguments, read out of
	   libpspsdk.a (psp-objdump -d pspSdkInetInit.o: li a0,5632; li a1,66).
	   That is the combination every working PSP homebrew ships, including
	   the mirror. Priority 0x42 also sits well below the audio thread's
	   0x12, which matters here in a way it does not for those ports. */
	rc = sceNetApctlInit( 0x1600, 0x42 );
	if( rc < 0 )
	{
		Com_Printf( "PSP net: sceNetApctlInit failed (0x%08X)\n", rc );
		sceNetResolverTerm();
		sceNetInetTerm();
		sceNetTerm();
		goto fail;
	}

	net_stackUp = qtrue;

	NET_Config( qtrue );
	return;

fail:
	sceUtilityUnloadNetModule( PSP_NET_MODULE_INET );
	sceUtilityUnloadNetModule( PSP_NET_MODULE_COMMON );
	net_modulesLoaded = qfalse;
}

/*
===========================================================================
NET_Shutdown
===========================================================================
*/
void NET_Shutdown( void )
{
	NET_Config( qfalse );

	if( net_stackUp )
	{
		sceNetApctlTerm();
		sceNetResolverTerm();
		sceNetInetTerm();
		sceNetTerm();
		net_stackUp = qfalse;
	}

	if( net_modulesLoaded )
	{
		sceUtilityUnloadNetModule( PSP_NET_MODULE_INET );
		sceUtilityUnloadNetModule( PSP_NET_MODULE_COMMON );
		net_modulesLoaded = qfalse;
	}
}

/*
===========================================================================
NET_JoinMulticast6 / NET_LeaveMulticast6

No IPv6 on this stack. sv_init.c:289 calls JoinMulticast6 unconditionally.
===========================================================================
*/
void NET_JoinMulticast6( void )
{
}

void NET_LeaveMulticast6( void )
{
}

void NET_Restart_f( void )
{
	NET_Config( qtrue );
}
