
/* Allow this code to be skipped by declaring NOFDZIP */
#ifndef NOFDZIP

/* Version: 2015.7.27 */

#ifndef FDZIPSTREAM_H
#define FDZIPSTREAM_H

#include <stdint.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set FDZIPSTREAM_REGISTEREXTENSIONS=1 to enable automatic support for enabled extensions in fdzipstream_extensions.h.
 *  Without this zs_registermethod() or zs_extensions_register(zs) can be used to register compression methods.
 */
#ifndef FDZIPSTREAM_REGISTEREXTENSIONS
#	define FDZIPSTREAM_REGISTEREXTENSIONS 0
#endif

#ifndef DEF_MEM_LEVEL
#  if MAX_MEM_LEVEL >= 8
#    define DEF_MEM_LEVEL 8
#  else
#    define DEF_MEM_LEVEL MAX_MEM_LEVEL
#  endif
#endif

/* ZIP record type signatures */
#define LOCALHEADERSIG      (0x04034b50)
#define DATADESCRIPTIONSIG  (0x08074b50)
#define CENTRALHEADERSIG    (0x02014b50)
#define ZIP64ENDRECORDSIG   (0x06064b50)
#define ZIP64ENDLOCATORSIG  (0x07064b50)
#define ENDHEADERSIG        (0x06054b50)

/* Compression methods */
#define ZS_STORE      0
#define ZS_DEFLATE    8

/* Maximum single size to write(), 1 MiB */
#define ZS_WRITE_SIZE 1048576

/* Multi-use stream buffer, 256 KiB */
#define ZS_BUFFER_SIZE 262144

/* Maximum length of file/entry name including NULL terminator */
#define ZENTRY_NAME_LENGTH 256

/* ZIP archive entry */
typedef struct zipentry_s
{
  uint16_t ZipVersion;        /* Version needed to extract, (Default: 2.0) */
  uint16_t GeneralFlag;       /* General purpose bit flag */
  uint16_t CompressionMethod; /* Compression methodId */
  uint16_t DOSDate;
  uint16_t DOSTime;
  uint32_t CRC32;
  uint64_t CompressedSize;
  uint64_t UncompressedSize;
  uint64_t LocalHeaderOffset;
  uint16_t NameLength;
  char Name[ZENTRY_NAME_LENGTH];
  struct zipmethod_s* method;
  struct zipentry_s *next;
} ZIPentry;

/* ZIP output stream managment */
typedef struct zipstream_s
{
  int fd;
  int64_t WriteOffset;
  int64_t CentralDirectoryOffset;
  int32_t EntryCount;
  struct zipentry_s *FirstEntry;
  struct zipentry_s *LastEntry;
  struct zipmethod_s* firstimpl; /* First registered implementation type in linked list */
  unsigned char buffer[ZS_BUFFER_SIZE];
} ZIPstream;

/* Interface for a compression method implementation registered via zs_registermethod() */
typedef struct zipmethod_s
{
	int id;              ///< Compression method this method applies to i.e. ZS_DEFLATE or ZS_STORE etc
	struct zipmethod_s* next;  ///< [internal] Next registered method in linked list

	/* Function prototypes for compression method implementation */
	int32_t (*entrybegin)( ZIPentry *zentry );
	int32_t (*getwritebuffer)( ZIPentry *zentry, unsigned char *entry, int64_t entrysize, unsigned char** writebuffer, int64_t* writebuffersize );
	int32_t (*writeentrydata)( ZIPstream *zstream, ZIPentry *zentry, unsigned char *entry, int64_t entrysize, int final, ssize_t *writestatus );
	int32_t (*entryend)( ZIPentry *zentry );
} ZIPmethod;

extern ZIPstream * zs_init ( int fd, ZIPstream *zs );
extern void zs_free ( ZIPstream *zs );

/* Register an implementation
 * @return 1 on success, 0 on failure
 */
extern int zs_registermethod( ZIPstream *zs, ZIPmethod *methodImpl );

extern ZIPentry * zs_writeentry ( ZIPstream *zstream, unsigned char *entry, int64_t entrysize,
				  char *name, time_t modtime, int methodId, ssize_t *writestatus );

extern ZIPentry * zs_entrybegin ( ZIPstream *zstream, char *name,
				  time_t modtime, int methodId,
				  ssize_t *writestatus );

extern ZIPentry * zs_entrydata ( ZIPstream *zstream, ZIPentry *zentry,
				 unsigned char *entry, int64_t entrysize,
				 int final, ssize_t *writestatus );

extern ZIPentry * zs_entryflush ( ZIPstream *zstream, ZIPentry *zentry, ssize_t *writestatus );

extern ZIPentry * zs_entryend ( ZIPstream *zstream, ZIPentry *zentry,
				ssize_t *writestatus);

extern int zs_finish ( ZIPstream *zstream, ssize_t *writestatus );


#ifdef __cplusplus
}
#endif

#endif /* FDZIPSTREAM_H */

#endif /* NOFDZIP */

