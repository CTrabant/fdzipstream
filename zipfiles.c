/***************************************************************************
 * zipfiles.c
 *
 * Create a ZIP archive from all files specified on the command line
 * and write archive to stdout.  All diagnostics are printed to stderr.
 *
 * Compile with:
 *   cc -Wall fdzipstream.c zipfiles.c -o zipfiles -lz
 *
 * modified 2013.9.28
 ***************************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#include "fdzipstream.h"

#define MAXIMUM_READ 10485760

int main (int argc, char *argv[])
{
  ZIPstream *zstream = NULL;
  ZIPentry *zentry = NULL;

  unsigned char *buffer = NULL;
  uint64_t bufferlength = 0;
  ssize_t writestatus;
  
  int method = ZS_DEFLATE;
  int streaming = 0;
  int fd;
  int final = 0;
  int idx;

  FILE *input;
  struct stat st;
  
  uint64_t readcount;
  uint64_t readsize;
  
  if ( argc < 2 )
    {
      fprintf (stderr, "zipfiles: write a ZIP archive to stdout containing specified files\n");
      fprintf (stderr, "Usage: zipfiles [-s] [-0] <file1> [file2] ... > output.zip\n");
      fprintf (stderr, "  -s  Create archive entries with streaming structures\n");
      fprintf (stderr, "  -0  Store archive entries, default is to deflate entries\n");
      fprintf (stderr, "\n");
      return 0;
    }
  
  /* Set output stream to stdout */
  fd = fileno (stdout);
  
  /* Initialize ZIP container */
  if ( (zstream = zs_init (fd, NULL)) == NULL )
    {
      fprintf (stderr, "Error initializing ZIP archive\n");
      return 1;
    }
  
  /* Loop through input arguments and process options */
  for ( idx=1; idx < argc; idx++ )
    {
      if ( ! strncmp (argv[idx], "-s", 2) )
	{
	  streaming = 1;
	  continue;
	}
      if ( ! strncmp (argv[idx], "-0", 2) )
	{
	  method = ZS_STORE;
	  fprintf (stderr, "Storing archive entries, no compression\n");
	  continue;
	}
    }
  
  if ( streaming )
    fprintf (stderr, "Creating streaming ZIP entries\n");
  else
    fprintf (stderr, "Creating non-streaming ZIP entries\n");
  
  /* Loop through input files, skip options */
  for ( idx=1; idx < argc; idx++ )
    {
      if ( ! strcmp (argv[idx], "-s") ||
	   ! strcmp (argv[idx], "-0") )
	continue;
      
      if ( (input = fopen (argv[idx], "r")) == NULL )
	{
	  fprintf (stderr, "Cannot open %s: %s\n", argv[idx], strerror(errno));
	  return 1;
	}
      
      if ( fstat (fileno(input), &st) )
	{
	  fprintf (stderr, "Cannot stat %s: %s\n", argv[idx], strerror(errno));
	  return 1;
	}
      
      if ( ! streaming )  /* Non-streaming, write entire entry at once */
	{
	  /* Allocate buffer */
	  if ( st.st_size > bufferlength )
	    {
	      if ( (buffer = realloc (buffer, st.st_size)) == NULL )
		{
		  fprintf (stderr, "Cannot allocate %lld bytes\n",
			   (long long int) st.st_size);
		  return 1;
		}
	      bufferlength = st.st_size;
	    }
	  
	  /* Read file into buffer */
	  readcount = 0;
	  while ( readcount < st.st_size )
	    {
	      readsize = ( (st.st_size - readcount) < MAXIMUM_READ ) ?
		(st.st_size - readcount) : MAXIMUM_READ;
	      
	      if ( fread (buffer+readcount, readsize, 1, input) != 1 )
		{
		  fprintf (stderr, "Cannot read input file\n");
		  return 1;
		}
	      
	      readcount += readsize;
	    }
	  
	  /* Write entire entry to ZIP archive */
	  zentry = zs_writeentry (zstream, buffer, st.st_size, argv[idx],
				  st.st_mtime, method, &writestatus);
	  
	  if ( zentry == NULL )
	    {
	      fprintf (stderr, "Error adding %s to output ZIP (writestatus: %lld)\n",
		       argv[idx], (long long int) writestatus);
	      return 1;
	    }
	  
	  fprintf (stderr, "Added %s: %lld -> %lld (%.1f%%)\n",
		   zentry->Name,
		   (long long int) zentry->UncompressedSize,
		   (long long int) zentry->CompressedSize,
		   (100.0 * zentry->CompressedSize / zentry->UncompressedSize));
	}
      else /* Streaming, write the entry in chunks */
	{
	  /* Allocate buffer */
	  if ( ! buffer )
	    {
	      bufferlength = 1048576;
	      if ( (buffer = malloc (bufferlength)) == NULL )
		{
		  fprintf (stderr, "Cannot allocate %lld bytes\n",
			   (long long int) bufferlength);
		  return 1;
		}
	    }
	  
	  /* Begin ZIP entry */
	  if ( ! (zentry = zs_entrybegin (zstream, argv[idx], st.st_mtime,
					  method, &writestatus)) )
	    {
	      fprintf (stderr, "Cannot begin ZIP entry for %s (writestatus: %lld)\n",
		       argv[idx], (long long int) writestatus);
	      return 1;
	    }
	  
	  /* Read file into buffer */
	  readcount = 0;
	  while ( readcount < st.st_size )
	    {
	      readsize = fread (buffer, 1, bufferlength, input);
	      readcount += readsize;
	      
	      final = ( readcount >= st.st_size ) ? 1 : 0; 
	      
	      /* Add data to ZIP entry */
	      if ( ! zs_entrydata (zstream, zentry, buffer, readsize, final, &writestatus) )
		{
		  fprintf (stderr, "Error adding entry data to ZIP for %s (writestatus: %lld)\n",
			   argv[idx], (long long int) writestatus);
		  return 1;
		}
	    }
	  
	  /* End ZIP entry */
	  if ( ! zs_entryend (zstream, zentry, &writestatus) )
	    {
	      fprintf (stderr, "Cannot end ZIP entry for %s (writestatus: %lld)\n",
		       argv[idx], (long long int) writestatus);
	      return 1;
	    }
	  
	  fprintf (stderr, "Added %s: %lld -> %lld (%.1f%%)\n",
		   zentry->Name,
		   (long long int) zentry->UncompressedSize,
		   (long long int) zentry->CompressedSize,
		   (100.0 * zentry->CompressedSize / zentry->UncompressedSize));
	}
      
      fclose (input);
    }
  
  /* Finish ZIP archive */
  if ( zs_finish (zstream, &writestatus) )
    {
      fprintf (stderr, "Error finishing ZIP archive (writestatus: %lld)\n",
	       (long long int) writestatus);
      return 1;
    }
  
  fprintf (stderr, "Success, created archive with %d entries\n",
	   zstream->EntryCount);
  
  /* Cleanup */
  zs_free (zstream);

  if ( buffer )
    free (buffer);
  
  return 0;
}
