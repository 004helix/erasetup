/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <endian.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include "erasetup.h"
#include "crc32c.h"

// options
static int verbose = 0;
static int force = 0;

// getopt_long
static char *short_options = "vf";
static struct option long_options[] = {
	{ "verbose", no_argument, NULL, 'v' },
	{ "force",   no_argument, NULL, 'f' },
	{ NULL,      0,           NULL, 0   }
};

// era superblock
static struct era_superblock *sb = NULL; // on disk

// print usage and exit
static void usage(int code)
{
	fprintf(stderr, "Usage:\n\n"
		"erasetup sbdump <era-metadata-device>\n"
		"\n");
	exit(code);
}

// convert uuid to string
static char *uuid2str(const void *uuid)
{
	static char buffer[1 + (UUID_LEN << 1)];
	int i;

	for (i = 0; i < UUID_LEN; i++)
	{
		int byte = *((unsigned char *)uuid + i);
		sprintf(buffer + (i << 1), "%02x", byte);
	}

	return buffer;
}

// check era superblock
static int check_era_sb(const char *name)
{
	uint32_t csum;
	uint32_t magic;
	uint32_t version;

	magic = le32toh(sb->magic);
	if (magic != SUPERBLOCK_MAGIC)
	{
		if (name)
			fprintf(stderr,
			        "%s: invalid superblock magic\n", name);
		return -1;
	}

	csum = crc_update(0xffffffff, &sb->flags,
		          MD_BLOCK_SIZE - sizeof(csum)) ^ SUPERBLOCK_CSUM_XOR;

	if (csum != le32toh(sb->csum))
	{
		if (name)
			fprintf(stderr,
			        "%s: invalid superblock checksum\n", name);
		return -1;
	}

	version = le32toh(sb->version);
	if (version < MIN_ERA_VERSION || version > MAX_ERA_VERSION)
	{
		if (name)
			fprintf(stderr,
			        "%s: unsupperted era version\n", name);
		return -1;
	}

	return 0;
}

// read era superblock from device
static int read_era_sb(const char *device)
{
	int fd = open(device, O_RDONLY | O_DIRECT);

	if (fd == -1)
	{
		fprintf(stderr, "Unable to open metadata device: %s\n",
		        strerror(errno));
		return -1;
	}

	if (sb == NULL)
	{
		sb = mmap(NULL, MD_BLOCK_SIZE, PROT_READ | PROT_WRITE,
		          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (sb == MAP_FAILED)
		{
			sb = NULL;
			fprintf(stderr, "Not enough memory\n");
			close(fd);
			return -1;
		}
	}

	if (pread(fd, sb, MD_BLOCK_SIZE, 0) != MD_BLOCK_SIZE)
	{
		fprintf(stderr, "Unable to read metadata device: %s\n",
		        strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

// check and print superblock records
static int sbdump(const char *name, int argc, char **argv)
{
	if (argc == 0)
	{
		fprintf(stderr, "%s: metadata device required\n", name);
		usage(1);
	}

	if (argc > 1)
	{
		fprintf(stderr, "%s: unknown argument: %s\n", name, argv[1]);
		usage(1);
	}

	if (read_era_sb(argv[0]))
		return -1;

	if (check_era_sb(name) && !force)
		return -1;

	printf("checksum:            0x%08X\n",
	       le32toh(sb->csum));
	printf("flags:               0x%08X\n",
	       le32toh(sb->flags));
	printf("blocknr:             %llu\n",
	       (unsigned long long)le64toh(sb->blocknr));
	printf("uuid:                %s\n",
	       uuid2str(sb->uuid));
	printf("magic:               %llu\n",
	       (unsigned long long)le64toh(sb->magic));
	printf("version:             %u\n",
	       le32toh(sb->version));
	printf("data block size:     %u sectors\n",
	       le32toh(sb->data_block_size));
	printf("metadata block size: %u sectors\n",
	       le32toh(sb->metadata_block_size));
	printf("total data blocks:   %u\n",
	       le32toh(sb->nr_blocks));
	printf("current era:         %u\n",
	       le32toh(sb->current_era));
	printf("metadata snapshot:   %llu\n",
	       (unsigned long long)le64toh(sb->metadata_snap));


	sb->uuid[0] = 'a';
	sb->uuid[1] = 'b';
	sb->uuid[2] = 0xFF;

	sb->csum = htole32(crc_update(0xffffffff, &sb->flags,
		MD_BLOCK_SIZE - sizeof(sb->csum)) ^ SUPERBLOCK_CSUM_XOR);

//	{
//		int fd = open(argv[0], O_WRONLY | O_DIRECT);
//		pwrite(fd, sb, MD_BLOCK_SIZE, 0);
//		close(fd);
//	}

	return 0;
}

int main(int argc, char **argv)
{
	char *cmd;

	while (1) {
		int option_index = 0;
		int c = getopt_long(argc, argv, short_options,
		                    long_options, &option_index);

		if (c == -1)
			break;

		switch (c)
		{
		case 'v':
			verbose++;
			break;
		case 'f':
			force++;
			break;
		case '?':
			usage(1);
		}
	}

	if (optind == argc)
		usage(0);

	cmd = argv[optind];
	optind++;

	if (!strcmp(cmd, "sbdump"))
		return sbdump(argv[0], argc - optind, &argv[optind]) ? 1 : 0;

	fprintf(stderr, "%s: unknown command: %s\n", argv[0], cmd);
	usage(1);
	return 0;
}
