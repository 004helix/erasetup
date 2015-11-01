CC = gcc

erasetup: crc32c.o erasetup.o
	$(CC) -o $@ $^

crc32c.o: crc32c.h crc32c.c
	$(CC) -Wall -c crc32c.c

erasetup.o: crc32c.h erasetup.h erasetup.c
	$(CC) -Wall -c erasetup.c
