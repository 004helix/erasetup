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

struct dump *dump_open(const char *file)
{
	struct dump *dump;
	int fd;

	fd = open(file, O_RDWR | O_EXCL | O_CREAT, 0666);
	if (fd == -1)
	{
		fprintf(stderr, "can't open dump-file %s: %s\n",
		                file, strerror(errno));
		return NULL;
	}

	dump = malloc(sizeof(*dump));
	if (dump == NULL)
	{
		fprintf(stderr, "not enough memory\n");
		return NULL;
	}

	dump->size = 0;
	dump->fd = fd;

	return dump;
}

int dump_append(struct dump *dump, void *data, unsigned size)
{
	size_t data_size = size * sizeof(uint32_t);
	ssize_t rc;

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

	return 0;
}

void dump_close(struct dump *dump)
{
	close(dump->fd);
	free(dump);
	return;
}
