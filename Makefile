.POSIX:

CC = cc
CFLAGS = -std=c99 -fPIC -g -O0 -pedantic -Wall -Wextra

# Linux only:
BINENV = LD_PRELOAD=./malloc.so

.PHONY: all clean test

all: malloc.so tests

malloc.so: malloc.o exports.o
	$(CC) $(CFLAGS) -shared -fvisibility=hidden -o $@ malloc.o exports.o
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

test: tests
	$(TESTENV) ./tests

# This target runs a few standard utilities backed by malloc.so to make sure
# they don't segfault.
run-binaries: malloc.so
	$(BINENV) ls -l / > /dev/null
	$(BINENV) grep -r blk . > /dev/null
	$(BINENV) find . -maxdepth 2 -type f | wc -l > /dev/null
	$(BINENV) sort malloc.c > /dev/null

clean:
	rm -f malloc.so malloc.o interpose.o
	rm -f tests tests.o

tags: malloc.c exports.c interpose.c tests.c malloc.h internal.h Makefile
	ctags -f ./tags -R --languages=C --map-C=+.h
