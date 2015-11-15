/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <unistd.h>

#include "era.h"
#include "era_md.h"
#include "era_sm.h"
#include "era_btree.h"

/*
 * callbacks states
 */

struct array_state {
	const char *prefix;
	unsigned total;
	unsigned count;
	unsigned last;
	unsigned maximum;
	unsigned overflow;
};

struct bitset_state {
	const char *prefix;
	unsigned total;
	unsigned count;
	unsigned last;
	unsigned maximum;
	unsigned overflow;
};

struct writesets_state {
	unsigned nr_blocks;
	struct md *md;
};

/*
 * era array callback
 */

static int era_array_cb(void *arg, unsigned size, void *dummy, void *data)
{
	struct array_state *state = arg;
	__le32 *values = data;
	unsigned i;

	if (size == 0)
	{
		switch (state->count)
		{
		case 0:
			break;
		case 1:
			printf("%s<block block=\"%u\" era=\"%u\"/>\n",
			       state->prefix, state->total - 1, state->last);
			break;
		default:
			printf("%s<range begin=\"%u\" end=\"%u\" "
			       "era=\"%u\"/>\n",
			       state->prefix,
			       state->total - state->count,
			       state->total - 1, state->last);
		}

		return 0;
	}

	for (i = 0; i < size; i++)
	{
		unsigned total = state->total;
		uint32_t era;

		if (state->total < state->maximum)
		{
			state->total++;
		}
		else
		{
			state->overflow++;
			continue;
		}

		era = le32toh(values[i]);

		if (state->count == 0)
		{
			state->last = era;
			state->count++;
			continue;
		}

		if (state->last == era)
		{
			state->count++;
			continue;
		}

		if (state->count == 1)
		{
			printf("%s<block block=\"%u\" era=\"%u\"/>\n",
			       state->prefix,
			       total - 1, state->last);
		}
		else
		{
			printf("%s<range begin=\"%u\" end=\"%u\" "
			       "era=\"%u\"/>\n",
			       state->prefix,
			       total - state->count,
			       total - 1, state->last);
		}

		state->last = era;
		state->count = 1;
	}

	return 0;
}

/*
 * dump era array
 */

static int dump_array(struct md *md, uint64_t root, unsigned max,
                      const char *prefix)
{
	struct array_state state = {
		.prefix = prefix,
		.maximum = max,
	};

	if (era_array_walk(md, root, era_array_cb, &state, NULL, NULL))
		return -1;

	if (state.total < state.maximum)
	{
		error(0, "not enough records in era_array: "
		         "expected %u, but got %u",
		         state.maximum, state.total);
		return -1;
	}

	if (state.overflow > 0)
	{
		error(0, "too many records in era_array: "
		         "overflow: %u", state.overflow);
		return -1;
	}

	return 0;
}

/*
 * bitset walk callback
 */

static int bitset_cb(void *arg, unsigned size, void *dummy, void *data)
{
	struct bitset_state *state = arg;
	__le64 *values = data;
	unsigned i, j;

	if (size == 0)
	{
		switch (state->count)
		{
		case 0:
			break;
		case 1:
			printf("%s<block block=\"%u\" bit=\"%u\"/>\n",
			       state->prefix, state->total - 1, state->last);
			break;
		default:
			printf("%s<range begin=\"%u\" end=\"%u\" "
			       "bit=\"%u\"/>\n",
			       state->prefix,
			       state->total - state->count,
			       state->total - 1, state->last);
		}

		return 0;
	}

	for (i = 0; i < size; i++)
	{
		uint64_t val = le64toh(values[i]);

		for (j = 0; j < sizeof(uint64_t) * 8; j++)
		{
			unsigned total = state->total;
			unsigned bit;

			if (state->total < state->maximum)
			{
				state->total++;
			}
			else
			{
				state->overflow++;
				continue;
			}

			bit = (unsigned)((val >> j) & 1);

			if (state->count == 0)
			{
				state->last = bit;
				state->count++;
				continue;
			}

			if (state->last == bit)
			{
				state->count++;
				continue;
			}

			if (state->count == 1)
			{
				printf("%s<block block=\"%u\" bit=\"%u\"/>\n",
				       state->prefix,
				       total - 1, state->last);
			}
			else
			{
				printf("%s<range begin=\"%u\" end=\"%u\" "
				       "bit=\"%u\"/>\n",
				       state->prefix,
				       total - state->count,
				       total - 1, state->last);
			}

			state->last = bit;
			state->count = 1;
		}
	}

	return 0;
}

/*
 * dump era bitset
 */

static int dump_bitset(struct md *md, uint64_t root, unsigned max,
                       const char *prefix)
{
	struct bitset_state state = {
		.prefix = prefix,
		.maximum = max
	};

	if (era_bitset_walk(md, root, bitset_cb, &state, NULL, NULL) == -1)
		return -1;

	if (state.total < state.maximum)
	{
		error(0, "not enough bits in writeset: "
		         "expected %u, but got %u",
		         state.maximum, state.total);
		return -1;
	}

	if (state.overflow != ((64 - max % 64) & 63))
	{
		error(0, "too many bits in writeset: "
		         "expected overflow %u, but got %u",
		         ((64 - max % 64) & 63), state.overflow);
		return -1;
	}

	return 0;
}

/*
 * writeset tree walk callback
 */

static int era_writesets_cb(void *arg, unsigned size, void *keys, void *values)
{
	struct writesets_state *state = arg;
	struct era_writeset *ws;
	uint64_t *eras;
	unsigned i;

	if (size == 0)
		return 0;

	ws = malloc(sizeof(*ws) * size);
	if (ws == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	eras = malloc(sizeof(*eras) * size);
	if (eras == NULL)
	{
		error(ENOMEM, NULL);
		free(ws);
		return -1;
	}

	memcpy(ws, values, size * sizeof(*ws));
	memcpy(eras, keys, size * sizeof(*eras));

	for (i = 0; i < size; i++)
	{
		unsigned era = (unsigned)le64toh(eras[i]);
		uint32_t bits = le32toh(ws[i].nr_bits);
		uint64_t root = le64toh(ws[i].root);

		printf("    <writeset era=\"%u\" bits=\"%u\">\n", era, bits);

		if (bits != state->nr_blocks)
		{
			error(0, "writeset bits mismatch");
			goto out;
		}

		if (dump_bitset(state->md, root, bits, "      ") == -1)
			goto out;

		printf("    </writeset>\n");
	}

	free(eras);
	free(ws);
	return 0;

out:
	free(eras);
	free(ws);
	return -1;
}

/*
 * dump writeset tree
 */

static int dump_writeset(struct md *md, uint64_t root, unsigned max)
{
	struct writesets_state state = {
		.nr_blocks = max,
		.md = md
	};

	return era_writesets_walk(md, root, era_writesets_cb, &state,
	                          NULL, NULL) ? -1 : 0;
}

/*
 * dumpsb command
 */

int era_dumpsb(int argc, char **argv)
{
	struct md *md;
	struct era_superblock *sb;
	unsigned current_writeset_bits;
	uint64_t current_writeset_root;
	uint64_t writeset_tree_root;
	uint64_t era_array_root;
	unsigned nr_blocks;

	if (argc == 0)
	{
		error(0, "metadata device argument expected");
		usage(stderr, 1);
	}

	if (argc > 1)
	{
		error(0, "unknown argument: %s", argv[1]);
		usage(stderr, 1);
	}

	md = md_open(argv[0], 0);
	if (md == NULL)
		return -1;

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL || era_sb_check(sb))
		return -1;

	printf("--- superblock ---------------------------------------"
	       "-----------\n");
	printv(1, "checksum:                    0x%08X\n",
	          le32toh(sb->csum));
	printv(1, "flags:                       0x%08X\n",
	          le32toh(sb->flags));
	printv(1, "blocknr:                     %llu\n",
	          (long long unsigned)le64toh(sb->blocknr));
	printv(0, "uuid:                        %s\n",
	          uuid2str(sb->uuid));
	printv(1, "magic:                       %llu\n",
	          (long long unsigned)le64toh(sb->magic));
	printv(1, "version:                     %u\n",
	          le32toh(sb->version));
	printv(0, "data block size:             %u sectors\n",
	          le32toh(sb->data_block_size));
	printv(0, "metadata block size:         %u sectors\n",
	          le32toh(sb->metadata_block_size));
	printv(0, "total data blocks:           %u\n",
	          le32toh(sb->nr_blocks));
	printv(0, "current era:                 %u\n",
	          le32toh(sb->current_era));
	printv(1, "current writeset/total bits: %u\n",
	          le32toh(sb->current_writeset.nr_bits));
	printv(1, "current writeset/root:       %llu\n",
	          (long long unsigned)le64toh(sb->current_writeset.root));
	printv(1, "writeset tree root:          %llu\n",
	          (long long unsigned)le64toh(sb->writeset_tree_root));
	printv(1, "era array root:              %llu\n",
	          (long long unsigned)le64toh(sb->era_array_root));
	printv(0, "metadata snapshot:           %llu\n",
	          (long long unsigned)le64toh(sb->metadata_snap));

	if (verbose < 2)
		goto done;

	printf("\n--- btrees -----------------------------------------"
	       "-------------\n");

	nr_blocks = le32toh(sb->nr_blocks);
	era_array_root = le64toh(sb->era_array_root);
	writeset_tree_root = le64toh(sb->writeset_tree_root);
	current_writeset_root = le64toh(sb->current_writeset.root);
	current_writeset_bits = le32toh(sb->current_writeset.nr_bits);

	printf("<superblock block_size=\"%u\" blocks=\"%u\" era=\"%u\">\n",
	       le32toh(sb->data_block_size), nr_blocks,
	       le32toh(sb->current_era));

	/*
	 * dump current writeset
	 */

	if (current_writeset_root != 0)
	{
		if (current_writeset_bits != nr_blocks)
		{
			error(0, "current writeset bits count mismatch");
			goto out;
		}

		printf("  <current_writeset bits=\"%u\">\n", nr_blocks);

		if (dump_bitset(md, current_writeset_root, nr_blocks, "    "))
			goto out;

		printf("  </current_writeset>\n");
	}

	/*
	 * dump archived writesets
	 */

	printf("  <writeset_tree>\n");

	if (dump_writeset(md, writeset_tree_root, nr_blocks))
		goto out;

	printf("  </writeset_tree>\n");

	/*
	 * dump era_array
	 */

	printf("  <era_array>\n");

	if (dump_array(md, era_array_root, nr_blocks, "    "))
		goto out;

	printf("  </era_array>\n");

	printf("</superblock>\n");

done:
	md_close(md);
	return 0;

out:
	md_close(md);
	return -1;
}
