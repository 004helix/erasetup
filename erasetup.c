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

#include "crc32c.h"
#include "era.h"
#include "era_md.h"
#include "era_btree.h"

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
static int sb_check(struct era_superblock *sb)
{
	uint32_t magic;
	uint32_t version;

	magic = le32toh(sb->magic);
	if (magic != SUPERBLOCK_MAGIC)
	{
		fprintf(stderr, "invalid superblock magic\n");
		return -1;
	}

	version = le32toh(sb->version);
	if (version < MIN_ERA_VERSION || version > MAX_ERA_VERSION)
	{
		fprintf(stderr, "unsupported era version: %d\n", version);
		return -1;
	}

	return 0;
}

// check and print superblock records
static int sbdump(int argc, char **argv)
{
	struct era_superblock *sb;
	struct md *md;

	if (argc == 0)
	{
		fprintf(stderr, "metadata device required\n");
		usage(1);
	}

	if (argc > 1)
	{
		fprintf(stderr, "unknown argument: %s\n", argv[1]);
		usage(1);
	}

	md = md_open(argv[0]);
	if (md == NULL)
		return -1;

	sb = md_block(md, 0, CACHED, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		return -1;

	if (sb_check(sb) && !force)
		return -1;

	printf("checksum:                    0x%08X\n",
	       le32toh(sb->csum));
	printf("flags:                       0x%08X\n",
	       le32toh(sb->flags));
	printf("blocknr:                     %llu\n",
	       (unsigned long long)le64toh(sb->blocknr));
	printf("uuid:                        %s\n",
	       uuid2str(sb->uuid));
	printf("magic:                       %llu\n",
	       (unsigned long long)le64toh(sb->magic));
	printf("version:                     %u\n",
	       le32toh(sb->version));
	printf("data block size:             %u sectors\n",
	       le32toh(sb->data_block_size));
	printf("metadata block size:         %u sectors\n",
	       le32toh(sb->metadata_block_size));
	printf("total data blocks:           %u\n",
	       le32toh(sb->nr_blocks));
	printf("current era:                 %u\n",
	       le32toh(sb->current_era));
	printf("current writeset/total bits: %u\n",
	       le32toh(sb->current_writeset.nr_bits));
	printf("current writeset/root:       %llu\n",
	       (unsigned long long)le64toh(sb->current_writeset.root));
	printf("writeset tree root:          %llu\n",
	       (unsigned long long)le64toh(sb->writeset_tree_root));
	printf("era array root:              %llu\n",
	       (unsigned long long)le64toh(sb->era_array_root));
	printf("metadata snapshot:           %llu\n",
	       (unsigned long long)le64toh(sb->metadata_snap));

	era_array_walk(md);

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
		return sbdump(argc - optind, &argv[optind]) ? 1 : 0;

	fprintf(stderr, "%s: unknown command: %s\n", argv[0], cmd);
	usage(1);
	return 0;
}
