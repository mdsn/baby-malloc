.POSIX:

CC = cc
CFLAGS = -std=c99 -g -O0 -pedantic -Wall -Wextra

all: malloc

malloc: malloc.o
	$(CC) $(CFLAGS) -o malloc malloc.o

malloc.o: malloc.c Makefile
	$(CC) $(CFLAGS) -c malloc.c

clean:
	rm -f malloc malloc.o
