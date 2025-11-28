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
	$(BINENV) git status > /dev/null
	$(BINENV) git log --oneline -n 50 > /dev/null
	$(BINENV) git diff HEAD~5..HEAD > /dev/null
	$(BINENV) grep -r blk . 2> /dev/null | sort | uniq -c > /dev/null
	$(BINENV) diff -r . .. 2> /dev/null | head -100 > /dev/null
	$(BINENV) find /usr -maxdepth 4 -type f -name "*.h" -exec echo {} \; > /dev/null 2>&1
	$(BINENV) find . -type f -print0 | xargs -0 -n100 echo > /dev/null 2>&1
	$(BINENV) tar cf - /etc 2>/dev/null | wc -c > /dev/null

clean:
	rm -f malloc.so malloc.o interpose.o
	rm -f tests tests.o

tags: malloc.c exports.c interpose.c tests.c malloc.h internal.h Makefile
	ctags -f ./tags -R --languages=C --map-C=+.h
