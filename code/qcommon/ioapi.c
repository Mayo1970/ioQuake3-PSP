/* ioapi.c -- IO base function header for compress/uncompress .zip
   files using zlib + zip or unzip API

   Version 1.01e, February 12th, 2005

   Copyright (C) 1998-2005 Gilles Vollant
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __PSP__
#include <stdint.h>	// intptr_t - the PSP stream value is a handle, not a pointer
#endif

#ifdef USE_INTERNAL_ZLIB
#include "zlib.h"
#else
#include <zlib.h>
#endif

#include "ioapi.h"



/* I've found an old Unix (a SunOS 4.1.3_U1) without all SEEK_* defined.... */

#ifndef SEEK_CUR
#define SEEK_CUR    1
#endif

#ifndef SEEK_END
#define SEEK_END    2
#endif

#ifndef SEEK_SET
#define SEEK_SET    0
#endif

voidpf ZCALLBACK fopen_file_func OF((
   voidpf opaque,
   const char* filename,
   int mode));

uLong ZCALLBACK fread_file_func OF((
   voidpf opaque,
   voidpf stream,
   void* buf,
   uLong size));

uLong ZCALLBACK fwrite_file_func OF((
   voidpf opaque,
   voidpf stream,
   const void* buf,
   uLong size));

long ZCALLBACK ftell_file_func OF((
   voidpf opaque,
   voidpf stream));

long ZCALLBACK fseek_file_func OF((
   voidpf opaque,
   voidpf stream,
   uLong offset,
   int origin));

int ZCALLBACK fclose_file_func OF((
   voidpf opaque,
   voidpf stream));

int ZCALLBACK ferror_file_func OF((
   voidpf opaque,
   voidpf stream));


voidpf ZCALLBACK fopen_file_func (opaque, filename, mode)
   voidpf opaque;
   const char* filename;
   int mode;
{
    FILE* file = NULL;
    const char* mode_fopen = NULL;
    if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER)==ZLIB_FILEFUNC_MODE_READ)
        mode_fopen = "rb";
    else
    if (mode & ZLIB_FILEFUNC_MODE_EXISTING)
        mode_fopen = "r+b";
    else
    if (mode & ZLIB_FILEFUNC_MODE_CREATE)
        mode_fopen = "wb";

    if ((filename!=NULL) && (mode_fopen != NULL))
        file = fopen(filename, mode_fopen);
    return file;
}


uLong ZCALLBACK fread_file_func (opaque, stream, buf, size)
   voidpf opaque;
   voidpf stream;
   void* buf;
   uLong size;
{
    uLong ret;
    ret = (uLong)fread(buf, 1, (size_t)size, (FILE *)stream);
    return ret;
}


uLong ZCALLBACK fwrite_file_func (opaque, stream, buf, size)
   voidpf opaque;
   voidpf stream;
   const void* buf;
   uLong size;
{
    uLong ret;
    ret = (uLong)fwrite(buf, 1, (size_t)size, (FILE *)stream);
    return ret;
}

long ZCALLBACK ftell_file_func (opaque, stream)
   voidpf opaque;
   voidpf stream;
{
    long ret;
    ret = ftell((FILE *)stream);
    return ret;
}

long ZCALLBACK fseek_file_func (opaque, stream, offset, origin)
   voidpf opaque;
   voidpf stream;
   uLong offset;
   int origin;
{
    int fseek_origin=0;
    long ret;
    switch (origin)
    {
    case ZLIB_FILEFUNC_SEEK_CUR :
        fseek_origin = SEEK_CUR;
        break;
    case ZLIB_FILEFUNC_SEEK_END :
        fseek_origin = SEEK_END;
        break;
    case ZLIB_FILEFUNC_SEEK_SET :
        fseek_origin = SEEK_SET;
        break;
    default: return -1;
    }
    ret = 0;
    fseek((FILE *)stream, offset, fseek_origin);
    return ret;
}

int ZCALLBACK fclose_file_func (opaque, stream)
   voidpf opaque;
   voidpf stream;
{
    int ret;
    ret = fclose((FILE *)stream);
    return ret;
}

int ZCALLBACK ferror_file_func (opaque, stream)
   voidpf opaque;
   voidpf stream;
{
    int ret;
    ret = ferror((FILE *)stream);
    return ret;
}

#ifdef __PSP__
/*
===========================================================================
PSP: virtual file handles.

The PSP runs out of stdio handles at about ten. ioquake3 holds one open per
pk3 for the process lifetime (pak->handle, files.c) and baseq3 ships nine,
so the tenth open fails and files.c raises
Com_Error(ERR_FATAL, "Couldn't open %s") - which is exactly how the first
Session 7 map load died.

Quake3PSP-mirror hit the same wall; its qcommon/ioctrl.c says so outright
("psp have limit, 10 opened files max (for fopen)") and fixes it with a
handle cache. It had to convert its whole unzip.c off FILE* to do that.
This tree does not: every pk3 byte already passes through these seven
callbacks, so swapping them here covers pak->handle and the per-read
uniqueFILE opens at once and leaves unzip.c untouched.

The stream value is a small integer handle, not a pointer. That is legal -
zlib only ever hands it back to these functions - and it is what makes 0
mean failure without needing a NULL pointer.
===========================================================================
*/
#include "../psp/psp_file.h"

#define PSP_VF_TO_STREAM(h)	((voidpf)(intptr_t)(h))
#define PSP_VF_FROM_STREAM(s)	((int)(intptr_t)(s))

static voidpf ZCALLBACK psp_fopen_file_func (voidpf opaque, const char* filename, int mode)
{
    const char* mode_fopen = NULL;
    int handle;

    if ((mode & ZLIB_FILEFUNC_MODE_READWRITEFILTER)==ZLIB_FILEFUNC_MODE_READ)
        mode_fopen = "rb";
    else
    if (mode & ZLIB_FILEFUNC_MODE_EXISTING)
        mode_fopen = "r+b";
    else
    if (mode & ZLIB_FILEFUNC_MODE_CREATE)
        mode_fopen = "wb";

    if (filename == NULL || mode_fopen == NULL)
        return NULL;

    handle = PSP_VF_Open(filename, mode_fopen);

    return handle ? PSP_VF_TO_STREAM(handle) : NULL;
}

static uLong ZCALLBACK psp_fread_file_func (voidpf opaque, voidpf stream, void* buf, uLong size)
{
    return (uLong)PSP_VF_Read(PSP_VF_FROM_STREAM(stream), buf, (int)size);
}

static uLong ZCALLBACK psp_fwrite_file_func (voidpf opaque, voidpf stream, const void* buf, uLong size)
{
    return (uLong)PSP_VF_Write(PSP_VF_FROM_STREAM(stream), buf, (int)size);
}

static long ZCALLBACK psp_ftell_file_func (voidpf opaque, voidpf stream)
{
    return PSP_VF_Tell(PSP_VF_FROM_STREAM(stream));
}

static long ZCALLBACK psp_fseek_file_func (voidpf opaque, voidpf stream, uLong offset, int origin)
{
    int fseek_origin;

    switch (origin)
    {
    case ZLIB_FILEFUNC_SEEK_CUR : fseek_origin = SEEK_CUR; break;
    case ZLIB_FILEFUNC_SEEK_END : fseek_origin = SEEK_END; break;
    case ZLIB_FILEFUNC_SEEK_SET : fseek_origin = SEEK_SET; break;
    default: return -1;
    }

    /* Upstream's fseek_file_func discards fseek's result and always returns
       0; unzip.c relies on that, so do not "fix" it here. */
    PSP_VF_Seek(PSP_VF_FROM_STREAM(stream), (long)offset, fseek_origin);
    return 0;
}

static int ZCALLBACK psp_fclose_file_func (voidpf opaque, voidpf stream)
{
    return PSP_VF_Close(PSP_VF_FROM_STREAM(stream));
}

static int ZCALLBACK psp_ferror_file_func (voidpf opaque, voidpf stream)
{
    return PSP_VF_Error(PSP_VF_FROM_STREAM(stream));
}
#endif // __PSP__

void fill_fopen_filefunc (pzlib_filefunc_def)
  zlib_filefunc_def* pzlib_filefunc_def;
{
#ifdef __PSP__
    pzlib_filefunc_def->zopen_file = psp_fopen_file_func;
    pzlib_filefunc_def->zread_file = psp_fread_file_func;
    pzlib_filefunc_def->zwrite_file = psp_fwrite_file_func;
    pzlib_filefunc_def->ztell_file = psp_ftell_file_func;
    pzlib_filefunc_def->zseek_file = psp_fseek_file_func;
    pzlib_filefunc_def->zclose_file = psp_fclose_file_func;
    pzlib_filefunc_def->zerror_file = psp_ferror_file_func;
    pzlib_filefunc_def->opaque = NULL;
    return;
#else
    pzlib_filefunc_def->zopen_file = fopen_file_func;
    pzlib_filefunc_def->zread_file = fread_file_func;
    pzlib_filefunc_def->zwrite_file = fwrite_file_func;
    pzlib_filefunc_def->ztell_file = ftell_file_func;
    pzlib_filefunc_def->zseek_file = fseek_file_func;
    pzlib_filefunc_def->zclose_file = fclose_file_func;
    pzlib_filefunc_def->zerror_file = ferror_file_func;
    pzlib_filefunc_def->opaque = NULL;
#endif
}
