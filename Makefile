CC = gcc
CFLAGS = -Wall
LDFLAGS = -ldevmapper
#CFLAGS = -Wall -Werror

erasetup: crc32c.o era_md.o era_dm.o era_dump.o \
          era_btree.o era_cmd_create.o erasetup.o
	$(CC) -o $@ $^ $(LDFLAGS)

crc32c.o: crc32c.h crc32c.c
	$(CC) $(CFLAGS) -c crc32c.c

era_md.o: crc32c.h era.h era_md.h era_md.c
	$(CC) $(CFLAGS) -c era_md.c

era_dm.o: era.h era_dm.h era_dm.c
	$(CC) $(CFLAGS) -c era_dm.c

era_dump.o: crc32c.h era.h era_dump.h era_dump.c
	$(CC) $(CFLAGS) -c era_dump.c

era_btree.o: era.h era_md.h era_dump.h era_btree.h era_btree.c
	$(CC) $(CFLAGS) -c era_btree.c

era_cmd_create.o: era.h era_md.h era_cmd_create.h era_cmd_create.c
	$(CC) $(CFLAGS) -c era_cmd_create.c

erasetup.o: crc32c.h era.h era_md.h era_dm.h era_dump.h era_btree.h erasetup.c
	$(CC) $(CFLAGS) -c erasetup.c

clean:
	rm -f erasetup *.o
