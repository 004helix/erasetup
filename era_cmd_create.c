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
#include <unistd.h>
#include <string.h>
#include <endian.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include "crc32c.h"
#include "era.h"
#include "era_md.h"
#include "era_dm.h"
#include "era_cmd_create.h"

#include <libdevmapper.h>

static char nametmp[32 + UUID_LEN * 2];
static char uuidstr[32 + UUID_LEN * 2];
static char uuid[UUID_LEN];
static char table[64];

static int parse_chunk(const char *str)
{
	long long chunk;
	char *endptr;

	chunk = strtoll(str, &endptr, 10);

	if (chunk <= 0)
		return -1;

	if (chunk == LLONG_MAX)
	{
		fprintf(stderr, "chunk too big: %s\n", str);
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
		fprintf(stderr, "chunk size is not divisible by %d\n",
		        SECTOR_SIZE);
		return -1;
	}

	chunk /= SECTOR_SIZE;

	if (chunk < MIN_CHUNK_SIZE)
	{
		fprintf(stderr, "chunk too small, minimum is %d\n",
		        (MIN_CHUNK_SIZE * SECTOR_SIZE));
		return -1;
	}

	if (chunk > INT_MAX)
	{
		fprintf(stderr, "chunk too big: %s\n", str);
		return -1;
	}

	return (int)chunk;
}

static int uuid_init(void)
{
	int fd;

	fd = open(RANDOM_DEVICE, O_RDONLY);
	if (fd == -1)
	{
		fprintf(stderr, "can't open rand device %s: %s\n",
		        RANDOM_DEVICE, strerror(errno));
		return -1;
	}

	if (read(fd, uuid, sizeof(uuid)) != sizeof(uuid))
	{
		fprintf(stderr, "can't read rand device %s: %s\n",
		        RANDOM_DEVICE, strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}

int era_create(int argc, char **argv)
{
	char *name, *meta, *data;
	struct era_superblock *sb;
	uint64_t dev_sectors;
	uint64_t dev_size;
	int maj, min, fd;
	struct stat st;
	struct md *md;
	int chunk;

	switch (argc)
	{
	case 0:
		fprintf(stderr, "device name argument expected\n");
		usage(stderr, 1);
	case 1:
		fprintf(stderr, "metadata device argument expected\n");
		usage(stderr, 1);
	case 2:
		fprintf(stderr, "data device argument expected\n");
		usage(stderr, 1);
	case 3:
		chunk = DEF_CHUNK_SIZE;
		break;
	case 4:
		chunk = parse_chunk(argv[3]);
		if (chunk == -1)
		{
			fprintf(stderr, "can't parse chunk size: %s\n",
			        argv[3]);
			return -1;
		}
		break;
	default:
		fprintf(stderr, "unknown argument: %s\n", argv[4]);
		usage(stderr, 1);
	}

	name = argv[0];
	meta = argv[1];
	data = argv[2];

	if (uuid_init())
		return -1;

	md = md_open(argv[1]);
	if (md == NULL)
		return -1;

	sb = md_block(md, MD_NOCRC, 0, 0);
	if (sb == NULL)
	{
		md_close(md);
		return -1;
	}

	if (le32toh(sb->magic) == SUPERBLOCK_MAGIC)
	{
		uint32_t csum;

		csum = crc_update(0xffffffff, &sb->flags,
		                  MD_BLOCK_SIZE - sizeof(uint32_t));
		csum ^= SUPERBLOCK_CSUM_XOR;

		/* TODO TODO TODO */
		if (le32toh(sb->csum) == csum)
		{

		/*
		fprintf(stderr, "valid superblock found on %s\n"
		        "  use --force to ignore this check\n", meta);
		md_close(md);
		return -1;
		*/

		}
	}

	if (!force && memcmp(sb, empty_block, MD_BLOCK_SIZE))
	{
		fprintf(stderr, "existing data found on %s\n"
		        "  use --force to ignore this check\n", meta);
		md_close(md);
		return -1;
	}

	if (md_write(md, 0, empty_block))
	{
		md_close(md);
		return -1;
	}

	fd = open(data, O_RDONLY | O_DIRECT);
	if (fd == -1)
	{
		fprintf(stderr, "can't open data device %s: %s\n",
		        data, strerror(errno));
		md_close(md);
		return -1;
	}

	if (fstat(fd, &st) == -1)
	{
		fprintf(stderr, "can't stat data device %s: %s\n",
		        data, strerror(errno));
		md_close(md);
		close(fd);
		return -1;
	}

	if (!S_ISBLK(st.st_mode))
	{
		fprintf(stderr, "data device is not a block device\n");
		md_close(md);
		close(fd);
		return -1;
	}

	if (ioctl(fd, BLKGETSIZE64, &dev_size))
	{
		fprintf(stderr, "can't get data device size: %s\n",
		        strerror(errno));
		md_close(md);
		close(fd);
		return -1;
	}

	close(fd);

	maj = major(st.st_rdev);
	min = minor(st.st_rdev);

	dev_sectors = dev_size / SECTOR_SIZE;

	snprintf(nametmp, sizeof(nametmp), "erasetup-%s",
	         uuid2str(uuid));

	snprintf(uuidstr, sizeof(uuidstr), "%s%s-tmp",
	         UUID_PREFIX, uuid2str(uuid));

	snprintf(table, sizeof(table), "%d:%d %d:%d %d",
	         md->major, md->minor, maj, min, chunk);

	if (era_dm_create(nametmp, uuidstr, dev_sectors,
	                  TARGET_ERA, table) == -1)
	{
		md_close(md);
		return -1;
	}

	md_close(md);
	return 0;
}
