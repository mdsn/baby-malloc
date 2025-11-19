.POSIX:

CC = cc
CFLAGS = -std=c99 -fPIC -g -O0 -pedantic -Wall -Wextra

# Apple's libmalloc complains if address sanitizer takes over its space. Silence it:
TESTENV = MallocNanoZone=0

.PHONY: all clean test

all: malloc.so malloc.dylib tests

malloc.so: malloc.o exports.o
	$(CC) $(CFLAGS) -shared -fvisibility=hidden -o $@ malloc.o exports.o
malloc.dylib: malloc.o interpose.o
	$(CC) $(CFLAGS) -dynamiclib -fvisibility=hidden -o $@ malloc.o interpose.o
malloc.o: malloc.c malloc.h internal.h Makefile
	$(CC) $(CFLAGS) -c malloc.c
exports.o: exports.c
	$(CC) $(CFLAGS) -c exports.c
interpose.o: interpose.c malloc.h
	$(CC) $(CFLAGS) -c interpose.c

tests: tests.o malloc.o
	$(CC) $(CFLAGS) -o $@ tests.o malloc.o
tests.o: tests.c malloc.h internal.h Makefile
	$(CC) $(CFLAGS) -c tests.c

test:
	make tests
	$(TESTENV) ./tests

clean:
	rm -f malloc.so malloc.dylib malloc.o interpose.o
	rm -f tests tests.o

tags:
	ctags -f ./tags -R --languages=C --map-C=+.h
