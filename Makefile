
all: zipexample zipfiles

zipexample: fdzipstream.h fdzipstream.c

zipfiles: fdzipstream.h fdzipstream.c

zipexample: fdzipstream.c zipexample.c
	$(CC) -Wall -o zipexample fdzipstream.c zipexample.c -lz

zipfiles: fdzipstream.c zipfiles.c
	$(CC) -Wall -o zipfiles fdzipstream.c zipfiles.c -lz 

clean:
	rm -f zipexample zipfiles

