/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>

#include "era.h"
#include "era_dump.h"

struct dump *dump_open(const char *file, unsigned total_blocks)
{
	struct dump *dump;
	unsigned bs; // bitset words count
	size_t ds; // dump struct size in bytes
	int fd;

	// open file
	fd = open(file, O_RDWR | O_EXCL | O_CREAT, 0666);
	if (fd == -1)
	{
		fprintf(stderr, "can't open dump-file %s: %s\n",
		                file, strerror(errno));
		return NULL;
	}

	// calc bitset words
	bs = (total_blocks + BITS_PER_ARRAY_ENTRY - 1) / BITS_PER_ARRAY_ENTRY;

	// calc dump struct size
	ds = sizeof(*dump) + bs * (BITS_PER_ARRAY_ENTRY / 8);

	// allocate dump struct
	dump = malloc(ds);
	if (dump == NULL)
	{
		fprintf(stderr, "not enough memory\n");
		return NULL;
	}

	// zero dump struct
	memset(dump, 0, ds);

	// init dump
	dump->fd = fd;
	dump->max_ents = total_blocks;
	dump->max_bs_ents = bs;

	return dump;
}

int dump_append_array(struct dump *dump, void *data, unsigned size)
{
	size_t data_size = size * ERA_ENTRY_SIZE;
	ssize_t rc;

	if (dump->cur_ents + size > dump->max_ents)
	{
		fprintf(stderr, "dump-file overflow: "
		        "attempt to append %u extra entries\n",
		        (dump->cur_ents + size) - dump->max_ents);
		return -1;
	}

	rc = write(dump->fd, data, data_size);
	if (rc == -1)
	{
		fprintf(stderr, "can't write dump-file: %s\n",
		        strerror(errno));
		return -1;
	}

	if (rc != data_size)
	{
		fprintf(stderr, "can't write dump-file "
		        "not enough disk space?");
		return -1;
	}

	dump->cur_ents += size;

	return 0;
}

int dump_append_bitset(struct dump *dump, uint64_t entry)
{
	if (dump->cur_bs_ents >= dump->max_bs_ents)
	{
		fprintf(stderr, "bitset overflow: "
		        "attempt to append word to full bit\n");
		return -1;
	}

	dump->bitset[dump->cur_bs_ents++] = entry;

	return 0;
}

void dump_close(struct dump *dump)
{
	close(dump->fd);
	free(dump);
	return;
}
