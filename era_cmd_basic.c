/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <linux/fs.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <stdint.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>
#include <endian.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include "crc32c.h"
#include "era.h"
#include "era_md.h"
#include "era_sm.h"
#include "era_dm.h"
#include "era_cmd_basic.h"

static int parse_chunk(const char *str)
{
	long long chunk;
	char *endptr;

	chunk = strtoll(str, &endptr, 10);

	if (chunk <= 0)
		return -1;

	if (chunk == LLONG_MAX)
	{
		error(0, "chunk too big: %s", str);
		return -1;
	}

	switch (*endptr)
	{
	case '\0':
		break;
	case 'g':
	case 'G':
		chunk *= 1024;
	case 'm':
	case 'M':
		chunk *= 1024;
	case 'k':
	case 'K':
		chunk *= 1024;
		endptr++;
		break;
	case 's':
	case 'S':
		chunk *= SECTOR_SIZE;
		endptr++;
		break;
	default:
		return -1;
	}

	if (*endptr != '\0')
		return -1;

	if (chunk % SECTOR_SIZE)
	{
		error(0, "chunk size is not divisible by %d", SECTOR_SIZE);
		return -1;
	}

	chunk /= SECTOR_SIZE;

	if (chunk < MIN_CHUNK_SIZE)
	{
		error(0, "chunk too small, minimum is %d",
		         (MIN_CHUNK_SIZE * SECTOR_SIZE));
		return -1;
	}

	if (chunk > INT_MAX)
	{
		error(0, "chunk too big: %s", str);
		return -1;
	}

	return (int)chunk;
}

static int clear_metadata(struct md *md, const char *device)
{
	struct era_superblock *sb;
	int supported = 0;
	int valid = 0;

	sb = md_block(md, MD_NOCRC, 0, 0);
	if (sb == NULL)
		return -1;

	if (le32toh(sb->magic) == SUPERBLOCK_MAGIC)
	{
		uint32_t csum;

		csum = crc_update(0xffffffff, &sb->flags,
		                  MD_BLOCK_SIZE - sizeof(uint32_t));
		csum ^= SUPERBLOCK_CSUM_XOR;

		if (le32toh(sb->csum) == csum)
		{
			valid++;

			if (le32toh(sb->version) >= MIN_ERA_VERSION &&
			    le32toh(sb->version) <= MIN_ERA_VERSION)
				supported++;
		}
	}

	if (!force && memcmp(sb, empty_block, MD_BLOCK_SIZE))
	{
		char *what;

		if (valid)
		{
			if (supported)
				what = "valid era superblock";
			else
				what = "unsupported era superblock";
		}
		else
			what = "existing data";

		error(0, "%s found on %s", what, device);

		return -1;
	}

	if (md_write(md, 0, empty_block))
		return -1;

	return 0;
}

static int blkstat(const char *device,
                   unsigned *major, unsigned *minor, uint64_t *sectors)
{
	struct stat st;
	uint64_t size;
	int fd;

	fd = open(device, O_RDONLY | O_DIRECT);
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

	close(fd);

	*sectors = size / SECTOR_SIZE;
	*major = major(st.st_rdev);
	*minor = minor(st.st_rdev);

	return 0;
}

int era_create(int argc, char **argv)
{
	char table[64], uuid[64];
	char *name, *meta, *data, *orig;
	unsigned orig_major, orig_minor;
	unsigned meta_major, meta_minor;
	unsigned data_major, data_minor;
	struct era_dm_info orig_info;
	uint64_t sectors;
	struct md *md;
	int chunk;

	/*
	 * check and save arguments
	 */

	switch (argc)
	{
	case 0:
		error(0, "device name argument expected");
		usage(stderr, 1);
	case 1:
		error(0, "metadata device argument expected");
		usage(stderr, 1);
	case 2:
		error(0, "data device argument expected");
		usage(stderr, 1);
	case 3:
		chunk = DEF_CHUNK_SIZE;
		break;
	case 4:
		chunk = parse_chunk(argv[3]);
		if (chunk == -1)
		{
			error(0, "can't parse chunk size: %s", argv[3]);
			return -1;
		}
		break;
	default:
		error(0, "unknown argument: %s", argv[4]);
		usage(stderr, 1);
	}

	name = argv[0];
	meta = argv[1];
	data = argv[2];

	orig = malloc(16 + strlen(data));
	if (orig == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	sprintf(orig, "%s-orig", name);

	/*
	 * stat data device
	 */

	if (blkstat(data, &data_major, &data_minor, &sectors))
		goto out;

	/*
	 * open metadata device
	 */

	md = md_open(meta, 1);
	if (md == NULL)
		goto out;

	meta_major = md->major;
	meta_minor = md->minor;

	/*
	 * create empty era target
	 */

	snprintf(uuid, sizeof(uuid), "%s%u-%u",
	         UUID_PREFIX, meta_major, meta_minor);

	if (era_dm_create_empty(name, uuid, NULL))
	{
		md_close(md);
		goto out;
	}

	/*
	 * clear and close metadata
	 */

	if (clear_metadata(md, meta))
	{
		(void)era_dm_remove(name);
		md_close(md);
		goto out;
	}

	md_close(md);

	/*
	 * create origin target
	 */

	snprintf(uuid, sizeof(uuid), "%s%u-%u-orig",
	         UUID_PREFIX, meta_major, meta_minor);

	snprintf(table, sizeof(table), "%u:%u 0",
	         data_major, data_minor);

	if (era_dm_create(orig, uuid, 0, sectors,
	                  TARGET_LINEAR, table, &orig_info) == -1)
	{
		(void)era_dm_remove(name);
		goto out;
	}

	orig_major = (unsigned)orig_info.major;
	orig_minor = (unsigned)orig_info.minor;

	/*
	 * load and resume era target
	 */

	snprintf(table, sizeof(table), "%u:%u %u:%u %d",
	         meta_major, meta_minor, orig_major, orig_minor, chunk);

	if (era_dm_load(name, 0, sectors, TARGET_ERA, table, NULL))
	{
		(void)era_dm_remove(orig);
		(void)era_dm_remove(name);
		goto out;
	}

	if (era_dm_resume(name))
	{
		(void)era_dm_remove(orig);
		(void)era_dm_remove(name);
		goto out;
	}

	free(orig);
	return 0;

out:
	free(orig);
	return -1;
}

int era_open(int argc, char **argv)
{
	char table[64], uuid[64];
	char *name, *meta, *data, *orig;
	unsigned orig_major, orig_minor;
	unsigned meta_major, meta_minor;
	unsigned data_major, data_minor;
	struct era_dm_info orig_info;
	struct era_superblock *sb;
	uint32_t nr_blocks;
	uint64_t sectors;
	unsigned chunks;
	struct md *md;
	int chunk;

	/*
	 * check and save arguments
	 */

	switch (argc)
	{
	case 0:
		error(0, "device name argument expected");
		usage(stderr, 1);
	case 1:
		error(0, "metadata device argument expected");
		usage(stderr, 1);
	case 2:
		error(0, "data device argument expected");
		usage(stderr, 1);
	case 3:
		break;
	default:
		error(0, "unknown argument: %s", argv[4]);
		usage(stderr, 1);
	}

	name = argv[0];
	meta = argv[1];
	data = argv[2];

	orig = malloc(16 + strlen(data));
	if (orig == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	sprintf(orig, "%s-orig", name);

	/*
	 * stat data device
	 */

	if (blkstat(data, &data_major, &data_minor, &sectors))
		goto out;

	/*
	 * open metadata device
	 */

	md = md_open(meta, 1);
	if (md == NULL)
		goto out;

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL || era_sb_check(sb))
	{
		md_close(md);
		goto out;
	}

	chunk = (int)le32toh(sb->data_block_size);
	nr_blocks = le32toh(sb->nr_blocks);

	meta_major = md->major;
	meta_minor = md->minor;

	/*
	 * create empty era target
	 */

	snprintf(uuid, sizeof(uuid), "%s%u-%u",
	         UUID_PREFIX, meta_major, meta_minor);

	if (era_dm_create_empty(name, uuid, NULL))
	{
		md_close(md);
		goto out;
	}

	/*
	 * rebuild spacemap
	 */

	if (era_spacemap_rebuild(md))
	{
		(void)era_dm_remove(name);
		md_close(md);
		goto out;
	}

	md_close(md);

	/*
	 * check nr_blocks
	 */

	chunks = (unsigned)((sectors + chunk - 1) / chunk);
	if (!force && chunks != (unsigned)nr_blocks)
	{
		(void)era_dm_remove(name);
		error(0, "can't open era device: data device resized\n"
		         "  chunk size: %i bytes\n"
		         "  total chunks: %u\n"
		         "  chunks on %s: %u",
		         chunk * SECTOR_SIZE,
		         (unsigned)nr_blocks,
		         data, chunks);
		goto out;
	}

	/*
	 * create orig target
	 */

	snprintf(uuid, sizeof(uuid), "%s%u-%u-orig",
	         UUID_PREFIX, meta_major, meta_minor);

	snprintf(table, sizeof(table), "%u:%u 0", data_major, data_minor);

	if (era_dm_create(orig, uuid, 0, sectors,
	                  TARGET_LINEAR, table, &orig_info) == -1)
	{
		(void)era_dm_remove(name);
		goto out;
	}

	orig_major = (unsigned)orig_info.major;
	orig_minor = (unsigned)orig_info.minor;

	/*
	 * load and resume era target
	 */

	snprintf(table, sizeof(table), "%u:%u %u:%u %d",
	         meta_major, meta_minor, orig_major, orig_minor, chunk);

	if (era_dm_load(name, 0, sectors, TARGET_ERA, table, NULL))
	{
		(void)era_dm_remove(orig);
		(void)era_dm_remove(name);
		goto out;
	}

	if (era_dm_resume(name))
	{
		(void)era_dm_remove(orig);
		(void)era_dm_remove(name);
		goto out;
	}

	free(orig);
	return 0;

out:
	free(orig);
	return -1;
}

#define MAX_UUID_SIZE 256
#define MAX_NAME_SIZE 768
int era_close(int argc, char **argv)
{
	char *name, *orig, *uuid;
	struct era_dm_info info;

	/*
	 * check and save arguments
	 */

	switch (argc)
	{
	case 0:
		error(0, "device name argument expected");
		usage(stderr, 1);
	case 1:
		break;
	default:
		error(0, "unknown argument: %s", argv[1]);
		usage(stderr, 1);
	}

	name = argv[0];

	/*
	 * allocate memory for name and uuid
	 */

	uuid = malloc(MAX_UUID_SIZE);
	if (uuid == NULL)
	{
		error(ENOMEM, 0);
		return -1;
	}

	orig = malloc(MAX_NAME_SIZE);
	if (orig == NULL)
	{
		error(ENOMEM, 0);
		free(orig);
		return -1;
	}

	/*
	 * check era device
	 */

	if (era_dm_info(name, NULL, &info,
	                0, NULL, MAX_UUID_SIZE - 16, uuid))
		goto out;

	if (!info.exists)
	{
		error(0, "device does not exists: %s", name);
		goto out;
	}

	/*
	 * check orig device
	 */

	strcat(uuid, "-orig");

	if (era_dm_info(NULL, uuid, &info,
	                MAX_NAME_SIZE, orig, 0, NULL))
	        goto out;

	if (!info.exists)
	{
		error(0, "data device does not exists: %s", uuid);
		goto out;
	}

	/*
	 * remove era and orig devices
	 */

	if (era_dm_remove(name))
		goto out;

	if (era_dm_remove(orig))
		goto out;

	free(orig);
	free(uuid);
	return 0;
out:
	free(orig);
	free(uuid);
	return -1;
}
