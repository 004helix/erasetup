/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "era.h"
#include "era_blk.h"

int blkopen(const char *device, int rw,
            unsigned *major, unsigned *minor, uint64_t *sectors)
{
	struct stat st;
	uint64_t size;
	int fd, flags;

	flags = (rw ? O_RDWR : O_RDONLY) | O_DIRECT;

	fd = open(device, flags);
	if (fd == -1)
	{
		error(errno, "can't open device %s", device);
		return -1;
	}

	if (fstat(fd, &st) == -1)
	{
		error(errno, "can't stat device %s", device);
		close(fd);
		return -1;
	}

	if (!S_ISBLK(st.st_mode))
	{
		error(0, "device is not a block device: %s", device);
		close(fd);
		return -1;
	}

	if (ioctl(fd, BLKGETSIZE64, &size))
	{
		error(errno, "can't get device size %s", device);
		close(fd);
		return -1;
	}

	*sectors = size / SECTOR_SIZE;
	*major = major(st.st_rdev);
	*minor = minor(st.st_rdev);

	return fd;
}
