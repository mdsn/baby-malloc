.POSIX:

CC = cc
CFLAGS = -std=c99 -fPIC -g -O0 -pedantic -Wall -Wextra

# Apple's libmalloc complains if address sanitizer takes over its space. Silence it:
TESTENV = MallocNanoZone=0

.PHONY: all clean test

all: malloc.dylib tests

malloc.dylib: malloc.o interpose.o
	$(CC) $(CFLAGS) -dynamiclib -fvisibility=hidden -o $@ malloc.o interpose.o
malloc.o: malloc.c malloc.h internal.h Makefile
	$(CC) $(CFLAGS) -c malloc.c
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
	rm -f malloc.dylib malloc.o interpose.o
	rm -f tests tests.o
