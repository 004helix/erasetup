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
#include <getopt.h>
#include <string.h>
#include <endian.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include "era.h"
#include "era_md.h"
#include "era_dm.h"
#include "era_dump.h"
#include "era_btree.h"

#include "era_cmd_create.h"

// verbose printf macro
#define printvf(v, f, ...) \
  do { \
    if ((v) <= verbose) \
      printf((f), __VA_ARGS__); \
  } while (0)

// empty metadata block
void *empty_block;

// options
int verbose = 0;
int force = 0;

// getopt_long
static char *short_options = "hvf";
static struct option long_options[] = {
	{ "help",    no_argument, NULL, 'h' },
	{ "verbose", no_argument, NULL, 'v' },
	{ "force",   no_argument, NULL, 'f' },
	{ NULL,      0,           NULL, 0   }
};

// print usage and exit
void usage(FILE *out, int code)
{
	fprintf(out, "Usage:\n\n"
	"erasetup [-h|--help] [-v|--verbose] [-f|--force]\n"
	"         <command> [command options]\n\n"
	"         dump <metadata-dev>\n"
	"         create <dev-name> <metadata-dev> <data-dev> [chunk-size]\n"
	"         open <dev-name> <metadata-dev> <data-dev>\n"
	"\n");
	exit(code);
}

// convert uuid to string
char *uuid2str(const void *uuid)
{
	static char ascii2hex[16] = "0123456789abcdef";
	static char buffer[1 + UUID_LEN * 2];
	char *chr = buffer;
	int i;

	for (i = 0; i < UUID_LEN; i++)
	{
		*chr++ = ascii2hex[((unsigned char *)uuid)[i] >> 4];
		*chr++ = ascii2hex[((unsigned char *)uuid)[i] & 0x0f];
	}
	*chr = '\0';

	return buffer;
}

// check era superblock
static int era_sb_check(struct era_superblock *sb)
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

// check and print superblock, dump era_array
static int era_dump(int argc, char **argv)
{
	struct era_superblock *sb;
	struct dump *dump;
	struct md *md;

	if (argc == 0)
	{
		fprintf(stderr, "metadata device argument expected\n");
		usage(stderr, 1);
	}

	if (argc > 1)
	{
		fprintf(stderr, "unknown argument: %s\n", argv[1]);
		usage(stderr, 1);
	}

	md = md_open(argv[0]);
	if (md == NULL)
		return -1;

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		return -1;

	if (era_sb_check(sb) && !force)
		return -1;

	printvf(1, "checksum:                    0x%08X\n",
	           le32toh(sb->csum));
	printvf(1, "flags:                       0x%08X\n",
	           le32toh(sb->flags));
	printvf(1, "blocknr:                     %llu\n",
	           (long long unsigned)le64toh(sb->blocknr));
	printvf(0, "uuid:                        %s\n",
	           uuid2str(sb->uuid));
	printvf(1, "magic:                       %llu\n",
	           (long long unsigned)le64toh(sb->magic));
	printvf(1, "version:                     %u\n",
	           le32toh(sb->version));
	printvf(0, "data block size:             %u sectors\n",
	           le32toh(sb->data_block_size));
	printvf(0, "metadata block size:         %u sectors\n",
	           le32toh(sb->metadata_block_size));
	printvf(0, "total data blocks:           %u\n",
	           le32toh(sb->nr_blocks));
	printvf(0, "current era:                 %u\n",
	           le32toh(sb->current_era));
	printvf(1, "current writeset/total bits: %u\n",
	           le32toh(sb->current_writeset.nr_bits));
	printvf(1, "current writeset/root:       %llu\n",
	           (long long unsigned)le64toh(sb->current_writeset.root));
	printvf(1, "writeset tree root:          %llu\n",
	           (long long unsigned)le64toh(sb->writeset_tree_root));
	printvf(1, "era array root:              %llu\n",
	           (long long unsigned)le64toh(sb->era_array_root));
	printvf(0, "metadata snapshot:           %llu\n",
	           (long long unsigned)le64toh(sb->metadata_snap));

	if (argc == 2)
	{
		int i;

		dump = dump_open(argv[1], le32toh(sb->nr_blocks));
		if (dump == NULL)
			return -1;

		if (era_array_dump(md, dump) == -1)
			return -1;

		// ----------------
		if (sb->current_writeset.root != 0)
		{

		if (era_bitset_dump(md, dump) == -1)
			return -1;

		for (i = 0; i < dump->max_bs_ents; i++)
		{
			if (i && (i + 1) % 4 == 0)
				printf("%016llx\n", (long long unsigned)htobe64(dump->bitset[i]));
			else
				printf("%016llx ", (long long unsigned)htobe64(dump->bitset[i]));
		}
		if ((dump->max_bs_ents + 1) % 4 == 0)
			printf("\n");

		} // ------------

		dump_close(dump);
	}

	md_close(md);

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
		case 'h':
			usage(stdout, 0);
		case '?':
			usage(stderr, 1);
		}
	}

	if (optind == argc)
		usage(stdout, 0);

	// init empty block
	empty_block = mmap(NULL, MD_BLOCK_SIZE, PROT_READ,
	                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (empty_block == MAP_FAILED)
	{
		fprintf(stderr, "not enough memory\n");
		return 1;
	}

	// init device mapper
	era_dm_init();

	// execute command
	cmd = argv[optind];
	optind++;

	if (!strcmp(cmd, "dump"))
		return era_dump(argc - optind, &argv[optind]) ? 1 : 0;

	if (!strcmp(cmd, "create"))
		return era_create(argc - optind, &argv[optind]) ? 1 : 0;

	fprintf(stderr, "%s: unknown command: %s\n", argv[0], cmd);
	usage(stderr, 1);
	return 0;
}
