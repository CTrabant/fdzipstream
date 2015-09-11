
#ifndef FDZIPSTREAM_EXTENSIONS_H
#define FDZIPSTREAM_EXTENSIONS_H

#include <stdint.h>
#include <unistd.h>

#include "fdzipstream.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register all enabled extensions
 * @return Number of extension methods registered
 * @remark if FDZIPSTREAM_REGISTEREXTENSIONS is defined to 1 then this is called automatically wihtin zs_init();
 */
extern size_t zs_extensions_register( ZIPstream *zs );

/* AES Deflate support */
#ifndef FDZIPSTREAM_AESDEFLATE
#define FDZIPSTREAM_AESDEFLATE 0
#endif

#if FDZIPSTREAM_AESDEFLATE
#	define ZS_AES1_DEFLATE    9901 /* Deflate with AES-1 encryption  */
//#	define ZS_AES2_DEFLATE    9901 /* Deflate with AES-2 encryption  */
#endif

#ifdef __cplusplus
}
#endif

#endif /* FDZIPSTREAM_EXTENSIONS_H */


