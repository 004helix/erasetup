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
#include "era_cmd_basic.h"

#include <libdevmapper.h>

static char uuid_raw[UUID_LEN];
static char uuid[32 + UUID_LEN * 2];
static char table[64];

static int generate_uuid(void)
{
	int fd;

	fd = open(RANDOM_DEVICE, O_RDONLY);
	if (fd == -1)
	{
		fprintf(stderr, "can't open rand device %s: %s\n",
		        RANDOM_DEVICE, strerror(errno));
		return -1;
	}

	if (read(fd, uuid_raw, sizeof(uuid_raw)) != sizeof(uuid_raw))
	{
		fprintf(stderr, "can't read rand device %s: %s\n",
		        RANDOM_DEVICE, strerror(errno));
		return -1;
	}

	close(fd);

	return 0;
}

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

		fprintf(stderr, "%s found on %s\n", what, device);

		return -1;
	}

	if (md_write(md, 0, empty_block))
		return -1;

	return 0;
}

static int blkstat(const char *device,
                   unsigned *maj, unsigned *min, uint64_t *sectors)
{
	struct stat st;
	uint64_t size;
	int fd;

	fd = open(device, O_RDONLY | O_DIRECT);
	if (fd == -1)
	{
		fprintf(stderr, "can't open device %s: %s\n",
		        device, strerror(errno));
		return -1;
	}

	if (fstat(fd, &st) == -1)
	{
		fprintf(stderr, "can't stat device %s: %s\n",
		        device, strerror(errno));
		close(fd);
		return -1;
	}

	if (!S_ISBLK(st.st_mode))
	{
		fprintf(stderr, "device is not a block device: %s\n",
		        device);
		close(fd);
		return -1;
	}

	if (ioctl(fd, BLKGETSIZE64, &size))
	{
		fprintf(stderr, "can't get device size %s: %s\n",
		        device, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);

	*sectors = size / SECTOR_SIZE;

	*maj = major(st.st_rdev);
	*min = minor(st.st_rdev);

	return 0;
}

int era_create(int argc, char **argv)
{
	char *name, *meta, *data, *liner;
	unsigned liner_major, liner_minor;
	unsigned meta_major, meta_minor;
	unsigned data_major, data_minor;
	struct era_dm_info li;
	uint64_t sectors;
	struct md *md;
	int chunk;

	if (generate_uuid())
		return -1;

	/*
	 * check and save arguments
	 */

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

	liner = malloc(16 + strlen(data));
	if (liner == NULL)
	{
		fprintf(stderr, "not enough memory\n");
		return -1;
	}

	sprintf(liner, "%s-data", name);

	/*
	 * stat data device
	 */

	if (blkstat(data, &data_major, &data_minor, &sectors))
		goto out;

	/*
	 * clear metadata device
	 */

	md = md_open(meta);
	if (md == NULL)
		goto out;

	if (clear_metadata(md, meta))
	{
		md_close(md);
		goto out;
	}

	meta_major = md->major;
	meta_minor = md->minor;

	md_close(md);

	/*
	 * create liner target
	 */

	snprintf(uuid, sizeof(uuid), "%s%s-data",
	         UUID_PREFIX, uuid2str(uuid_raw));

	snprintf(table, sizeof(table), "%u:%u 0", data_major, data_minor);

	if (era_dm_create(liner, uuid, sectors,
	                  TARGET_LINEAR, table, &li) == -1)
		goto out;

	liner_major = (unsigned)li.major;
	liner_minor = (unsigned)li.minor;

	/*
	 * create era target
	 */

	snprintf(uuid, sizeof(uuid), "%s%s",
	         UUID_PREFIX, uuid2str(uuid_raw));

	snprintf(table, sizeof(table), "%u:%u %u:%u %d",
	         meta_major, meta_minor, liner_major, liner_minor, chunk);

	if (era_dm_create(name, uuid, sectors,
	                  TARGET_ERA, table, NULL))
	{
		(void)era_dm_remove(liner);
		goto out;
	}

	free(liner);
	return 0;

out:
	free(liner);
	return -1;
}

int era_open(int argc, char **argv)
{
	char *name, *meta, *data, *liner;
	unsigned liner_major, liner_minor;
	unsigned meta_major, meta_minor;
	unsigned data_major, data_minor;
	struct era_superblock *sb;
	struct era_dm_info li;
	uint64_t sectors;
	uint32_t blocks;
	unsigned chunks;
	struct md *md;
	int chunk;

	if (generate_uuid())
		return -1;

	/*
	 * check and save arguments
	 */

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
		break;
	default:
		fprintf(stderr, "unknown argument: %s\n", argv[4]);
		usage(stderr, 1);
	}

	name = argv[0];
	meta = argv[1];
	data = argv[2];

	liner = malloc(16 + strlen(data));
	if (liner == NULL)
	{
		fprintf(stderr, "not enough memory\n");
		return -1;
	}

	sprintf(liner, "%s-data", name);

	/*
	 * stat data device
	 */

	if (blkstat(data, &data_major, &data_minor, &sectors))
		goto out;

	/*
	 * read metadata device
	 */

	md = md_open(meta);
	if (md == NULL)
		goto out;

	sb = md_block(md, 0, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL || era_sb_check(sb))
	{
		md_close(md);
		goto out;
	}

	chunk = (int)le32toh(sb->data_block_size);
	blocks = le32toh(sb->nr_blocks);

	meta_major = md->major;
	meta_minor = md->minor;

	md_close(md);

	chunks = (unsigned)((sectors + chunk - 1) / chunk);
	if (!force && chunks != (unsigned)blocks)
	{
		fprintf(stderr, "can't open era device: data device resized\n"
		                "  chunk size: %i bytes\n"
		                "  total chunks: %u\n"
		                "  chunks on %s: %u\n",
		                chunk * SECTOR_SIZE, (unsigned)blocks,
		                data, chunks);
		goto out;
	}

	/*
	 * create liner target
	 */

	snprintf(uuid, sizeof(uuid), "%s%s-data",
	         UUID_PREFIX, uuid2str(uuid_raw));

	snprintf(table, sizeof(table), "%u:%u 0", data_major, data_minor);

	if (era_dm_create(liner, uuid, sectors,
	                  TARGET_LINEAR, table, &li) == -1)
		goto out;

	liner_major = (unsigned)li.major;
	liner_minor = (unsigned)li.minor;

	/*
	 * create era target
	 */

	snprintf(uuid, sizeof(uuid), "%s%s",
	         UUID_PREFIX, uuid2str(uuid_raw));

	snprintf(table, sizeof(table), "%u:%u %u:%u %d",
	         meta_major, meta_minor, liner_major, liner_minor, chunk);

	if (era_dm_create(name, uuid, sectors,
	                  TARGET_ERA, table, NULL))
	{
		(void)era_dm_remove(liner);
		goto out;
	}

	free(liner);
	return 0;

out:
	free(liner);
	return -1;
}
