// Load-time DXT1/DXT5 encoder from the Xbox port (stb_dxt PCA endpoints, optional refinement).
// GE blocks put the colour indexes before the endpoints and DXT5 alpha last (Quake3PSP-mirror).
#include "psp_dxt.h"

#include <math.h>
#include <string.h>

#define PSP_DXT_POWER_ITERATIONS 4
// Below this the block has no usable principal axis; stb_dxt then uses luminance weights.
#define PSP_DXT_MIN_AXIS 4.0f

// RGB565 with red in the high bits, as the mirror's hardware-proven encoder writes it.
static unsigned int PSP_DxtPack( const float *rgb )
{
	static const float	scale[3] = { 31.0f / 255.0f, 63.0f / 255.0f, 31.0f / 255.0f };
	static const int	top[3] = { 31, 63, 31 };
	int					value[3];
	int					k;

	for ( k = 0; k < 3; k++ ) {
		value[k] = (int)( rgb[k] * scale[k] + 0.5f );
		if ( value[k] < 0 )
			value[k] = 0;
		else if ( value[k] > top[k] )
			value[k] = top[k];
	}
	return (unsigned int)( ( value[0] << 11 ) | ( value[1] << 5 ) | value[2] );
}

static void PSP_DxtUnpack( unsigned int color, int *rgb )
{
	int	r = ( color >> 11 ) & 31;
	int	g = ( color >> 5 ) & 63;
	int	b = color & 31;

	rgb[0] = ( r << 3 ) | ( r >> 2 );
	rgb[1] = ( g << 2 ) | ( g >> 4 );
	rgb[2] = ( b << 3 ) | ( b >> 2 );
}

// Palette index per texel along the c1-c0 line, as if c0 > c1; returns the squared error.
static int PSP_DxtIndexes( const byte (*texels)[4], unsigned int c0, unsigned int c1,
	unsigned int *indexes )
{
	int	palette[4][3];
	int	axis[3];
	int	lengthSq;
	int	error = 0;
	int	i, k;

	PSP_DxtUnpack( c0, palette[0] );
	PSP_DxtUnpack( c1, palette[1] );
	for ( k = 0; k < 3; k++ ) {
		palette[2][k] = ( 2 * palette[0][k] + palette[1][k] + 1 ) / 3;
		palette[3][k] = ( palette[0][k] + 2 * palette[1][k] + 1 ) / 3;
		axis[k] = palette[0][k] - palette[1][k];
	}
	lengthSq = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];
	*indexes = 0;
	for ( i = 0; i < 16; i++ ) {
		int	p = 0;

		if ( lengthSq ) {
			// Six times the position on the line; entries 1, 3, 2, 0 sit at 0, 2, 4 and 6.
			int	step = 6 * ( ( texels[i][0] - palette[1][0] ) * axis[0] +
				( texels[i][1] - palette[1][1] ) * axis[1] +
				( texels[i][2] - palette[1][2] ) * axis[2] );

			p = step < lengthSq ? 1 : step < 3 * lengthSq ? 3 : step < 5 * lengthSq ? 2 : 0;
		}
		for ( k = 0; k < 3; k++ ) {
			int	d = texels[i][k] - palette[p][k];

			error += d * d;
		}
		*indexes |= (unsigned int)p << ( i * 2 );
	}
	return error;
}

// The two texels farthest apart along the colour covariance's principal axis.
static void PSP_DxtEndpoints( const byte (*texels)[4], float *high, float *low )
{
	float	mean[3] = { 0.0f, 0.0f, 0.0f };
	float	cov[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	float	axis[3];
	int		minColor[3] = { 255, 255, 255 };
	int		maxColor[3] = { 0, 0, 0 };
	float	minDot = 1e30f, maxDot = -1e30f;
	int		minTexel = 0, maxTexel = 0;
	int		i, k;

	for ( i = 0; i < 16; i++ ) {
		for ( k = 0; k < 3; k++ ) {
			mean[k] += texels[i][k];
			if ( texels[i][k] < minColor[k] )
				minColor[k] = texels[i][k];
			if ( texels[i][k] > maxColor[k] )
				maxColor[k] = texels[i][k];
		}
	}
	for ( k = 0; k < 3; k++ )
		mean[k] *= 1.0f / 16.0f;
	for ( i = 0; i < 16; i++ ) {
		float	r = texels[i][0] - mean[0];
		float	g = texels[i][1] - mean[1];
		float	b = texels[i][2] - mean[2];

		cov[0] += r * r;
		cov[1] += r * g;
		cov[2] += r * b;
		cov[3] += g * g;
		cov[4] += g * b;
		cov[5] += b * b;
	}
	for ( k = 0; k < 3; k++ )
		axis[k] = (float)( maxColor[k] - minColor[k] );
	for ( i = 0; i < PSP_DXT_POWER_ITERATIONS; i++ ) {
		float	x = axis[0] * cov[0] + axis[1] * cov[1] + axis[2] * cov[2];
		float	y = axis[0] * cov[1] + axis[1] * cov[3] + axis[2] * cov[4];
		float	z = axis[0] * cov[2] + axis[1] * cov[4] + axis[2] * cov[5];
		float	largest = fabsf( x );

		if ( fabsf( y ) > largest )
			largest = fabsf( y );
		if ( fabsf( z ) > largest )
			largest = fabsf( z );
		if ( largest < PSP_DXT_MIN_AXIS ) {
			axis[0] = 0.299f;
			axis[1] = 0.587f;
			axis[2] = 0.114f;
			break;
		}
		axis[0] = x / largest;
		axis[1] = y / largest;
		axis[2] = z / largest;
	}
	for ( i = 0; i < 16; i++ ) {
		float	d = texels[i][0] * axis[0] + texels[i][1] * axis[1] + texels[i][2] * axis[2];

		if ( d < minDot ) {
			minDot = d;
			minTexel = i;
		}
		if ( d > maxDot ) {
			maxDot = d;
			maxTexel = i;
		}
	}
	for ( k = 0; k < 3; k++ ) {
		high[k] = texels[maxTexel][k];
		low[k] = texels[minTexel][k];
	}
}

// Least-squares endpoints for fixed indexes (stb_dxt RefineBlock); qfalse when all indexes match.
static qboolean PSP_DxtRefine( const byte (*texels)[4], unsigned int indexes, float *high, float *low )
{
	static const float	weights[4] = { 1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
	float				aa = 0.0f, ab = 0.0f, bb = 0.0f;
	float				ax[3] = { 0.0f, 0.0f, 0.0f };
	float				bx[3] = { 0.0f, 0.0f, 0.0f };
	float				det;
	int					i, k;

	for ( i = 0; i < 16; i++ ) {
		float	a = weights[( indexes >> ( i * 2 ) ) & 3];
		float	b = 1.0f - a;

		aa += a * a;
		ab += a * b;
		bb += b * b;
		for ( k = 0; k < 3; k++ ) {
			ax[k] += a * texels[i][k];
			bx[k] += b * texels[i][k];
		}
	}
	det = aa * bb - ab * ab;
	if ( fabsf( det ) < 1e-6f )
		return qfalse;
	det = 1.0f / det;
	for ( k = 0; k < 3; k++ ) {
		high[k] = ( bb * ax[k] - ab * bx[k] ) * det;
		low[k] = ( aa * bx[k] - ab * ax[k] ) * det;
	}
	return qtrue;
}

// GE colour block: the index word first, then c0 and c1.
static void PSP_DxtCompressBlock( const byte (*texels)[4], qboolean refine, unsigned int *out )
{
	float			high[3], low[3];
	unsigned int	c0, c1;
	unsigned int	indexes;
	int				error;

	PSP_DxtEndpoints( texels, high, low );
	c0 = PSP_DxtPack( high );
	c1 = PSP_DxtPack( low );
	error = PSP_DxtIndexes( texels, c0, c1, &indexes );
	if ( refine && error && PSP_DxtRefine( texels, indexes, high, low ) ) {
		unsigned int	r0 = PSP_DxtPack( high );
		unsigned int	r1 = PSP_DxtPack( low );
		unsigned int	refined;

		if ( PSP_DxtIndexes( texels, r0, r1, &refined ) < error ) {
			c0 = r0;
			c1 = r1;
			indexes = refined;
		}
	}
	// Four-colour mode needs c0 > c1; with equal endpoints index 3 would be transparent.
	if ( c0 < c1 ) {
		unsigned int	swap = c0;

		c0 = c1;
		c1 = swap;
		indexes ^= 0x55555555u;
	} else if ( c0 == c1 ) {
		indexes = 0;
	}
	out[0] = indexes;
	out[1] = c0 | ( c1 << 16 );
}

// DXT5 alpha palette: a0 > a1 gives 8 steps, otherwise 6 steps plus exact 0 and 255.
static int PSP_DxtAlphaIndexes( const byte (*texels)[4], int a0, int a1, unsigned long long *indexes )
{
	int	palette[8];
	int	error = 0;
	int	i, j;

	palette[0] = a0;
	palette[1] = a1;
	if ( a0 > a1 ) {
		for ( i = 1; i < 7; i++ )
			palette[i + 1] = ( ( 7 - i ) * a0 + i * a1 ) / 7;
	} else {
		for ( i = 1; i < 5; i++ )
			palette[i + 1] = ( ( 5 - i ) * a0 + i * a1 ) / 5;
		palette[6] = 0;
		palette[7] = 255;
	}
	*indexes = 0;
	for ( i = 0; i < 16; i++ ) {
		int	best = 0;
		int	bestError = 256 * 256;

		for ( j = 0; j < 8; j++ ) {
			int	d = texels[i][3] - palette[j];

			if ( d * d < bestError ) {
				bestError = d * d;
				best = j;
			}
		}
		error += bestError;
		*indexes |= (unsigned long long)best << ( i * 3 );
	}
	return error;
}

// GE alpha block: 48 index bits, then a0 and a1. Tries 8 steps, then 6 with 0 and 255 exact.
static void PSP_DxtCompressAlpha( const byte (*texels)[4], byte *out )
{
	int					low = 255, high = 0, innerLow = 255, innerHigh = 0;
	int					a0, a1, i;
	unsigned long long	indexes;

	for ( i = 0; i < 16; i++ ) {
		int	a = texels[i][3];

		if ( a < low )
			low = a;
		if ( a > high )
			high = a;
		if ( a != 0 && a != 255 ) {
			if ( a < innerLow )
				innerLow = a;
			if ( a > innerHigh )
				innerHigh = a;
		}
	}
	if ( innerLow > innerHigh )
		innerLow = innerHigh = 0;
	a0 = high;
	a1 = low;
	if ( high == low ) {
		indexes = 0;
	} else {
		int					error = PSP_DxtAlphaIndexes( texels, high, low, &indexes );
		unsigned long long	inner;

		if ( error && ( low == 0 || high == 255 ) &&
			PSP_DxtAlphaIndexes( texels, innerLow, innerHigh, &inner ) < error ) {
			a0 = innerLow;
			a1 = innerHigh;
			indexes = inner;
		}
	}
	for ( i = 0; i < 6; i++ )
		out[i] = (byte)( indexes >> ( i * 8 ) );
	out[6] = (byte)a0;
	out[7] = (byte)a1;
}

void PSP_DxtCompress( qboolean alpha, qboolean refine, const byte *rgba, int width, int height, void *out )
{
	byte	texels[16][4];
	byte	*block = (byte *)out;
	int		x, y, row;

	for ( y = 0; y < height; y += 4 ) {
		for ( x = 0; x < width; x += 4 ) {
			for ( row = 0; row < 4; row++ )
				memcpy( texels[row * 4], rgba + ( (size_t)( y + row ) * width + x ) * 4, 16 );
			// DXT5 colour blocks never use the 3-colour mode, so both formats share the encoder.
			PSP_DxtCompressBlock( (const byte (*)[4])texels, refine, (unsigned int *)block );
			if ( alpha ) {
				PSP_DxtCompressAlpha( (const byte (*)[4])texels, block + 8 );
				block += 16;
			} else {
				block += 8;
			}
		}
	}
}
