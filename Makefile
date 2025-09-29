.POSIX:

CC = cc
CFLAGS = -std=c99 -fsanitize=address,undefined -g -O0 -pedantic -Wall -Wextra

# Apple's libmalloc complains if address sanitizer takes over its space. Silence it:
TESTENV = MallocNanoZone=0

.PHONY: all clean test

all: malloc.dylib tests

malloc.dylib: malloc.o
	$(CC) $(CFLAGS) -dynamiclib -o $@ malloc.o
malloc.o: malloc.c malloc.h internal.h Makefile
	$(CC) $(CFLAGS) -c malloc.c

tests: tests.o malloc.o
	$(CC) $(CFLAGS) -o $@ tests.o malloc.o
tests.o: tests.c malloc.h internal.h Makefile
	$(CC) $(CFLAGS) -c tests.c

test:
	make tests
	$(TESTENV) ./tests

clean:
	rm -f malloc.dylib malloc.o
	rm -f tests tests.o
