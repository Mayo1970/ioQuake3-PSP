/*
===========================================================================
PSP port - code/psp/psp_file.h

Session 7. Virtual file handles for the pk3 layer.

The PSP allows only about ten simultaneously open stdio files. ioquake3
keeps one open per pk3 for the whole life of the process (pak->handle,
qcommon/files.c) and baseq3 ships nine of them, so the tenth open - which
in practice is the game VM opening games.log during G_InitGame - fails, and
files.c raises Com_Error(ERR_FATAL, "Couldn't open %s").

This is not a guess. Quake3PSP-mirror hit the identical wall and its fix is
qcommon/ioctrl.c, which opens with

    #define MAX_OPEN_FILES 5   //psp have limit, 10 opened files max (for fopen)

and virtualises the handle: FS_Fopen returns an index, at most five are
physically open, an eviction fcloses one while remembering its name, and
the next read silently fopens it again.

Same idea here, with two differences. The mirror had to convert its whole
unzip.c off FILE* to make it work; this tree's unzip.c already routes every
byte through a pluggable zlib_filefunc_def (qcommon/ioapi.c), so the cache
drops in under that instead of touching zip code at all. And an evicted
handle records its file position and restores it on reopen - the mirror's
FS_RestoreHandle does not, and relies on the caller seeking afterwards.
===========================================================================
*/

#ifndef __PSP_FILE_H__
#define __PSP_FILE_H__

#include "../qcommon/q_shared.h"

/*
 Logical handles. Cheap - one struct each, nothing open.
*/
#define PSP_VF_SLOTS		64

/*
 How many are allowed to be physically open at once.

 The mirror uses 5 of its measured ceiling of ~10. 4 is used here because
 this port has consumers the mirror does not: con_psp.c reopens the log for
 every single line (write-through, Session 3), and files.c still opens
 non-pak files as ordinary FILE*.
*/
#define PSP_VF_MAX_OPEN		4

/*
 A handle is an index + 1, so 0 is always "no handle" and the value can be
 stuffed into unzip's voidpf stream field without a cast to a real pointer.
*/
int	PSP_VF_Open( const char *name, const char *mode );
int	PSP_VF_Close( int handle );
int	PSP_VF_Read( int handle, void *buf, int size );
int	PSP_VF_Write( int handle, const void *buf, int size );
long	PSP_VF_Tell( int handle );
int	PSP_VF_Seek( int handle, long offset, int origin );
int	PSP_VF_Error( int handle );

// Appended to the PSP resource report in code/sys/sys_psp.c.
void	PSP_VF_ReportInto( char *buf, int size );

#endif // __PSP_FILE_H__
