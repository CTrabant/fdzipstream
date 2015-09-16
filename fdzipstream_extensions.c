
#include "fdzipstream_extensions.h"

#include <assert.h>
#include <errno.h>
#include <memory.h>
#include <stdlib.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#if FDZIPSTREAM_AESDEFLATE

/* Deflate implementation adds z_stream */
typedef struct zipaesdeflateimpl_s
{
	ZIPmethod method; //< Base
	//z_stream zlstream;
} ZIPaesdeflateimpl;

/* AES-Deflate implementation */
static ZIPmethodimpl* zs_aesdeflate_init( ZIPstream *zstream, int method );
static void zs_aesdeflate_free( ZIPmethodimpl* this );
static int32_t zs_aesdeflate_entryend( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry );
static int32_t zs_aesdeflate_process( struct zipmethodimpl_s* this, uint8_t *entry, int64_t entrySize, uint8_t* writeBuffer, int64_t writeBufferSize, int32_t final );
static int32_t zs_aesdeflate_entrybegin( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry );
static int32_t zs_aesdeflate_writeExtraData( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry );

struct AesCtrState
{
	size_t blockIndex;
    uint8_t ivec[AES_BLOCK_SIZE];  /* ivec[0..7] is the IV, ivec[8..15] is the big-endian counter */
    unsigned int num;
    uint8_t ecount[AES_BLOCK_SIZE];
};

static void initAesCtrState( struct AesCtrState* aesState)
{
    aesState->blockIndex = 0; //< ZIP AES uses the block index as the nondex/iv/initialiseVEctor

    // aes_ctr128_encrypt requires 'num' and 'ecount' set to zero on the first call.
    aesState->num = 0;
    memset(aesState->ecount, 0, sizeof(aesState->ecount));
}

static void updateAesCtrState( struct AesCtrState* aesState )
{
    //Update the block index
    memset(aesState->ivec, 0, sizeof(aesState->ivec));
	((uint64_t*)aesState->ivec)[0] = ++aesState->blockIndex;
}

typedef struct zipdeflateimpl_s
{
	ZIPmethodimpl this; //< Base
	ZIPmethodimpl* deflate; //< Use deflate method
	int version; //< Whether we ae AES1 or AES2

	uint8_t salt[16]; //< 256-bit encryption has a 16byte key salt
	uint8_t key[32];
	uint8_t initialisationVector[32];
	uint8_t passwordVerification[2]; //< Password verification prior to decryption
	uint8_t authenticationCode[10]; //<

	HMAC_CTX hmaxCtx; //< HMAC authentication hash code calculation context

	struct AesCtrState aesState;
	AES_KEY aesKey;
	size_t aesPartialBlockCacheSize;
	uint8_t aesPartialBlockCache[AES_BLOCK_SIZE];

} ZIPdeflateimpl;

static ZIPdeflateimpl aesdeflateImpl = { { { zs_aesdeflate_init, zs_aesdeflate_free}, zs_aesdeflate_entrybegin, zs_aesdeflate_process, zs_aesdeflate_entryend, zs_aesdeflate_writeExtraData } };

int print_hex(uint8_t *buf, int len)
{
	int i;
	int n;

	for(i=0,n=0;i<len;i++){
		if(n > 7){
			printf("\n");
			n = 0;
		}
		printf("0x%02x, ",buf[i]);
		n++;
	}
	printf("\n");

	return(0);
}

static ZIPmethodimpl* zs_aesdeflate_init( ZIPstream *zstream, int method )
{
	if ( /*method != ZS_AES1_DEFLATE &&*/
			 method != ZS_AES2_DEFLATE )
		return NULL;

	ZIPdeflateimpl *this = (ZIPdeflateimpl *) calloc (1, sizeof(aesdeflateImpl));
	memcpy( this, &aesdeflateImpl, sizeof(*this) );
	this->version = (method == ZS_AES2_DEFLATE) ? 2 : 1;
	this->aesPartialBlockCacheSize = 0;

    ENGINE_load_builtin_engines();
    ENGINE_register_all_complete();

    HMAC_CTX_init( &this->hmaxCtx );

	this->deflate = zs_initmethod( zstream, ZS_DEFLATE );
	assert(this->deflate);


	return (ZIPmethodimpl*)this;
}

static void zs_aesdeflate_free( ZIPmethodimpl* thisbase )
{
	ZIPdeflateimpl* this = (ZIPdeflateimpl*)thisbase;
    HMAC_CTX_cleanup(&this->hmaxCtx);

    zs_freeImpl( this->deflate );
    free( this );
}

static int32_t zs_aesdeflate_entrybegin( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry )
{
	ZIPdeflateimpl* this = (ZIPdeflateimpl*)thisbase;

	// Forward to deflate
	if ( !this->deflate->entrybegin( this->deflate, zstream, zentry ) )
		return NULL;

	//http://www.winzip.com/aes_info.html#pwd-verify
	zentry->CompressionMethod = 99; //, AES type
	zentry->ExtraDataSize += 11; //< Size fo AES
	zentry->ZipVersion = 51; //<TODO: Minimum version fo AES?

	zentry->GeneralFlag |= 0x01; //, Flag the encrypted bit

	return 1;
}

static int32_t zs_aesdeflate_writeExtraData( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry )
{
	ZIPdeflateimpl* this = (ZIPdeflateimpl*)thisbase;

	// Forward to deflate
	if ( this->deflate->writeExtraData != NULL
			&& !this->deflate->writeExtraData( this->deflate, zstream, zentry ) )
	{
		return NULL;
	}

	/* Write Central Directory Header, packing into write buffer and swapped to little-endian order */
	int packed = 0;
	packuint16 (zstream, &packed, 0x9901); /* Extra field header ID (0x9901)*/
	packuint16 (zstream, &packed, 7); /* Data size (currently 7, but subject to possible increase in the future) */
	packuint16 (zstream, &packed, this->version/*AE-1/2*/); /* Integer version number specific to the zip vendor */
	packuint16 (zstream, &packed,('A')|('E'<<8u) ); /* 2-character vendor ID - must be 'AE' */
	packuint8 (zstream, &packed, 0x03 /*256-bit*/); /* Integer mode value indicating AES encryption strength */
	packuint16 (zstream, &packed, ZS_DEFLATE ); /* The actual compression method used to compress the file */

	int64_t lwritestatus = zs_writedata (zstream, zstream->buffer, packed);
	if ( lwritestatus != packed )
	{
		fprintf (stderr, "Error writing ZIP AES extra data: %s\n", strerror(errno));
		return NULL;
	}

	return 1;
}

/*
 * 	entry & entrySize = start of entry block
 * 	entry & !entrySize = continuation of entry block
 * 	final = flush/ entry end
 */
static int32_t zs_aesdeflate_process( struct zipmethodimpl_s* thisbase, uint8_t *entry, int64_t entrySize, uint8_t* writeBuffer, int64_t writeBufferSize, int32_t final )
{
	ZIPdeflateimpl* this = (ZIPdeflateimpl*)thisbase;

	// Forward to deflate
	// NOTE:If we had a partial block data we will append to it
	int32_t writeSize = this->deflate->process( this->deflate, entry, entrySize
			, writeBuffer+this->aesPartialBlockCacheSize, writeBufferSize-this->aesPartialBlockCacheSize, final );
	if ( writeSize < 0 ) //< Check for failure
	{
		return writeSize;
	}

	writeSize += this->aesPartialBlockCacheSize;
	if ( writeSize == 0  ) //, Check for no data
	{
		return 0;
	}

	//Request for maximum desired writeBufferSize capacity
	if ( !writeBuffer || !writeBufferSize )
	{
		return writeSize; //< Return deflate size plus any extra storage we may already have
	}

	//If we fill the buffer exactly we expect deflate will have some more data for now...
	//TODO: We don't handle fragment deflate of the final data well as it could end up being exactly on the buffer boundary! i.e. we need to add means to determine if deflate() actually finished.
	if ( writeSize == writeBufferSize )
		final = 0;

	//Restore the partialBlock cache into out buffer
	if ( this->aesPartialBlockCacheSize )
	{
		memcpy( writeBuffer, this->aesPartialBlockCache, this->aesPartialBlockCacheSize );
		this->aesPartialBlockCacheSize = 0;
	}

	const size_t cFullBlockCount = (writeSize / AES_BLOCK_SIZE);
	const size_t cPartialBlockSize = (writeSize - (cFullBlockCount*AES_BLOCK_SIZE));
	uint8_t* writeBufferCursor = writeBuffer;

	size_t iBlock;
	for ( iBlock = 0; iBlock < cFullBlockCount; ++iBlock, writeBufferCursor += AES_BLOCK_SIZE )
	{
		updateAesCtrState( &this->aesState );
		AES_ctr128_encrypt( writeBufferCursor, writeBufferCursor, AES_BLOCK_SIZE, &this->aesKey, this->aesState.ivec, this->aesState.ecount, &this->aesState.num);
	}

	if ( cPartialBlockSize )
	{
		if ( final ) //< If flush operation then we will write the remaining block storage if any
		{
			updateAesCtrState( &this->aesState );
			AES_ctr128_encrypt( writeBufferCursor, writeBufferCursor, cPartialBlockSize, &this->aesKey, this->aesState.ivec, this->aesState.ecount, &this->aesState.num);
		}
		else // We cache the extra block data so we can complete a full block on the next call
		{
			assert( sizeof(this->aesPartialBlockCache) >= cPartialBlockSize );
			memcpy( this->aesPartialBlockCache, writeBufferCursor, cPartialBlockSize );
			this->aesPartialBlockCacheSize = cPartialBlockSize;

			//Update that we didn't actually write this block yet
			writeSize -= cPartialBlockSize;
		}
	}

	//UPdate the data authentication Hash MAC (Works like CRC)
    HMAC_Update(&this->hmaxCtx, writeBuffer, writeSize );

	return writeSize;
}

static int32_t zs_aesdeflate_entryend( ZIPmethodimpl* thisbase, ZIPstream *zstream, ZIPentry *zentry )
{
	ZIPdeflateimpl* this = (ZIPdeflateimpl*)thisbase;

	// Forward to deflate
	if ( !this->deflate->entryend( this->deflate, zstream, zentry ) )
		return NULL;

	uint8_t hmacResult[65];
	uint32_t hmacResultLength;
    HMAC_Final( &this->hmaxCtx, hmacResult, &hmacResultLength);

    //assert( hmacResultLength == 10 );
    //TODO: no need to store this really but here for debugging!
    memcpy( this->authenticationCode, hmacResult, sizeof(this->authenticationCode) );

	int64_t lwritestatus = zs_writedata( zstream, this->authenticationCode, sizeof(this->authenticationCode) );
	if ( lwritestatus != sizeof(this->authenticationCode) )
	{
		fprintf (stderr, "Error writing streaming ZIP aes authentication code: %s\n", strerror(errno));
		return NULL;
	}
	zentry->CompressedSize += sizeof(this->authenticationCode);

	//Version 2 doens't include the normal CRC
	if ( this->version == 2 )
	{
		zentry->CRC32 = 0;
	}

	return 1;
}

static int32_t generateNewKey( ZIPdeflateimpl* this, const char* password )
{
	//Generate our new files 'salt' key
	RAND_bytes( this->salt, sizeof(this->salt) );

	//http://www.winzip.com/aes_info.htm#base-format
	//http://www.winzip.com/aes_tips.html

	uint8_t buf[sizeof(this->key)+sizeof(this->initialisationVector)+sizeof(this->passwordVerification)];
	uint8_t* key = buf;
	uint8_t* initialisationVector = key+sizeof(this->key); //< init vector
	uint8_t* passwordVerification = initialisationVector+sizeof(this->initialisationVector);

	//http://www.gladman.me.uk/cryptography_technology/fileencrypt/
	// -"HMAC-SHA1 for authentication and key derivation from a password and a salt according to RFC2898"
	const int cIterationCount = 1000; //< iteration count
	if ( !PKCS5_PBKDF2_HMAC_SHA1(password, strlen(password), (uint8_t*)this->salt, sizeof(this->salt), cIterationCount, sizeof(buf), buf) )
	{
		fprintf (stderr, "PKCS5_PBKDF2_HMAC_SHA1 failed: %s\n", strerror(errno));
		return 0; ///< fail!
	}

	memcpy( this->key, key, sizeof(this->key) );
	memcpy( this->initialisationVector, initialisationVector, sizeof(this->initialisationVector) );
	memcpy( this->passwordVerification, passwordVerification, sizeof(this->passwordVerification) );
	return 1;
}

int32_t zs_aesdeflate_setpassword( ZIPstream *zstream, ZIPentry *zentry, const char* password )
{
	if ( zentry->impl->method.init != zs_aesdeflate_init )
	{
		assert( 0 == "Entry is not AESDeflate compressed" ); //< Must be an AES deflate!
		return 0; ///< fail!
	}

	ZIPdeflateimpl* this = (ZIPdeflateimpl*)zentry->impl;

	if ( !generateNewKey( this, password ) )
	{
		fprintf (stderr, "Error generating encryption key: %s\n", strerror(errno));
		return NULL;
	}

	//Intiialise authentication code
    HMAC_Init_ex( &this->hmaxCtx, this->initialisationVector, 32, EVP_sha1(), NULL);

	//Now encrypt the data
	if ( AES_set_encrypt_key(this->key, 256, &this->aesKey) != 0 )
	{
		assert(0);
		return NULL;
	}

	initAesCtrState( &this->aesState );

	int64_t saltStatus = zs_writedata( zstream, this->salt, sizeof(this->salt) );
	if ( saltStatus != sizeof(this->salt) )
	{
		fprintf (stderr, "Error writing streaming ZIP salt: %s\n", strerror(errno));
		return NULL;
	}

	int64_t passwordVerificationStatus = zs_writedata( zstream, this->passwordVerification, sizeof(this->passwordVerification) );
	if ( passwordVerificationStatus != sizeof(this->passwordVerification) )
	{
		fprintf (stderr, "Error writing streaming ZIP passwordVerification: %s\n", strerror(errno));
		return NULL;
	}

	zentry->CompressedSize += sizeof(this->salt) + sizeof(this->passwordVerification);

	return 1;
}

#endif //END: FDZIPSTREAM_AESDEFLATE

size_t zs_extensions_register( ZIPstream *zs )
{
	size_t count = 0;

#if FDZIPSTREAM_AESDEFLATE
	if ( zs_registermethod( zs, (ZIPmethod*)&aesdeflateImpl ) )
		++count;
#endif

	return count;
}


