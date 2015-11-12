/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>

#include "era.h"
#include "era_blk.h"
#include "era_cmd_snap.h"

int era_takesnap(int argc, char **argv)
{
	uint64_t sectors;
	int fd;

	fd = blkopen2(253, 3, 0, &sectors);
	if (fd != -1)
		close(fd);

	printf("%llu\n", (long long unsigned)sectors);

	return 0;
}
