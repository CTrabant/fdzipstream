/***************************************************************************
 * test_fdzipstream.c
 *
 * Self-contained regression/behavior test suite for fdzipstream.[ch].
 * No external test framework and no external tools (e.g. unzip) are
 * used; verification is done via the public API/struct fields and by
 * parsing the raw bytes of produced archives directly.
 *
 * Compile with:
 *   cc -Wall fdzipstream.c test_fdzipstream.c -o test_fdzipstream -lz
 *
 * Copyright 2019 CTrabant
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
 ***************************************************************************/

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include "fdzipstream.h"

static int failures = 0;
static int passes = 0;

#define CHECK(cond, msg)                                               \
  do                                                                   \
  {                                                                    \
    if (cond)                                                          \
    {                                                                  \
      passes++;                                                        \
    }                                                                  \
    else                                                               \
    {                                                                  \
      failures++;                                                      \
      fprintf (stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
    }                                                                  \
  } while (0)

/***************************************************************************
 * find_signature:
 *
 * Search buf[0..len) for the little-endian encoding of a ZIP record
 * signature (the signature macros in fdzipstream.h are host-order
 * uint32_t constants, e.g. 0x04034b50, which are always stored
 * little-endian in an actual ZIP archive).
 *
 * @return offset of first match at or after 'start', or -1 if not found.
 ***************************************************************************/
static long
find_signature (const uint8_t *buf, size_t len, uint32_t sig, long start)
{
  uint8_t needle[4];
  long i;

  needle[0] = (uint8_t)(sig & 0xFF);
  needle[1] = (uint8_t)((sig >> 8) & 0xFF);
  needle[2] = (uint8_t)((sig >> 16) & 0xFF);
  needle[3] = (uint8_t)((sig >> 24) & 0xFF);

  for (i = start; i + 4 <= (long)len; i++)
  {
    if (buf[i] == needle[0] && buf[i + 1] == needle[1] && buf[i + 2] == needle[2] &&
        buf[i + 3] == needle[3])
    {
      return i;
    }
  }

  return -1;
}

/***************************************************************************
 * build_and_read:
 *
 * Create a temporary file, invoke the supplied 'build' callback with
 * its file descriptor and an opaque argument (the callback is expected
 * to drive zs_init(), write whatever entries it wants, call zs_finish()
 * and zs_free()), then read the entire resulting file back into a
 * malloc'd buffer.
 *
 * The temp file is removed before returning.  Caller must free *buf.
 ***************************************************************************/
static void
build_and_read (void (*build) (int fd, void *arg), void *arg, uint8_t **buf, size_t *len)
{
  char path[] = "/tmp/test_fdzipstream.XXXXXX";
  int fd;
  FILE *f;
  long size;

  fd = mkstemp (path);
  if (fd < 0)
  {
    fprintf (stderr, "mkstemp failed: %s\n", strerror (errno));
    *buf = NULL;
    *len = 0;
    return;
  }

  build (fd, arg);
  close (fd);

  f = fopen (path, "rb");
  if (!f)
  {
    fprintf (stderr, "fopen failed: %s\n", strerror (errno));
    remove (path);
    *buf = NULL;
    *len = 0;
    return;
  }

  fseek (f, 0, SEEK_END);
  size = ftell (f);
  fseek (f, 0, SEEK_SET);

  *buf = malloc ((size_t)size);
  *len = fread (*buf, 1, (size_t)size, f);

  fclose (f);
  remove (path);
}

/* Argument type + top-level callback for build_and_read(): write a
 * single entry with the given data/name/method, then finish. */
typedef struct
{
  const void *data;
  size_t datalen;
  int method;
  const char *name;
} entry_spec;

static void
build_one_entry (int fd, void *arg)
{
  entry_spec *spec = (entry_spec *)arg;
  ZIPstream *z;
  int64_t ws;

  z = zs_init (fd, NULL);
  zs_writeentry (z, (uint8_t *)spec->data, spec->datalen, (char *)spec->name, time (NULL),
                 spec->method, &ws);
  zs_finish (z, &ws);
  zs_free (z);
}

static void
build_entrycount_zip64 (int fd, void *arg)
{
  ZIPstream *z;
  int64_t ws;

  (void)arg;

  z = zs_init (fd, NULL);
  z->EntryCount = 70000; /* force past the 16-bit boundary without 70000 real entries */
  zs_finish (z, &ws);
  zs_free (z);
}

static void
build_zip64_version_needed (int fd, void *arg)
{
  ZIPstream *z;
  ZIPentry *e;
  int64_t ws;

  (void)arg;

  z = zs_init (fd, NULL);
  e = zs_writeentry (z, (uint8_t *)"x", 1, "big.bin", time (NULL), ZS_STORE, &ws);
  if (e)
    e->LocalHeaderOffset = 0x100000000ULL; /* force this entry's zip64 flag */

  zs_finish (z, &ws);
  zs_free (z);
}

static void
build_finish_zero_entries (int fd, void *arg)
{
  ZIPstream *z;
  int64_t ws;

  (void)arg;

  z = zs_init (fd, NULL);
  CHECK (zs_finish (z, &ws) == 0, "finish_zero_entries: zs_finish on an empty stream succeeds");
  zs_free (z);
}

/***************************************************************************
 * Custom method used by several tests below: a trivial working method
 * (behaves like STORE) registered under a fresh method ID, to exercise
 * the pluggable init/process/finish machinery end-to-end.
 ***************************************************************************/
static int32_t
custom_init (ZIPstream *zstream, ZIPentry *zentry)
{
  (void)zstream;
  zentry->methoddata = (void *)1; /* sentinel: prove init() ran */
  return 0;
}

static int32_t
custom_process (ZIPstream *zstream, ZIPentry *zentry, uint8_t *entry, int64_t entrySize,
                int64_t *entryConsumed, uint8_t *writeBuffer, int64_t writeBufferSize)
{
  (void)zstream;
  (void)zentry;

  if (!entry || entrySize <= 0)
    return 0;

  if (entrySize < writeBufferSize)
    writeBufferSize = entrySize;

  memcpy (writeBuffer, entry, writeBufferSize);

  if (entryConsumed)
    *entryConsumed = writeBufferSize;

  return (int32_t)writeBufferSize;
}

static int custom_finish_called = 0;

static int32_t
custom_finish (ZIPstream *zstream, ZIPentry *zentry)
{
  (void)zstream;
  (void)zentry;
  custom_finish_called = 1;
  return 0;
}

/* A method whose init() always fails, used to test cleanup on failure. */
static int32_t
failing_init (ZIPstream *zstream, ZIPentry *zentry)
{
  (void)zstream;
  (void)zentry;
  return -1;
}

static int32_t
failing_process (ZIPstream *zstream, ZIPentry *zentry, uint8_t *entry, int64_t entrySize,
                 int64_t *entryConsumed, uint8_t *writeBuffer, int64_t writeBufferSize)
{
  (void)zstream;
  (void)zentry;
  (void)entry;
  (void)entrySize;
  (void)entryConsumed;
  (void)writeBuffer;
  (void)writeBufferSize;
  return 0;
}

/***************************************************************************
 * Tests
 ***************************************************************************/

static void
test_store_roundtrip (void)
{
  static const char data[] = "123456789";
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;
  uint8_t *buf;
  size_t len;
  entry_spec spec;
  long loc, dd, cen, end;

  /* Exercise the API directly to get a ZIPentry pointer for field checks */
  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  e = zs_writeentry (zs, (uint8_t *)data, strlen (data), "nums.txt", time (NULL), ZS_STORE, &ws);
  CHECK (e != NULL, "store_roundtrip: zs_writeentry succeeded");
  if (e)
  {
    CHECK (e->UncompressedSize == strlen (data), "store_roundtrip: UncompressedSize matches");
    CHECK (e->CompressedSize == e->UncompressedSize,
           "store_roundtrip: CompressedSize == Uncompressed");
    /* Known-answer CRC-32 check value for ASCII "123456789" */
    CHECK (e->CRC32 == 0xCBF43926UL, "store_roundtrip: CRC32 matches standard check value");
    CHECK (e->CRC32 == crc32 (0L, (const Bytef *)data, (uInt)strlen (data)),
           "store_roundtrip: CRC32 matches zlib's own crc32()");
  }
  zs_finish (zs, &ws);
  zs_free (zs);

  /* Structural byte-level check: all four record types present, in order */
  spec.data = data;
  spec.datalen = strlen (data);
  spec.method = ZS_STORE;
  spec.name = "nums.txt";
  build_and_read (build_one_entry, &spec, &buf, &len);

  CHECK (buf != NULL && len > 0, "store_roundtrip: archive produced");
  if (buf)
  {
    loc = find_signature (buf, len, LOCALHEADERSIG, 0);
    dd = (loc >= 0) ? find_signature (buf, len, DATADESCRIPTIONSIG, loc) : -1;
    cen = (dd >= 0) ? find_signature (buf, len, CENTRALHEADERSIG, dd) : -1;
    end = (cen >= 0) ? find_signature (buf, len, ENDHEADERSIG, cen) : -1;

    CHECK (loc == 0, "store_roundtrip: local header at offset 0");
    CHECK (dd > loc, "store_roundtrip: data description present after local header");
    CHECK (cen > dd, "store_roundtrip: central directory header present after data description");
    CHECK (end > cen, "store_roundtrip: end of central directory present after central header");

    free (buf);
  }
}

static void
test_deflate_roundtrip (void)
{
  /* Repetitive/compressible data */
  static char data[2048];
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;
  z_stream infstream;
  uint8_t inflated[4096];
  int rv;
  entry_spec spec;
  uint8_t *buf;
  size_t len;
  long loc, nameLenOff, dataOff;
  uint16_t nameLen;

  memset (data, 'A', sizeof (data));

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  e = zs_writeentry (zs, (uint8_t *)data, sizeof (data), "aaa.txt", time (NULL), ZS_DEFLATE, &ws);
  CHECK (e != NULL, "deflate_roundtrip: zs_writeentry succeeded");

  if (e)
  {
    CHECK (e->UncompressedSize == sizeof (data), "deflate_roundtrip: UncompressedSize matches");
    CHECK (e->CompressedSize <= e->UncompressedSize,
           "deflate_roundtrip: CompressedSize <= UncompressedSize");
    CHECK (e->CRC32 == crc32 (0L, (const Bytef *)data, (uInt)sizeof (data)),
           "deflate_roundtrip: CRC32 matches zlib's own crc32()");
  }

  zs_finish (zs, &ws);
  zs_free (zs);

  /* Independently inflate the actual archived bytes and compare */
  spec.data = data;
  spec.datalen = sizeof (data);
  spec.method = ZS_DEFLATE;
  spec.name = "aaa.txt";
  build_and_read (build_one_entry, &spec, &buf, &len);

  CHECK (buf != NULL, "deflate_roundtrip: archive produced");

  if (buf)
  {
    loc = find_signature (buf, len, LOCALHEADERSIG, 0);
    CHECK (loc == 0, "deflate_roundtrip: local header at offset 0");

    /* Local File Header: name length is a 2-byte LE field at offset+26 */
    nameLenOff = loc + 26;
    nameLen = (uint16_t)(buf[nameLenOff] | (buf[nameLenOff + 1] << 8));
    dataOff = loc + 30 + nameLen; /* fixed header (30 bytes) + name + (no extra field) */

    memset (&infstream, 0, sizeof (infstream));
    rv = inflateInit2 (&infstream, -MAX_WBITS);
    CHECK (rv == Z_OK, "deflate_roundtrip: inflateInit2 succeeded");

    infstream.next_in = buf + dataOff;
    infstream.avail_in = (uInt)(len - dataOff);
    infstream.next_out = inflated;
    infstream.avail_out = sizeof (inflated);

    rv = inflate (&infstream, Z_FINISH);
    CHECK (rv == Z_STREAM_END, "deflate_roundtrip: inflate reached stream end");
    CHECK (infstream.total_out == sizeof (data),
           "deflate_roundtrip: inflated size matches original");
    CHECK (memcmp (inflated, data, sizeof (data)) == 0,
           "deflate_roundtrip: inflated content matches original");

    inflateEnd (&infstream);
    free (buf);
  }
}

static void
test_chunked_streaming (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;
  const char chunk1[] = "Hello, ";
  const char chunk2[] = "streaming ";
  const char chunk3[] = "world!";
  uint32_t expectedCrc;

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  e = zs_entrybegin (zs, "stream.txt", time (NULL), ZS_STORE, &ws);
  CHECK (e != NULL, "chunked_streaming: zs_entrybegin succeeded");

  if (e)
  {
    CHECK ((e->GeneralFlag & (1 << 3)) != 0, "chunked_streaming: streaming bit (3) is set");

    zs_entrydata (zs, e, (uint8_t *)chunk1, strlen (chunk1), &ws);
    zs_entrydata (zs, e, (uint8_t *)chunk2, strlen (chunk2), &ws);
    zs_entrydata (zs, e, (uint8_t *)chunk3, strlen (chunk3), &ws);
    zs_entryend (zs, e, &ws);

    CHECK (e->UncompressedSize == strlen (chunk1) + strlen (chunk2) + strlen (chunk3),
           "chunked_streaming: UncompressedSize matches sum of chunks");
    CHECK (e->CompressedSize == e->UncompressedSize,
           "chunked_streaming: CompressedSize == UncompressedSize for STORE");

    expectedCrc = crc32 (0L, Z_NULL, 0);
    expectedCrc = crc32 (expectedCrc, (const Bytef *)chunk1, (uInt)strlen (chunk1));
    expectedCrc = crc32 (expectedCrc, (const Bytef *)chunk2, (uInt)strlen (chunk2));
    expectedCrc = crc32 (expectedCrc, (const Bytef *)chunk3, (uInt)strlen (chunk3));
    CHECK (e->CRC32 == expectedCrc, "chunked_streaming: CRC32 matches concatenated chunks");
  }

  zs_finish (zs, &ws);
  zs_free (zs);
}

static void
test_multiple_entries (void)
{
  ZIPstream *zs;
  int64_t ws;
  char data[] = "x";

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  zs_writeentry (zs, (uint8_t *)data, 1, "a.txt", time (NULL), ZS_STORE, &ws);
  zs_writeentry (zs, (uint8_t *)data, 1, "b.txt", time (NULL), ZS_STORE, &ws);
  zs_writeentry (zs, (uint8_t *)data, 1, "c.txt", time (NULL), ZS_STORE, &ws);

  CHECK (zs->EntryCount == 3, "multiple_entries: EntryCount is 3");
  CHECK (zs_finish (zs, &ws) == 0, "multiple_entries: zs_finish succeeds");

  zs_free (zs);
}

static void
test_empty_entry_store (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  e = zs_writeentry (zs, (uint8_t *)"", 0, "empty.txt", time (NULL), ZS_STORE, &ws);

  CHECK (e != NULL, "empty_entry_store: zs_writeentry succeeded");
  if (e)
  {
    CHECK (e->UncompressedSize == 0, "empty_entry_store: UncompressedSize is 0");
    CHECK (e->CompressedSize == 0, "empty_entry_store: CompressedSize is 0");
    CHECK (e->CRC32 == 0, "empty_entry_store: CRC32 is 0");
  }

  zs_finish (zs, &ws);
  zs_free (zs);
}

static void
test_empty_entry_deflate (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  e = zs_writeentry (zs, (uint8_t *)"", 0, "empty.txt", time (NULL), ZS_DEFLATE, &ws);

  CHECK (e != NULL, "empty_entry_deflate: zs_writeentry succeeded");
  if (e)
  {
    CHECK (e->UncompressedSize == 0, "empty_entry_deflate: UncompressedSize is 0");
    /* An empty raw-deflate (Z_FINISH, no input) stream is exactly the
     * 2 bytes 0x03 0x00, verified directly against zlib. */
    CHECK (e->CompressedSize == 2,
           "empty_entry_deflate: CompressedSize is 2 (empty deflate block)");
    CHECK (e->CRC32 == 0, "empty_entry_deflate: CRC32 is 0");
  }

  zs_finish (zs, &ws);
  zs_free (zs);
}

static void
test_custom_method_roundtrip (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;
  const char data[] = "custom method data";

  custom_finish_called = 0;

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  CHECK (zs_registermethod (zs, 99, custom_init, custom_process, custom_finish) != NULL,
         "custom_method: registration succeeded");

  e = zs_writeentry (zs, (uint8_t *)data, strlen (data), "custom.txt", time (NULL), 99, &ws);
  CHECK (e != NULL, "custom_method: zs_writeentry with custom method succeeded");

  if (e)
  {
    CHECK (e->methoddata == (void *)1, "custom_method: init() ran (sentinel set)");
    CHECK (e->UncompressedSize == strlen (data), "custom_method: UncompressedSize matches");
    CHECK (e->CompressedSize == e->UncompressedSize,
           "custom_method: CompressedSize == UncompressedSize (store-like)");
  }

  CHECK (custom_finish_called == 1, "custom_method: finish() ran");

  zs_finish (zs, &ws);
  zs_free (zs);
}

static void
test_entry_size_limit (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;
  char chunk[64];
  ZIPentry *r;

  memset (chunk, 'x', sizeof (chunk));

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  e = zs_entrybegin (zs, "big.bin", time (NULL), ZS_STORE, &ws);
  CHECK (e != NULL, "entry_size_limit: zs_entrybegin succeeded");

  if (e)
  {
    /* Seed sizes right at the edge of the 4GiB boundary (public fields) */
    e->CompressedSize = 0xFFFFFFF0ULL;
    e->UncompressedSize = 0xFFFFFFF0ULL;

    r = zs_entrydata (zs, e, (uint8_t *)chunk, sizeof (chunk), &ws);
    CHECK (r == NULL, "entry_size_limit: zs_entrydata rejects crossing the 4GiB boundary");
  }

  zs_free (zs);
}

static void
test_name_length_boundary (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;
  char name255[256]; /* 255 chars + NUL */
  char name256[257]; /* 256 chars + NUL */

  memset (name255, 'a', 255);
  name255[255] = '\0';
  memset (name256, 'a', 256);
  name256[256] = '\0';

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);

  e = zs_writeentry (zs, (uint8_t *)"x", 1, name255, time (NULL), ZS_STORE, &ws);
  CHECK (e != NULL, "name_length_boundary: 255-char name accepted");
  if (e)
    CHECK (e->NameLength == 255, "name_length_boundary: NameLength is 255");

  e = zs_writeentry (zs, (uint8_t *)"x", 1, name256, time (NULL), ZS_STORE, &ws);
  CHECK (e == NULL, "name_length_boundary: 256-char name rejected");

  zs_finish (zs, &ws);
  zs_free (zs);
}

static void
test_dos_date_clamp (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;
  time_t farFuture;
  time_t epochStart = 0; /* 1970, before the DOS epoch */
  int year, month, day;

  /* Any time far enough past 2107 clamps identically; exact date doesn't
   * matter once past the boundary. */
  farFuture = (time_t)10000000000LL; /* year ~2286 */

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);

  e = zs_writeentry (zs, (uint8_t *)"x", 1, "future.txt", farFuture, ZS_STORE, &ws);
  CHECK (e != NULL, "dos_date_clamp: future entry succeeded");
  if (e)
  {
    year = ((e->DOSDate >> 9) & 0x7F) + 1980;
    CHECK (year == 2107, "dos_date_clamp: future timestamp clamps to year 2107");
  }

  e = zs_writeentry (zs, (uint8_t *)"x", 1, "past.txt", epochStart, ZS_STORE, &ws);
  CHECK (e != NULL, "dos_date_clamp: past entry succeeded");
  if (e)
  {
    year = ((e->DOSDate >> 9) & 0x7F) + 1980;
    month = (e->DOSDate >> 5) & 0x0F;
    day = e->DOSDate & 0x1F;
    CHECK (year == 1980 && month == 1 && day == 1,
           "dos_date_clamp: pre-1980 timestamp floors to 1980-01-01");
  }

  zs_finish (zs, &ws);
  zs_free (zs);
}

static void
test_entrycount_zip64_eocd (void)
{
  uint8_t *buf;
  size_t len;
  long z64, eocd;
  uint16_t thisDisk, total;

  build_and_read (build_entrycount_zip64, NULL, &buf, &len);
  CHECK (buf != NULL, "entrycount_zip64_eocd: archive produced");

  if (buf)
  {
    z64 = find_signature (buf, len, ZIP64ENDRECORDSIG, 0);
    CHECK (z64 >= 0, "entrycount_zip64_eocd: ZIP64 end-of-central-dir record present");

    eocd = find_signature (buf, len, ENDHEADERSIG, 0);
    CHECK (eocd >= 0, "entrycount_zip64_eocd: classic EOCD present");

    if (eocd >= 0)
    {
      thisDisk = (uint16_t)(buf[eocd + 8] | (buf[eocd + 9] << 8));
      total = (uint16_t)(buf[eocd + 10] | (buf[eocd + 11] << 8));
      CHECK (thisDisk == 0xFFFF, "entrycount_zip64_eocd: classic EOCD this-disk count is 0xFFFF");
      CHECK (total == 0xFFFF, "entrycount_zip64_eocd: classic EOCD total count is 0xFFFF");
    }

    free (buf);
  }
}

static void
test_zip64_version_needed (void)
{
  uint8_t *buf;
  size_t len;
  long cdh;
  uint16_t versionNeeded;

  build_and_read (build_zip64_version_needed, NULL, &buf, &len);
  CHECK (buf != NULL, "zip64_version_needed: archive produced");

  if (buf)
  {
    cdh = find_signature (buf, len, CENTRALHEADERSIG, 0);
    CHECK (cdh >= 0, "zip64_version_needed: central directory header present");

    if (cdh >= 0)
    {
      versionNeeded = (uint16_t)(buf[cdh + 6] | (buf[cdh + 7] << 8));
      CHECK (versionNeeded >= 45,
             "zip64_version_needed: version needed to extract is >= 45 for a ZIP64 entry");
    }

    free (buf);
  }
}

static void
test_finish_zero_entries (void)
{
  uint8_t *buf;
  size_t len;
  long eocd;
  uint16_t total;

  build_and_read (build_finish_zero_entries, NULL, &buf, &len);
  CHECK (buf != NULL, "finish_zero_entries: archive produced");

  if (buf)
  {
    eocd = find_signature (buf, len, ENDHEADERSIG, 0);
    CHECK (eocd >= 0, "finish_zero_entries: EOCD present");

    if (eocd >= 0)
    {
      total = (uint16_t)(buf[eocd + 10] | (buf[eocd + 11] << 8));
      CHECK (total == 0, "finish_zero_entries: entry count is 0");
    }

    free (buf);
  }
}

static void
test_failed_entrybegin_cleanup (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  zs_registermethod (zs, 98, failing_init, failing_process, NULL);

  e = zs_entrybegin (zs, "bad.txt", time (NULL), 98, &ws);
  CHECK (e == NULL, "failed_entrybegin_cleanup: zs_entrybegin returns NULL on init() failure");
  CHECK (zs->EntryCount == 0, "failed_entrybegin_cleanup: EntryCount stays 0");
  CHECK (zs->FirstEntry == NULL, "failed_entrybegin_cleanup: FirstEntry stays NULL");

  zs_free (zs);
}

static void
test_write_error_path (void)
{
  int fd;
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;

  fd = open ("/dev/full", O_WRONLY);
  if (fd < 0)
  {
    fprintf (stderr, "SKIP: write_error_path (/dev/full not available: %s)\n", strerror (errno));
    return;
  }

  zs = zs_init (fd, NULL);
  e = zs_entrybegin (zs, "willfail.txt", time (NULL), ZS_STORE, &ws);

  CHECK (e == NULL, "write_error_path: zs_entrybegin fails when the underlying write() fails");
  CHECK (ws != 0, "write_error_path: writestatus is propagated");
  CHECK (zs->EntryCount == 0, "write_error_path: EntryCount stays 0 after cleanup");

  zs_free (zs);
  close (fd);
}

static void
test_null_safety_and_registermethod (void)
{
  ZIPstream *zs;
  int64_t ws;
  ZIPentry *e;

  CHECK (zs_registermethod (NULL, 1, NULL, custom_process, NULL) == NULL,
         "null_safety: zs_registermethod(NULL, ...) returns NULL");

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);

  CHECK (zs_registermethod (zs, 100, NULL, NULL, NULL) == NULL,
         "null_safety: zs_registermethod with NULL process() returns NULL");

  CHECK (zs_registermethod (zs, ZS_STORE, NULL, custom_process, NULL) == NULL,
         "null_safety: zs_registermethod with a duplicate ID returns NULL");

  e = zs_writeentry (zs, (uint8_t *)"x", 1, "x.txt", time (NULL), 12345, &ws);
  CHECK (e == NULL, "null_safety: zs_writeentry with an unregistered methodID returns NULL");

  zs_finish (zs, &ws);
  zs_free (zs);

  /* These must not crash; reaching the CHECK() calls is itself the proof */
  zs_free (NULL);
  CHECK (1, "null_safety: zs_free(NULL) does not crash");
  CHECK (zs_finish (NULL, &ws) == -1, "null_safety: zs_finish(NULL, ...) returns -1");
}

static void
test_zs_init_reuse (void)
{
  ZIPstream *zs;
  ZIPentry *e;
  int64_t ws;

  zs = zs_init (open ("/dev/null", O_WRONLY), NULL);
  zs_writeentry (zs, (uint8_t *)"x", 1, "x.txt", time (NULL), ZS_STORE, &ws);
  CHECK (zs->EntryCount == 1, "zs_init_reuse: entry added before reuse");

  zs = zs_init (open ("/dev/null", O_WRONLY), zs);
  CHECK (zs != NULL, "zs_init_reuse: re-init succeeded");
  if (zs)
  {
    CHECK (zs->EntryCount == 0, "zs_init_reuse: EntryCount reset to 0");
    CHECK (zs->FirstEntry == NULL, "zs_init_reuse: FirstEntry reset to NULL");

    e = zs_writeentry (zs, (uint8_t *)"y", 1, "y.txt", time (NULL), ZS_DEFLATE, &ws);
    CHECK (e != NULL, "zs_init_reuse: STORE/DEFLATE methods usable again after reuse");

    zs_finish (zs, &ws);
    zs_free (zs);
  }
}

int
main (void)
{
  test_store_roundtrip ();
  test_deflate_roundtrip ();
  test_chunked_streaming ();
  test_multiple_entries ();
  test_empty_entry_store ();
  test_empty_entry_deflate ();
  test_custom_method_roundtrip ();
  test_entry_size_limit ();
  test_name_length_boundary ();
  test_dos_date_clamp ();
  test_entrycount_zip64_eocd ();
  test_zip64_version_needed ();
  test_finish_zero_entries ();
  test_failed_entrybegin_cleanup ();
  test_write_error_path ();
  test_null_safety_and_registermethod ();
  test_zs_init_reuse ();

  fprintf (stderr, "%d passed, %d failed\n", passes, failures);

  return (failures != 0);
}
