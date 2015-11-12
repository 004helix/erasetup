/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>

#include "era.h"
#include "era_blk.h"

static int trypath(const char *path, unsigned major, unsigned minor)
{
	struct stat st;

	if (stat(path, &st) != -1 && S_ISBLK(st.st_mode) &&
	    major == major(st.st_rdev) && minor == minor(st.st_rdev))
		return 1;

	return 0;
}

static int findandopen(int dirfd, int rw, unsigned major, unsigned minor)
{
	DIR *dir;
	struct dirent entry;
	struct dirent *d;
	struct stat st;
	char *name;
	int type;
	int fd;

	dir = fdopendir(dirfd);
	if (dir == NULL)
	{
		error(errno, "can't open directory");
		close(dirfd);
		return -1;
	}

	for (;;)
	{
		int err = readdir_r(dir, &entry, &d);
		int stat_done = 0;

		if (err)
		{
			error(err, "can't read directory");
			closedir(dir);
			return -1;
		}

		if (d == NULL)
			break;

		name = d->d_name;
		type = d->d_type;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;

		if (type == DT_UNKNOWN)
		{
			if (fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW))
				continue;

			stat_done++;

			if (S_ISBLK(st.st_mode))
				type = DT_BLK;
			else
			if (S_ISDIR(st.st_mode))
				type = DT_DIR;
			else
				continue;
		}

		if (type == DT_DIR)
		{
			fd = openat(dirfd, name, O_RDONLY | O_DIRECTORY);
			if (fd == -1)
				continue;

			fd = findandopen(fd, rw, major, minor);

			if (fd == -1)
			{
				closedir(dir);
				return -1;
			}

			if (fd == -2)
				continue;

			return fd;
		}

		if (type == DT_BLK)
		{
			if (stat_done == 0 &&
			    fstatat(dirfd, name, &st, AT_SYMLINK_NOFOLLOW))
				continue;

			if (major != major(st.st_rdev) ||
			    minor != minor(st.st_rdev))
				continue;

			fd = openat(dirfd, name,
			            (rw ? O_RDWR : O_RDONLY) | O_DIRECT);
			if (fd == -1)
			{
				error(errno, "can't open block device %s",
				      name);
				closedir(dir);
				return -1;
			}

			closedir(dir);
			return fd;
		}
	}

	closedir(dir);
	return -2;
}

int blkopen(const char *device, int rw,
            unsigned *major, unsigned *minor, uint64_t *sectors)
{
	struct stat st;
	uint64_t size;
	int fd;

	if (device == NULL)
		fd = rw;
	else
	{
		int flags = (rw ? O_RDWR : O_RDONLY) | O_DIRECT;

		fd = open(device, flags);
		if (fd == -1)
		{
			error(errno, "can't open device %s", device);
			return -1;
		}
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

	if (sectors)
		*sectors = size / SECTOR_SIZE;

	if (major)
		*major = major(st.st_rdev);

	if (minor)
		*minor = minor(st.st_rdev);

	return fd;
}

int blkopen2(unsigned major, unsigned minor, int rw, uint64_t *sectors)
{
	char path[PATH_MAX];
	int fd;

	/*
	 * try /dev/block/<major>:<minor>
	 */

	snprintf(path, sizeof(path), "/dev/block/%u:%u", major, minor);
	if (trypath(path, major, minor))
		return blkopen(path, rw, NULL, NULL, sectors);

	/*
	 * read /sys/dev/block/<major>:<minor>/uevent
	 * and try /dev/<DEVNAME>
	 */

	snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/uevent",
	         major, minor);
	fd = open(path, O_RDONLY);
	if (fd != -1)
	{
		char buffer[4096];
		size_t size;

		size = read(fd, buffer, sizeof(buffer));

		close(fd);

		if (size > 0 && size < sizeof(buffer))
		{
			char *line;
			buffer[size] = '\0';

			line = strstr(buffer, "DEVNAME=");
			if (line != NULL)
			{
				char *name = line + 8;
				char *endptr = name;

				while (*endptr && *endptr != '\n')
					endptr++;

				*endptr = '\0';

				snprintf(path, sizeof(path), "/dev/%s", name);

				if (trypath(path, major, minor))
					return blkopen(path, rw, NULL, NULL,
					               sectors);
			}
		}
	}

	/*
	 * scan /dev directory
	 */

	fd = open("/dev", O_RDONLY | O_DIRECTORY);
	if (fd == -1)
	{
		error(errno, "can't open /dev directory");
		return -1;
	}

	fd = findandopen(fd, rw, major, minor);
	if (fd == -1)
		return -1;

	if (fd == -2)
	{
		error(0, "can't find device %u:%u", major, minor);
		return -1;
	}

	return blkopen(NULL, fd, NULL, NULL, sectors);
}
