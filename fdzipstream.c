/***************************************************************************
 * fdzipstream.c
 *
 * Create ZIP archives in streaming fashion, writing to a file
 * descriptor.  The output stream (file descriptor) does not need to
 * be seekable and can be a pipe or a network socket.  The entire
 * archive contents does not need to be in memory at once.
 *
 * zlib is required for deflate compression: http://www.zlib.net/
 *
 * What this will do for you:
 *
 * - Create a ZIP archive in a streaming fashion, writing to an output
 *   stream (file descriptor, pipe, network socket) without seeking.
 * - Compress the archive entries (using zlib).
 * - Add ZIP64 structures as needed to support large (>4GB) archives.
 * - Simple creation of ZIP archives even if not streaming.
 *
 * What this will NOT do for you:
 *
 * - Open/close files or sockets.
 * - Support advanced ZIP archive features (e.g. file attributes).
 * - Allow archiving of individual files/entries larger than 4GB, the total
 *    of all files can be larger than 4GB but not individual entries.
 * - Allow every possible compression methodId.
 *
 * ZIP archive file/entry modifiation times are stored in UTC.
 *
 * Usage pattern
 *
 * Creating a ZIP archive when entire files/entries are in memory:
 *  zs_init ()
 *    for each entry:
 *      zs_writeentry ()
 *  zs_finish ()
 *  zs_free ()
 *
 * Creating a ZIP archive when files/entries are chunked:
 *  zs_init ()
 *    for each entry:
 *      zs_entrybegin ()
 *        for each chunk of entry:
 *          zs_entrydata()
 *      zs_entryflush()
 *      zs_entryend()
 *  zs_finish ()
 *  zs_free ()
 *
 * LICENSE
 *
 * Copyright 2015 CTrabant
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified 2015.8.2
 ***************************************************************************/

/* Allow this code to be skipped by declaring NOFDZIP */
#ifndef NOFDZIP

#define FDZIPVERSION 1.1

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <zlib.h>

#include "fdzipstream.h"

#if FDZIPSTREAM_REGISTEREXTENSIONS
#include "fdzipstream_extensions.h"
#endif

#define BIT_SET(a,b) ((a) |= (1<<(b)))

static uint32_t zs_datetime_unixtodos ( time_t t );
static void zs_htolx ( void *data, int size );

/* Helper functions to write little-endian integer values to a
 * specified offset in the ZIPstream buffer and increment offset. */
void packuint8 (ZIPstream *ZS, int *O, uint16_t V)
{
	memcpy (ZS->buffer+*O, &V, 1);
	*O += 1;
}
void packuint16 (ZIPstream *ZS, int *O, uint16_t V)
{
	memcpy (ZS->buffer+*O, &V, 2);
	zs_htolx(ZS->buffer+*O, 2);
	*O += 2;
}
void packuint32 (ZIPstream *ZS, int *O, uint32_t V)
{
	memcpy (ZS->buffer+*O, &V, 4);
	zs_htolx(ZS->buffer+*O, 4);
	*O += 4;
}
void packuint64 (ZIPstream *ZS, int *O, uint64_t V)
{
	memcpy (ZS->buffer+*O, &V, 8);
	zs_htolx(ZS->buffer+*O, 8);
	*O += 8;
}

void zs_freeImpl( ZIPmethodimpl* this )
{
	this->method.free( this );
}

/* store implementation */
static ZIPmethodimpl* zs_store_init( ZIPstream *zstream, int method );
static void zs_store_free( ZIPmethodimpl* impl );
static int32_t zs_store_entryend( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry );
static int32_t zs_store_process( ZIPmethodimpl* thisbase, uint8_t *entry, int64_t entrySize, uint8_t* writeBuffer, int64_t writeBufferSize, int32_t final );
static int32_t zs_store_entrybegin( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry );
static ZIPmethodimpl storeImpl = { {zs_store_init, zs_store_free }, zs_store_entrybegin, zs_store_process, zs_store_entryend };

/* Deflate implementation */
static ZIPmethodimpl* zs_deflate_init( ZIPstream *zstream, int method );
static void zs_deflate_free( ZIPmethodimpl* this );
static int32_t zs_deflate_entryend( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry );
static int32_t zs_deflate_process( ZIPmethodimpl* thisbase, uint8_t *entry, int64_t entrySize, uint8_t* writeBuffer, int64_t writeBufferSize, int32_t final );
static int32_t zs_deflate_entrybegin( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry );
typedef struct zipdeflateimpl_s
{
	ZIPmethodimpl impl; //< Base
	z_stream zlstream;
} ZIPdeflateimpl;
static ZIPdeflateimpl deflateImpl = { { { zs_deflate_init, zs_deflate_free}, zs_deflate_entrybegin, zs_deflate_process, zs_deflate_entryend } };

static int32_t zs_store_entrybegin( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry )
{
	zentry->CompressionMethod = ZS_STORE;
	return 1;
}

static int32_t zs_store_process( ZIPmethodimpl* thisbase, uint8_t *entry, int64_t entrySize, uint8_t* writeBuffer, int64_t writeBufferSize, int32_t final )
{
	return 0;
}

static int32_t zs_store_entryend( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry )
{
	return 1;
}
static ZIPmethodimpl* zs_store_init( ZIPstream *zstream, int method )
{
	if ( method != ZS_STORE )
		return NULL;

	return &storeImpl;
}

static void zs_store_free( ZIPmethodimpl* impl )
{
	/* do nothing as storeImpl is on stack */
}

static int32_t zs_deflate_entrybegin( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry )
{
	zentry->CompressionMethod = ZS_DEFLATE;

	ZIPdeflateimpl* this = (ZIPdeflateimpl*)thisbase;
	z_stream* zlstream = &(this->zlstream);

	/* Allocate deflate zlib stream state & initialize */
	zlstream->zalloc = Z_NULL;
	zlstream->zfree = Z_NULL;
	zlstream->opaque = Z_NULL;
	zlstream->total_in = 0;
	zlstream->total_out = 0;
	zlstream->data_type = Z_BINARY;

	if ( deflateInit2(zlstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
			-MAX_WBITS, DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY) != Z_OK )
	{
		fprintf (stderr, "zs_beginentry: Error with deflateInit2()\n");
		return NULL;
	}

	return 1;
}

/*
 * 	entry & entrySize = start of entry block
 * 	entry & !entrySize = continuation of entry block
 * 	final = flush/ entry end
 */
static int32_t zs_deflate_process( ZIPmethodimpl* thisbase, uint8_t *entry, int64_t entrySize, uint8_t* writeBuffer, int64_t writeBufferSize, int32_t final )
{
	ZIPdeflateimpl* this = (ZIPdeflateimpl*)thisbase;
	z_stream* zlstream = &this->zlstream;

	//Input buffer
	if ( entry && entrySize )
	{
		zlstream->next_in = entry;
		zlstream->avail_in = entrySize;
	}

	if ( !writeBuffer || !writeBufferSize )
	{
		/* Determine maximum size of compressed data and allocate buffer */
		size_t maximumWriteSize = deflateBound( zlstream, entrySize );
		return maximumWriteSize;
	}

	//Output buffer
	zlstream->next_out = writeBuffer;
	zlstream->avail_out = writeBufferSize;

	int rv = deflate( zlstream, (final ? Z_FINISH : Z_NO_FLUSH) );
	if ( rv != Z_OK && rv != Z_STREAM_END )
	{
		fprintf (stderr, "zs_deflate_process: Error with deflate(): %d\n", rv);
		return -1;
	}

	/*if ( entry==NULL && rv == Z_STREAM_END )
	{
		fprintf (stderr, "zs_deflate_process:  deflate() flush failed!\n", rv);
		return -1;
	}*/

	size_t writetotal = writeBufferSize - zlstream->avail_out;
	return writetotal;
}

static int32_t zs_deflate_entryend( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry )
{
	ZIPdeflateimpl* this = (ZIPdeflateimpl*)thisbase;
	z_stream* zlstream = &this->zlstream;
	deflateEnd (zlstream);
	return 1;
}

static void zs_deflate_free( ZIPmethodimpl* this )
{
	free( this );
}

static ZIPmethodimpl* zs_deflate_init( ZIPstream *zstream, int method )
{
	if ( method != ZS_DEFLATE )
		return NULL;

	ZIPmethodimpl *impl = (ZIPmethodimpl *) calloc (1, sizeof(deflateImpl));
	memcpy( impl, &deflateImpl, sizeof(deflateImpl) );
	return impl;
}

static ZIPentry* zs_allocateentry( ZIPstream *zstream, char *name, time_t modtime )
{
	ZIPentry *zentry = (ZIPentry *) calloc (1, sizeof(ZIPentry));

	/* Allocate and initialize new entry */
	if ( zentry == NULL )
	{
		fprintf (stderr, "Cannot allocate memory for entry\n");
		return NULL;
	}

	zentry->ZipVersion = 20;
	zentry->GeneralFlag = 0;
	uint32_t u32 = zs_datetime_unixtodos (modtime);
	zentry->DOSDate = (uint16_t) (u32 >> 16);
	zentry->DOSTime = (uint16_t) (u32 & 0xFFFF);
	zentry->CRC32 = crc32 (0L, Z_NULL, 0);
	zentry->CompressedSize = 0;
	zentry->UncompressedSize = 0;
	zentry->LocalHeaderOffset = zstream->WriteOffset;
	strncpy (zentry->Name, name, ZENTRY_NAME_LENGTH - 1);
	zentry->NameLength = strlen (zentry->Name);

	/* Add new entry to stream list */
	if ( ! zstream->FirstEntry )
	{
		zstream->FirstEntry = zentry;
		zstream->LastEntry = zentry;
	}
	else
	{
		zstream->LastEntry->next = zentry;
		zstream->LastEntry = zentry;
	}
	zstream->EntryCount++;
	return zentry;
}

/***************************************************************************
 * zs_init:
 *
 * Initialize and return an ZIPstream struct. If a pointer to an
 * existing ZIPstream is supplied it will be re-initizlied, otherwise
 * memory will be allocated.
 *
 * Returns a pointer to a ZIPstream struct on success or NULL on error.
 ***************************************************************************/
ZIPstream *
zs_init ( int fd, ZIPstream *zs )
{
	ZIPentry *zentry, *tofree;

	if ( ! zs )
	{
		zs = (ZIPstream *) malloc (sizeof(ZIPstream));
	}
	else
	{
		zentry = zs->FirstEntry;
		while ( zentry )
		{
			tofree = zentry;
			zentry = zentry->next;
			free (tofree);
		}
	}

	if ( zs == NULL )
	{
		fprintf (stderr, "zs_init: Cannot allocate memory\n");
		return NULL;
	}

	memset (zs, 0, sizeof (ZIPstream));

	zs->fd = fd;

	/* Register inbuilt compression methodIds */
	zs_registermethod( zs, (ZIPmethod*)&storeImpl );
	zs_registermethod( zs, (ZIPmethod*)&deflateImpl );


#if FDZIPSTREAM_REGISTEREXTENSIONS
	zs_extensions_register( zs );
#endif


	return zs;
}  /* End of zs_init() */


/***************************************************************************
 * zs_free:
 *
 * Free all memory associated with a ZIPstream including all ZIPentry
 * structures.
 ***************************************************************************/
void
zs_free ( ZIPstream *zs )
{
	ZIPentry *zentry, *tofree;

	if ( ! zs )
		return;

	zentry = zs->FirstEntry;
	while ( zentry )
	{
		zentry->impl->method.free( zentry->impl );
		tofree = zentry;
		zentry = zentry->next;
		free (tofree);
	}

	free (zs);

}  /* End of zs_free() */

/* Finds the implementation for a storage methodId (i.e. Deflate/Store)
 * @param      methodId     ZS_STORE or ZS_DEFLATE
 * @returns  Pointer to a zipmethod_s structure which contains callbacks for handling a file entries data
 */
ZIPmethodimpl* zs_initmethod( ZIPstream *zs, int methodId )
{
	ZIPmethod* method = zs->firstMethod;
	while ( method )
	{
		ZIPmethodimpl* impl = method->init( zs, methodId );
		if ( impl)
		{
			return impl;
		}
		method = method->next;
	}
	fprintf (stderr, "Unrecognized compression methodId: %d\n", methodId);
	return NULL;
}

static int32_t zs_writeheader(  ZIPstream *zstream, ZIPentry *zentry, ssize_t *writestatus  )
{
	/* Write the Local File Header, with zero'd CRC and sizes */
	int32_t packed = 0;
	packuint32 (zstream, &packed, LOCALHEADERSIG);              /* Data Description signature */
	packuint16 (zstream, &packed, zentry->ZipVersion);
	packuint16 (zstream, &packed, zentry->GeneralFlag);
	packuint16 (zstream, &packed, zentry->CompressionMethod);
	packuint16 (zstream, &packed, zentry->DOSTime);             /* DOS file modification time */
	packuint16 (zstream, &packed, zentry->DOSDate);             /* DOS file modification date */
	packuint32 (zstream, &packed, zentry->CRC32);               /* CRC-32 value of entry */
	packuint32 (zstream, &packed, zentry->CompressedSize);      /* Compressed entry size */
	packuint32 (zstream, &packed, zentry->UncompressedSize);    /* Uncompressed entry size */
	packuint16 (zstream, &packed, zentry->NameLength);          /* File/entry name length */
	packuint16 (zstream, &packed, zentry->ExtraDataSize);       /* Extra field length */
	/* File/entry name */
	memcpy (zstream->buffer+packed, zentry->Name, zentry->NameLength); packed += zentry->NameLength;

	int64_t lwritestatus = zs_writedata (zstream, zstream->buffer, packed);
	if ( lwritestatus != packed )
	{
		fprintf (stderr, "Error writing ZIP local header: %s\n", strerror(errno));

		if ( writestatus )
			*writestatus = (ssize_t)lwritestatus;

		return NULL;
	}

	if ( zentry->impl->writeExtraData )
	{
		zentry->impl->writeExtraData( zentry->impl, zstream, zentry );
	}
	return 1;
}

int zs_registermethod( ZIPstream *zs, ZIPmethod *newMethod )
{
	ZIPmethod* iMethod = zs->firstMethod;
	while ( iMethod )
	{
		if ( iMethod == newMethod )
		{
			fprintf (stderr, "Compression method already registered\n" );
			return 0;
		}
		iMethod = iMethod->next;
	}

	/* Insert at head as its easier!
	 * TODO: if we had lots of methodIds or lots of files we may want to improve the methodId search behaviour with a binary search etc. */
	newMethod->next = zs->firstMethod;
	zs->firstMethod = newMethod;

	return 1;
}

/***************************************************************************
 * zs_writeentry:
 *
 * Write ZIP archive entry contained in a memory buffer using the
 * specified compression methodId.
 *
 * The methodId argument specifies the compression methodId to be used for
 * this entry.  Possible values:
 *   Z_STORE - no compression
 *   Z_DEFLATE - deflate compression
 *
 * The entry modified time (modtime) is stored in UTC.
 *
 * If specified, writestatus will be set to the output of write() when
 * a write error occurs, otherwise it will be set to 0.
 *
 * Return pointer to ZIPentry on success and NULL on error.
 ***************************************************************************/
ZIPentry *
zs_writeentry ( ZIPstream *zstream, uint8_t *entry, int64_t entrySize,
		char *name, time_t modtime, int methodId, ssize_t *writestatus )
{
	if ( writestatus )
		*writestatus = 0;

	if ( ! zstream || ! name )
		return NULL;

	if ( entrySize > 0xFFFFFFFF )
	{
		fprintf (stderr, "zs_writeentry(%s): Individual entries cannot exceed %lld bytes\n",
				(name) ? name : "", (long long) 0xFFFFFFFF);
		return NULL;
	}

	ZIPentry *zentry = zs_allocateentry( zstream, name, modtime );
	if ( zentry == NULL )
		return NULL;

	/* Calculate, or continue calculation of, CRC32 of original data */
	zentry->CRC32 = crc32 (zentry->CRC32, entry, entrySize);

	/* Process entry data depending on methodId */
	if ( (zentry->impl = zs_initmethod( zstream, methodId )) == NULL )
		return NULL;

	if ( !zentry->impl->entrybegin( zentry->impl, zstream, zentry ) )
		return NULL;

	int32_t writeBufferSize = zentry->impl->process( zentry->impl, entry, entrySize, NULL, NULL, 0 );
	if ( writeBufferSize < 0 )
	{
		fprintf (stderr, "zs_writeentry: Buffer size error = %d\n", writeBufferSize );
		return NULL;
	}

	//If we don't have a buffer size this must a be a 'store'
	uint8_t* writeBuffer = writeBufferSize ? (uint8_t*)malloc(writeBufferSize)
			                               : entry;
	if ( writeBuffer == NULL )
	{
		fprintf (stderr, "zs_writeentry: Error allocating deflation buffer\n");
		return NULL;
	}

	int32_t writeSize = writeBufferSize ? zentry->impl->process( zentry->impl, entry, NULL, writeBuffer, writeBufferSize, 1 )
			                            : entrySize;
	if ( writeSize < 0 )
		return NULL;

	zentry->CompressedSize += writeSize;
	zentry->UncompressedSize += entrySize;

	/* Write the Local File Header */
	if ( !zs_writeheader( zstream, zentry, writestatus ) )
	{
		/* Free memory if allocated in this function */
		if ( writeBuffer != entry )
			free (writeBuffer);

		return NULL;
	}

	/* Write entry data */
	int64_t lwritestatus = zs_writedata (zstream, writeBuffer, writeBufferSize);

	/* Free memory if allocated in this function */
	free (writeBuffer);

	if ( lwritestatus != writeBufferSize )
	{
		fprintf (stderr, "Error writing ZIP entry data (%d): %s\n",
				zstream->fd, strerror(errno));

		if ( writestatus )
			*writestatus = (ssize_t)lwritestatus;

		return NULL;
	}


	return zentry;
}  /* End of zs_writeentry() */


/***************************************************************************
 * zs_entrybegin:
 *
 * Begin a streaming entry by writing a Local File Header to the
 * output stream.  The modtime argument sets the modification time
 * stamp for the entry.
 *
 * The methodId argument specifies the compression methodId to be used
 * for this entry.  This argument can be:
 *   Z_STORE   - no compression
 *   Z_DEFLATE - deflate compression
 *
 * The entry modified time (modtime) is stored in UTC.
 *
 * If specified, writestatus will be set to the output of write() when
 * a write error occurs, otherwise it will be set to 0.
 *
 * Return pointer to ZIPentry on success and NULL on error.
 ***************************************************************************/
ZIPentry *
zs_entrybegin ( ZIPstream *zstream, char *name, time_t modtime, int methodId,
		ssize_t *writestatus )
{
	if ( writestatus )
		*writestatus = 0;

	if ( ! zstream | ! name )
		return NULL;

	/* Allocate and initialize new entry */
	ZIPentry *zentry = zs_allocateentry( zstream, name, modtime );
	if ( zentry == NULL )
	{
		return NULL;
	}

	/* Set bit to denote streaming */
	BIT_SET (zentry->GeneralFlag, 3);

	/* Process entry data depending on methodId */
	if ( (zentry->impl = zs_initmethod( zstream, methodId )) == NULL )
	{
		return NULL;
	}

	if ( !zentry->impl->entrybegin( zentry->impl, zstream, zentry ) )
	{
		return NULL;
	}

	if ( !zs_writeheader( zstream, zentry, writestatus ) )
	{
		return NULL;
	}

	return zentry;
}  /* End of zs_entrybegin() */


/***************************************************************************
 * zs_entrydata:
 *
 * Write a chunk of entry data, of size entrySize, to the output
 * stream according to the parameters already set for the stream and
 * entry.
 *
 * If the call contains the final data for this entry, the final
 * argument should be set to true (1) to flush internal buffers.  If
 * more data is expected for this stream, the final argument should be
 * false (0).  Alternatively, internal buffers will be flushed if
 * entry is NULL and entrySize is 0.
 *
 * If specified, writestatus will be set to the output of write() when
 * a write error occurs, otherwise it will be set to 0.
 *
 * Return pointer to ZIPentry on success and NULL on error.
 ***************************************************************************/
ZIPentry *
zs_entrydata ( ZIPstream *zstream, ZIPentry *zentry, uint8_t *entry,
		int64_t entrySize, int final, ssize_t *writestatus )
{
	if ( writestatus )
		*writestatus = 0;

	if ( ! zstream | ! zentry )
		return NULL;

	/* Implied data flush */
	if ( entry == NULL && entrySize == 0 )
		final = 1;

	/* Calculate, or continue calculation of, CRC32 */
	if ( entry )
	{
		zentry->CRC32 = crc32 (zentry->CRC32, (uint8_t *)entry, entrySize);
	}

	/* Process entry data depending on methodId */
	int32_t writeSize = zentry->impl->process( zentry->impl, entry, entrySize, zstream->buffer, sizeof(zstream->buffer), final );
	while ( writeSize > 0 )
	{
		int64_t lwritestatus = zs_writedata (zstream, zstream->buffer, writeSize);
		if ( lwritestatus != writeSize )
		{
			fprintf (stderr, "zs_entrydata: Error writing ZIP entry data (%d): %s\n",
					zstream->fd, strerror(errno));

			if ( writestatus )
				*writestatus = (ssize_t)lwritestatus;

			return NULL;
		}

		zentry->CompressedSize += writeSize;
		writeSize = zentry->impl->process( zentry->impl, entry, NULL, zstream->buffer, sizeof(zstream->buffer), final );
	}

	//Check for error in processing data
	if ( writeSize < 0 )
	{
		fprintf (stderr, "zs_entrydata: Process failed: %s\n", strerror(errno));
		return NULL;
	}

	zentry->UncompressedSize += entrySize;

	return zentry;

}  /* End of zs_entrydata() */


/***************************************************************************
 * zs_entryflush:
 *
 * Flush any buffered data for a streaming entry.  This is a simple
 * wrapper for zs_entrydata() directing a flush.
 *
 * If specified, writestatus will be set to the output of write() when
 * a write error occurs, otherwise it will be set to 0.
 *
 * Return pointer to ZIPentry on success and NULL on error.
 ***************************************************************************/
ZIPentry *
zs_entryflush ( ZIPstream *zstream, ZIPentry *zentry, ssize_t *writestatus)
{
	return zs_entrydata ( zstream, zentry, NULL, 0, 1, writestatus );
}  /* End of zs_entryflush() */


/***************************************************************************
 * zs_entryend:
 *
 * End a streaming entry by writing a Data Description record to
 * output stream.
 *
 * If specified, writestatus will be set to the output of write() when
 * a write error occurs, otherwise it will be set to 0.
 *
 * Return pointer to ZIPentry on success and NULL on error.
 ***************************************************************************/
ZIPentry *
zs_entryend ( ZIPstream *zstream, ZIPentry *zentry, ssize_t *writestatus)
{
	if ( writestatus )
		*writestatus = 0;

	if ( ! zstream || ! zentry )
		return NULL;

	zentry->impl->entryend( zentry->impl, zstream, zentry );

	/* Write Data Description */
	int32_t packed = 0;
	packuint32 (zstream, &packed, DATADESCRIPTIONSIG);       /* Data Description signature */
	packuint32 (zstream, &packed, zentry->CRC32);            /* CRC-32 value of entry */
	packuint32 (zstream, &packed, zentry->CompressedSize);   /* Compressed entry size */
	packuint32 (zstream, &packed, zentry->UncompressedSize); /* Uncompressed entry size */

	int64_t lwritestatus = zs_writedata (zstream, zstream->buffer, packed);
	if ( lwritestatus != packed )
	{
		fprintf (stderr, "Error writing streaming ZIP data description: %s\n", strerror(errno));

		if ( writestatus )
			*writestatus = (ssize_t)lwritestatus;

		return NULL;
	}

	return zentry;
}  /* End of zs_entryend() */

/***************************************************************************
 * zs_finish:
 *
 * Write end of ZIP archive structures (Central Directory, etc.).
 *
 * ZIP64 structures will be added to the Central Directory when the
 * total length of the archive exceeds 0xFFFFFFFF bytes.
 *
 * If specified, writestatus will be set to the output of write() when
 * a write error occurs, otherwise it will be set to 0.
 *
 * Returns 0 on success and non-zero on error.
 ***************************************************************************/
int
zs_finish ( ZIPstream *zstream, ssize_t *writestatus )
{
	ZIPentry *zentry;
	int64_t lwritestatus;
	int packed;

	uint64_t cdsize;
	uint64_t zip64endrecord;
	int zip64 = 0;

	if ( writestatus )
		*writestatus = 0;

	if ( ! zstream )
		return -1;

	/* Store offset of Central Directory */
	zstream->CentralDirectoryOffset = zstream->WriteOffset;

	zentry = zstream->FirstEntry;
	while ( zentry )
	{
		zip64 = ( zentry->LocalHeaderOffset > 0xFFFFFFFF ) ? 1 : 0;

		/* Write Central Directory Header, packing into write buffer and swapped to little-endian order */
		packed = 0;
		packuint32 (zstream, &packed, CENTRALHEADERSIG);    /* Central File Header signature */
		packuint16 (zstream, &packed, 0);                   /* Version made by */
		packuint16 (zstream, &packed, zentry->ZipVersion);
		packuint16 (zstream, &packed, zentry->GeneralFlag);
		packuint16 (zstream, &packed, zentry->CompressionMethod);
		packuint16 (zstream, &packed, zentry->DOSTime);     /* DOS file modification time */
		packuint16 (zstream, &packed, zentry->DOSDate);     /* DOS file modification date */
		packuint32 (zstream, &packed, zentry->CRC32);       /* CRC-32 value of entry */
		packuint32 (zstream, &packed, zentry->CompressedSize); /* Compressed entry size */
		packuint32 (zstream, &packed, zentry->UncompressedSize); /* Uncompressed entry size */
		packuint16 (zstream, &packed, zentry->NameLength);  /* File/entry name length */
		packuint16 (zstream, &packed, zentry->ExtraDataSize + (zip64 ? 12 : 0) ); /* Extra field length, switch for ZIP64 */
		packuint16 (zstream, &packed, 0);                   /* File/entry comment length */
		packuint16 (zstream, &packed, 0);                   /* Disk number start */
		packuint16 (zstream, &packed, 0);                   /* Internal file attributes */
		packuint32 (zstream, &packed, 0);                   /* External file attributes */
		packuint32 (zstream, &packed, ( zip64 ) ?
				0xFFFFFFFF : zentry->LocalHeaderOffset); /* Relative offset of Local Header */

		/* File/entry name */
		memcpy (zstream->buffer+packed, zentry->Name, zentry->NameLength); packed += zentry->NameLength;

		if ( zip64 )  /* ZIP64 Extra Field */
		{
			packuint16 (zstream, &packed, 1);      /* Extra field ID, 1 = ZIP64 */
			packuint16 (zstream, &packed, 8);      /* Extra field data length */
			packuint64 (zstream, &packed, zentry->LocalHeaderOffset); /* Offset to Local Header */
		}

		lwritestatus = zs_writedata (zstream, zstream->buffer, packed);
		if ( lwritestatus != packed )
		{
			fprintf (stderr, "Error writing ZIP central directory header: %s\n", strerror(errno));

			if ( writestatus )
				*writestatus = (ssize_t)lwritestatus;

			return -1;
		}

		if ( zentry->impl->writeExtraData )
		{
			zentry->impl->writeExtraData( zentry->impl, zstream, zentry );
		}

		zentry = zentry->next;
	}

	/* Calculate size of Central Directory */
	cdsize = zstream->WriteOffset - zstream->CentralDirectoryOffset;

	/* Add ZIP64 structures if offset to Central Directory is beyond limit */
	if ( zstream->CentralDirectoryOffset > 0xFFFFFFFF )
	{
		/* Note offset of ZIP64 End of Central Directory Record */
		zip64endrecord = zstream->WriteOffset;

		/* Write ZIP64 End of Central Directory Record, packing into write buffer and swapped to little-endian order */
		packed = 0;
		packuint32 (zstream, &packed, ZIP64ENDRECORDSIG); /* ZIP64 End of Central Dir record */
		packuint64 (zstream, &packed, 44);                /* Size of this record after this field */
		packuint16 (zstream, &packed, 30);                /* Version made by */
		packuint16 (zstream, &packed, 45);                /* Version needed to extract */
		packuint32 (zstream, &packed, 0);                 /* Number of this disk */
		packuint32 (zstream, &packed, 0);                 /* Disk with start of the CD */
		packuint64 (zstream, &packed, zstream->EntryCount); /* Number of CD entries on this disk */
		packuint64 (zstream, &packed, zstream->EntryCount); /* Total number of CD entries */
		packuint64 (zstream, &packed, cdsize);            /* Size of Central Directory */
		packuint64 (zstream, &packed, zstream->CentralDirectoryOffset); /* Offset to Central Directory */

		lwritestatus = zs_writedata (zstream, zstream->buffer, packed);
		if ( lwritestatus != packed )
		{
			fprintf (stderr, "Error writing ZIP64 end of central directory record: %s\n", strerror(errno));

			if ( writestatus )
				*writestatus = (ssize_t)lwritestatus;

			return -1;
		}

		/* Write ZIP64 End of Central Directory Locator, packing into write buffer and swapped to little-endian order */
		packed = 0;
		packuint32 (zstream, &packed, ZIP64ENDLOCATORSIG); /* ZIP64 End of Central Dir Locator */
		packuint32 (zstream, &packed, 0);                  /* Number of disk w/ ZIP64 End of CD */
		packuint64 (zstream, &packed, zip64endrecord);     /* Offset to ZIP64 End of CD */
		packuint32 (zstream, &packed, 1);                  /* Total number of disks */

		lwritestatus = zs_writedata (zstream, zstream->buffer, packed);
		if ( lwritestatus != packed )
		{
			fprintf (stderr, "Error writing ZIP64 end of central directory locator: %s\n", strerror(errno));

			if ( writestatus )
				*writestatus = (ssize_t)lwritestatus;

			return -1;
		}
	}

	/* Write End of Central Directory Record, packing into write buffer and swapped to little-endian order */
	packed = 0;
	packuint32 (zstream, &packed, ENDHEADERSIG);     /* End of Central Dir signature */
	packuint16 (zstream, &packed, 0);                /* Number of this disk */
	packuint16 (zstream, &packed, 0);                /* Number of disk with CD */
	packuint16 (zstream, &packed, zstream->EntryCount); /* Number of entries in CD this disk */
	packuint16 (zstream, &packed, zstream->EntryCount); /* Number of entries in CD */
	packuint32 (zstream, &packed, cdsize);           /* Size of Central Directory */
	packuint32 (zstream, &packed, (zstream->CentralDirectoryOffset > 0xFFFFFFFF) ?
			0xFFFFFFFF : zstream->CentralDirectoryOffset); /* Offset to start of CD */
	packuint16 (zstream, &packed, 0);                /* ZIP file comment length */

	lwritestatus = zs_writedata (zstream, zstream->buffer, packed);
	if ( lwritestatus != packed )
	{
		fprintf (stderr, "Error writing end of central directory record: %s\n", strerror(errno));

		if ( writestatus )
			*writestatus = (ssize_t)lwritestatus;

		return -1;
	}

	return 0;
}  /* End of zs_finish() */


/***************************************************************************
 * zs_writedata:
 *
 * Write data to output descriptor in blocks of ZS_WRITE_SIZE bytes.
 *
 * The ZIPstream.WriteOffset value will be incremented accordingly.
 *
 * Return number of bytes written on success and return value of
 * write() on error.
 ***************************************************************************/
int64_t
zs_writedata ( ZIPstream *zstream, uint8_t *writeBuffer, int64_t writeBufferSize )
{
	if ( !zstream || !writeBuffer )
		return 0;

	/* Write blocks of ZS_WRITE_SIZE until done */
	int64_t written = 0;
	while ( written < writeBufferSize )
	{
		size_t writeLength = ( (writeBufferSize - written) > ZS_WRITE_SIZE ) ?
				ZS_WRITE_SIZE : (writeBufferSize - written);

		ssize_t lwritestatus = write (zstream->fd, writeBuffer+written, writeLength);
		if ( lwritestatus <= 0 )
		{
			return lwritestatus;
		}

		zstream->WriteOffset += lwritestatus;
		written += lwritestatus;
	}

	return written;
}  /* End of zs_writedata() */


/* DOS time start date is January 1, 1980 */
#define DOSTIME_STARTDATE  0x00210000L

/***************************************************************************
 * zs_datetime_unixtodos:
 *
 * Convert Unix time_t to 4 byte DOS date and time.
 *
 * Routine adapted from sources:
 *  Copyright (C) 2006 Michael Liebscher <johnnycanuck@users.sourceforge.net>
 *
 * Return converted 4-byte quantity on success and 0 on error.
 ***************************************************************************/
static uint32_t
zs_datetime_unixtodos ( time_t t )
{
	struct tm s;

	if ( gmtime_r (&t, &s) == NULL )
		return 0;

	s.tm_year += 1900;
	s.tm_mon += 1;

	return ( ((s.tm_year) < 1980) ? DOSTIME_STARTDATE :
			(((uint32_t)(s.tm_year) - 1980) << 25) |
			((uint32_t)(s.tm_mon) << 21) |
			((uint32_t)(s.tm_mday) << 16) |
			((uint32_t)(s.tm_hour) << 11) |
			((uint32_t)(s.tm_min) << 5) |
			((uint32_t)(s.tm_sec) >> 1) );
}


/***************************************************************************
 * Byte swapping routine:
 *
 * Functions for generalized, in-place byte swapping from host order
 * to little-endian.  A run-time test of byte order is conducted on
 * the first usage and a static variable is used to store the result
 * for later use.
 *
 * The byte-swapping requires memory-aligned quantities.
 *
 ***************************************************************************/
static void
zs_htolx ( void *data, int size )
{
	static int le = -1;
	int16_t host = 1;

	uint16_t *data2;
	uint32_t *data4;
	uint32_t h0, h1;

	/* Determine byte order, test for little-endianness */
	if ( le < 0 )
	{
		le = (*((int8_t *)(&host)));
	}

	/* Swap bytes if not little-endian, requires memory-aligned quantities */
	if ( le == 0 )
	{
		switch ( size )
		{
		case 2:
			data2 = (uint16_t *) data;
			*data2=(((*data2>>8)&0xff) | ((*data2&0xff)<<8));
			break;
		case 4:
			data4 = (uint32_t *) data;
			*data4=(((*data4>>24)&0xff) | ((*data4&0xff)<<24) |
					((*data4>>8)&0xff00) | ((*data4&0xff00)<<8));
			break;
		case 8:
			data4 = (uint32_t *) data;

			h0 = data4[0];
			h0 = (((h0>>24)&0xff) | ((h0&0xff)<<24) |
					((h0>>8)&0xff00) | ((h0&0xff00)<<8));

			h1 = data4[1];
			h1 = (((h1>>24)&0xff) | ((h1&0xff)<<24) |
					((h1>>8)&0xff00) | ((h1&0xff00)<<8));

			data4[0] = h1;
			data4[1] = h0;
			break;
		}
	}
}

#endif /* NOFDZIP */
