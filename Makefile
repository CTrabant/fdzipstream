
# Build environment can be configured the following
# environment variables:
#   CC : Specify the C compiler to use
#   CFLAGS : Specify compiler options to use

CFLAGS += -Wall

all: zipexample zipfiles

zipexample: fdzipstream.h fdzipstream.c

zipfiles: fdzipstream.h fdzipstream.c

zipexample: fdzipstream.c zipexample.c
	$(CC) $(CFLAGS) -o zipexample fdzipstream.c zipexample.c -lz

zipfiles: fdzipstream.c zipfiles.c
	$(CC) $(CFLAGS) -o zipfiles fdzipstream.c zipfiles.c -lz

test_fdzipstream: fdzipstream.h fdzipstream.c test_fdzipstream.c
	$(CC) $(CFLAGS) -o test_fdzipstream fdzipstream.c test_fdzipstream.c -lz

test: test_fdzipstream
	./test_fdzipstream

check: test

clean:
	rm -f zipexample zipfiles test_fdzipstream

