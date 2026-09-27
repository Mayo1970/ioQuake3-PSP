#ifndef __PSP_ADPCM_H__
#define __PSP_ADPCM_H__

struct sndBuffer_s;

// Decoded samples of one ADPCM chunk (SND_CHUNK_SIZE * 4), cached so each chunk decodes once.
const short	*PSP_AdpcmSamples( struct sndBuffer_s *chunk );
// SND_free calls this before a chunk can hold another sound.
void		PSP_AdpcmForget( const struct sndBuffer_s *chunk );
void		PSP_AdpcmCacheInit( void );
void		PSP_AdpcmCacheShutdown( void );

#endif // __PSP_ADPCM_H__
