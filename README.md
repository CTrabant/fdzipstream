NOTE: This is a modified fork of CTrabant/fdzipstream which has be severely rewritten to remove duplication and add extensibility via an interface which adds AE-2 (AES-256) encryption standard to fdzipstream.

# FD ZIP Stream

Create ZIP archives in streaming fashion, writing to a file
descriptor.  The output stream (file descriptor) does not need to be
seekable and can be a pipe or a network socket.  The entire archive
contents does not need to be in memory at once.

[zlib](http://www.zlib.net/) is required for deflate compression: http://www.zlib.net/

## What this will do for you:

* Create a ZIP archive in a streaming fashion, writing to an output stream (file descriptor, pipe, network socket) without seeking.
* Compress the archive entries (using zlib).
* Add ZIP64 structures as needed to support large (>4GB) archives.
* Simple creation of ZIP archives even if not streaming.
* Support AES (AE-2) encryption standard to encypt your data.

## What this will **NOT** do for you:

- Open/close files or sockets.
- Support advanced ZIP archive features (e.g. file attributes).
- Allow archiving of individual files/entries larger than 4GB, the total
   of all files can be larger than 4GB but not individual entries.
- Allow every possible compression method.

ZIP archive file/entry modifiation times are stored in UTC.

## Usage pattern

### Creating a ZIP archive when entire files/entries are in memory:
```
zs_init ()
  for each entry:
    zs_writeentry ()
zs_finish ()
zs_free ()
```

### Creating a ZIP archive when files/entries are chunked:
```
zs_init ()
  for each entry:
    zs_entrybegin ()
      for each chunk of entry:
        zs_entrydata()
    zs_entryflush()  [if final not flagged in zs_entrydata()]
    zs_entryend()
zs_finish ()
zs_free ()
```

### Creating an encrypted ZIP archive when files/entries are chunked:
Extensions are optional additions that add extra functionality and dependencies. 
The extension FDZIPSTREAM_AESDEFLATE adds AE-2 file encryption. This utilises the OpenSSL library 
for AES-256 and HMAC-SHA1 authentication accoring to RFC2898. These files can be decrypted using 7-Zip,
WinZip etc.
```
#define FDZIPSTREAM_REGISTEREXTENSIONS 1 //in fdzipstream.h
#define FDZIPSTREAM_AESDEFLATE 1 //in fdzipstream_extensions.h
```
Usage
```
zs_init ()
  for each entry:
    zs_entrybegin ( ..., ZS_AES2_DEFLATE, ... )
      zs_aesdeflate_setpassword()
      for each chunk of entry:
        zs_entrydata()
    zs_entryflush()  [if final not flagged in zs_entrydata()]
    zs_entryend()
zs_finish ()
zs_free ()
```

## Why?

Libraries such as libarchive (http://www.libarchive.org/) can create
ZIP archives in a streaming fashion.  But that is a lot of code, and a
large dependency, to add to a project to get streaming ZIP archive
creation.  Other small ZIP creation projects exist but I could not
find one that did not rely on seekable I/O streams and thus could not
be used to write to a socket or pipe.  I also needed a portable
solution.  This code depends only on zlib, which is itself very
portable and commonly found in the base installation on many systems.

Simple ZIP archive creation in about 1000 lines of code, ready to be
embedded.
