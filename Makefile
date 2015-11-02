CC = gcc
CFLAGS = -Wall -Werror

erasetup: crc32c.o era_md.o era_btree.o erasetup.o
	$(CC) -o $@ $^

crc32c.o: crc32c.h crc32c.c
	$(CC) $(CFLAGS) -c crc32c.c

era_md.o: crc32c.h era.h era_md.h era_md.c
	$(CC) $(CFLAGS) -c era_md.c

era_btree.o: era.h era_md.h era_btree.h era_btree.c
	$(CC) $(CFLAGS) -c era_btree.c

erasetup.o: crc32c.h era.h era_md.h era_btree.h erasetup.c
	$(CC) $(CFLAGS) -c erasetup.c
