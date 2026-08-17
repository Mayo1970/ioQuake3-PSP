/*
===========================================================================
PSP port - code/psp/psp_pool.h

Session 11d. A dedicated allocator over the ~4 MB VOLATILE partition
(partition 5), which every PSP model has and which this port has never
touched. It exists because Session 11c measured the newlib heap completely
dry with textures still asking for room:

    PSP heap [SV_SpawnServer entry]: arena 35065 KB, used 35057 KB,
                                     free 7 KB, largest free 1 KB
    PSP_TexUpload2D: out of memory for 256x256 level 0 (131072 bytes)
    Error: Not a JPEG file: starts with 0x7a 0x57, loading levelshots/Q3DM3.jpg

The second line is a FRAGMENTATION failure as much as an exhaustion one -
at the point it printed there were 3.3 MB free with a largest block under
1 KB, because psp_tex.c memaligns every mip level individually out of a
heap it shares with the hunk, the zone, the sound pool and libjpeg. The
third is the same shortage reaching FS_ReadFile, which then hands back a
buffer it never got.

Moving textures here fixes both at once: it is ~4 MB of memory the port was
not using at all, rather than another re-slicing of a heap that is short,
and it is a pool with ONE kind of customer, so its fragmentation behaviour
is the texture set's own.

Cost, and why it is acceptable here: partition 5 is what the firmware uses
for suspend, so holding it means holding scePowerLock and blocking sleep.
That is Session 12's subject (suspend/resume already has to reopen Memory
Stick and Wi-Fi handles), and DaedalusX64 - the reference this port takes
its PSP technique from - holds the same block for the same reason.

The allocator is first-fit with coalescing over one contiguous block. The
first allocation is a reserved sound-cache segment; texture allocations use
the remainder. Textures arrive in a burst at map load and leave in a burst at
RE_Shutdown, which is the easiest possible pattern.
===========================================================================
*/

#ifndef __PSP_POOL_H__
#define __PSP_POOL_H__

#include <stddef.h>

// Locks the volatile partition and prepares the allocator. Safe to call
// twice; returns qfalse when the block could not be locked, in which case
// every PSP_PoolAlloc returns NULL and callers fall back to the heap.
qboolean	PSP_PoolInit( void );

void		*PSP_PoolAlloc( size_t bytes );
void		PSP_PoolFree( void *p );
qboolean	PSP_PoolOwns( const void *p );

// The first sound-cache segment is reserved before texture allocation starts.
// SND_setup takes ownership of it after the renderer has initialized the pool.
#define PSP_SOUND_CHUNKS_PER_UNIT 1536
void		*PSP_PoolTakeSound( void );
void		PSP_PoolReleaseSound( void *p );

// Bytes in use / total, for the memory reports.
void		PSP_PoolStats( int *usedBytes, int *totalBytes, int *largestFree );

#endif // __PSP_POOL_H__
