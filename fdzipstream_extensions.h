
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
 * @remark if FDZIPSTREAM_REGISTEREXTENSIONS is defined to 1 then this is called automatically within zs_init();
 */
extern size_t zs_extensions_register( ZIPstream *zs );

/* AES Deflate support */
#ifndef FDZIPSTREAM_AESDEFLATE
#define FDZIPSTREAM_AESDEFLATE 1
#endif

#if FDZIPSTREAM_AESDEFLATE
//#	define ZS_AES1_DEFLATE    9901 /* Deflate with AES-1 encryption  */
#	define ZS_AES2_DEFLATE    9902 /* Deflate with AES-2 encryption  */

/* Set required AES encryption information
 * @remark Must be called after zs_entrybegin() when methodId == ZS_AES1_DEFLATE
 */
extern int32_t zs_aesdeflate_setpassword( ZIPstream *zstream, ZIPentry *zentry, const char* password );
#endif

//############ For extension use only #############
//TODO: Sort this
void zs_freeImpl( ZIPmethodimpl* this );
//TODO: Sort this
ZIPmethodimpl* zs_initmethod( ZIPstream *zs, int methodId );
//TODO: Sort this
int64_t zs_writedata ( ZIPstream *zstream, uint8_t *writebuffer, int64_t writebuffersize );
//TODO: Sort this
void packuint8 (ZIPstream *ZS, int *O, uint16_t V);
//TODO: Sort this
void packuint16 (ZIPstream *ZS, int *O, uint16_t V);
//TODO: Sort this
void packuint32 (ZIPstream *ZS, int *O, uint32_t V);
//TODO: Sort this
void packuint64 (ZIPstream *ZS, int *O, uint64_t V);

#ifdef __cplusplus
}
#endif

#endif /* FDZIPSTREAM_EXTENSIONS_H */


