
#include "fdzipstream_extensions.h"

#if FDZIPSTREAM_AESDEFLATE

/* Deflate implementation adds z_stream */
typedef struct zipaesdeflateimpl_s
{
	ZIPmethod method; //< Base
	//z_stream zlstream;
} ZIPaesdeflateimpl;

static ZIPaesdeflateimpl aesdeflateImpl = { {ZS_AES1_DEFLATE, NULL, NULL, NULL, NULL, NULL} };

#endif

size_t zs_extensions_register( ZIPstream *zs )
{
	size_t count = 0;

#if FDZIPSTREAM_AESDEFLATE
	if ( zs_registermethod( zs, (ZIPmethod*)&aesdeflateImpl ) )
		++count;
#endif

	return count;
}


