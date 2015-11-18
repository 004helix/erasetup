/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <sys/mman.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "era.h"
#include "era_md.h"
#include "era_dm.h"

#include "era_cmd_basic.h"
#include "era_cmd_dumpsb.h"
#include "era_cmd_status.h"
#include "era_cmd_takesnap.h"
#include "era_cmd_dropsnap.h"

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
	"         create <name> <metadata-dev> <data-dev> [chunk-size]\n"
	"         open <name> <metadata-dev> <data-dev>\n"
	"         close <name>\n"
	"         status [name]\n\n"
	"         takesnap <name> <snapshot-dev>\n"
	"         dropsnap <snapshot-dev>\n\n"
	"         dumpsb <metadata-dev>\n"
	"\n");
	exit(code);
}

// custom error print function
void error(int err, const char *fmt, ...)
{
	static size_t bufsize = 0;
	static char *buffer, *p;
	va_list ap;
	int n;

	if (err == ENOMEM)
	{
		fprintf(stderr, "not enough memory\n");
		return;
	}

	if (bufsize == 0)
	{
		buffer = malloc(512);
		if (!buffer)
			return;
		bufsize = 512;
	}

	while (1)
	{
		va_start(ap, fmt);
		n = vsnprintf(buffer, bufsize, fmt, ap);
		va_end(ap);

		if (n < 0)
			return;

		if (n < bufsize)
			break;

		p = realloc(buffer, bufsize << 1);
		if (p)
		{
			bufsize <<= 1;
			buffer = p;
		}
		else
		{
			free(buffer);
			bufsize = 0;
			return;
		}
	}

	if (err == 0)
		fprintf(stderr, "%s\n", buffer);
	else
		fprintf(stderr, "%s: %s\n", buffer, strerror(err));
}

// convert uuid to string
char *uuid2str(const void *uuid)
{
	static char ascii2hex[16] = "0123456789abcdef";
	static char buffer[6 + UUID_LEN * 2];
	char *chr = buffer;
	int i;

	for (i = 0; i < UUID_LEN; i++)
	{
		*chr++ = ascii2hex[((unsigned char *)uuid)[i] >> 4];
		*chr++ = ascii2hex[((unsigned char *)uuid)[i] & 0x0f];

		if (i == 4 || i == 6 || i == 8 || i == 10)
			*chr++ = '-';
	}

	*chr = '\0';

	return buffer;
}

// check era superblock
int era_sb_check(struct era_superblock *sb)
{
	uint32_t magic;
	uint32_t version;

	magic = le32toh(sb->magic);
	if (magic != SUPERBLOCK_MAGIC)
	{
		error(0, "invalid superblock magic");
		return -1;
	}

	version = le32toh(sb->version);
	if (version < MIN_ERA_VERSION || version > MAX_ERA_VERSION)
	{
		error(0, "unsupported era version: %d", version);
		return -1;
	}

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
		error(ENOMEM, NULL);
		return 1;
	}

	// init device mapper
	era_dm_init();

	// execute command
	cmd = argv[optind];
	optind++;

	argv = &argv[optind];
	argc -= optind;

	if (!strcmp(cmd, "dumpsb"))
		return era_dumpsb(argc, argv) ? 1 : 0;

	if (!strcmp(cmd, "create"))
		return era_create(argc, argv) ? 1 : 0;

	if (!strcmp(cmd, "open"))
		return era_open(argc, argv) ? 1 : 0;

	if (!strcmp(cmd, "close"))
		return era_close(argc, argv) ? 1 : 0;

	if (!strcmp(cmd, "status"))
		return era_status(argc, argv) ? 1 : 0;

	if (!strcmp(cmd, "takesnap"))
		return era_takesnap(argc, argv) ? 1 : 0;

	if (!strcmp(cmd, "dropsnap"))
		return era_dropsnap(argc, argv) ? 1 : 0;

	error(0, "unknown command: %s", cmd);
	usage(stderr, 1);
	return 0;
}
