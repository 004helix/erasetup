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

// verbose printf macro
#define printvf(v, f, ...) \
  do { \
    if ((v) <= verbose) \
      printf((f), __VA_ARGS__); \
  } while (0)

/*
 * display superblock entries
 */

static void display_superblock(struct era_superblock *sb)
{
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
}

/*
 * display space map
 */
static void display_spacemap(struct disk_sm_root *sm)
{
	printf("total metadata blocks:       %llu\n",
	       (long long unsigned)le64toh(sm->nr_blocks));
	printf("allocated metadata blocks:   %llu\n",
	       (long long unsigned)le64toh(sm->nr_allocated));
	printf("bitmap root:                 %llu\n",
	       (long long unsigned)le64toh(sm->bitmap_root));
	printf("ref count root:              %llu\n",
	       (long long unsigned)le64toh(sm->ref_count_root));
}

/*
 * era array walk callback state
 */

static struct array_state {
	const char *prefix;
	unsigned total;
	unsigned count;
	unsigned last;
	unsigned maximum;
	unsigned overflow;
} array;

/*
 * era array callback
 */

static int era_array_cb(unsigned size, void *dummy, void *data)
{
	__le32 *values = data;
	unsigned i;

	if (size == 0)
	{
		switch (array.count)
		{
		case 0:
			break;
		case 1:
			printf("%s<block block=\"%u\" era=\"%u\"/>\n",
			       array.prefix, array.total - 1, array.last);
			break;
		default:
			printf("%s<range begin=\"%u\" end=\"%u\" "
			       "era=\"%u\"/>\n",
			       array.prefix,
			       array.total - array.count,
			       array.total - 1, array.last);
		}

		return 0;
	}

	for (i = 0; i < size; i++)
	{
		unsigned total = array.total;
		uint32_t era;

		if (array.total < array.maximum)
		{
			array.total++;
		}
		else
		{
			array.overflow++;
			continue;
		}

		era = le32toh(values[i]);

		if (array.count == 0)
		{
			array.last = era;
			array.count++;
			continue;
		}

		if (array.last == era)
		{
			array.count++;
			continue;
		}

		if (array.count == 1)
		{
			printf("%s<block block=\"%u\" era=\"%u\"/>\n",
			       array.prefix,
			       total - 1, array.last);
		}
		else
		{
			printf("%s<range begin=\"%u\" end=\"%u\" "
			       "era=\"%u\"/>\n",
			       array.prefix,
			       total - array.count,
			       total - 1, array.last);
		}

		array.last = era;
		array.count = 1;
	}

	return 0;
}

/*
 * dump era array
 */

static int dump_array(struct md *md, uint64_t root, unsigned max,
                      const char *prefix)
{
	array = (struct array_state) {
		.prefix = prefix,
		.maximum = max,
	};

	if (era_array_walk(md, root, era_array_cb, NULL))
		return -1;

	if (array.total < array.maximum)
	{
		error(0, "not enough records in era_array: "
		         "expected %u, but got %u",
		         array.maximum, array.total);
		return -1;
	}

	if (array.overflow > 0)
	{
		error(0, "too many records in era_array: "
		         "overflow: %u", array.overflow);
		return -1;
	}

	return 0;
}

/*
 * bitset walk callback state
 */

static struct bitset_state {
	const char *prefix;
	unsigned total;
	unsigned count;
	unsigned last;
	unsigned maximum;
	unsigned overflow;
} bitset;

/*
 * bitset walk callback
 */

static int bitset_cb(unsigned size, void *dummy, void *data)
{
	__le64 *values = data;
	unsigned i, j;

	if (size == 0)
	{
		switch (bitset.count)
		{
		case 0:
			break;
		case 1:
			printf("%s<block block=\"%u\" bit=\"%u\"/>\n",
			       bitset.prefix, bitset.total - 1, bitset.last);
			break;
		default:
			printf("%s<range begin=\"%u\" end=\"%u\" "
			       "bit=\"%u\"/>\n",
			       bitset.prefix,
			       bitset.total - bitset.count,
			       bitset.total - 1, bitset.last);
		}

		return 0;
	}

	for (i = 0; i < size; i++)
	{
		uint64_t val = le64toh(values[i]);

		for (j = 0; j < sizeof(uint64_t) * 8; j++)
		{
			unsigned total = bitset.total;
			unsigned bit;

			if (bitset.total < bitset.maximum)
			{
				bitset.total++;
			}
			else
			{
				bitset.overflow++;
				continue;
			}

			bit = (unsigned)((val >> j) & 1);

			if (bitset.count == 0)
			{
				bitset.last = bit;
				bitset.count++;
				continue;
			}

			if (bitset.last == bit)
			{
				bitset.count++;
				continue;
			}

			if (bitset.count == 1)
			{
				printf("%s<block block=\"%u\" bit=\"%u\"/>\n",
				       bitset.prefix,
				       total - 1, bitset.last);
			}
			else
			{
				printf("%s<range begin=\"%u\" end=\"%u\" "
				       "bit=\"%u\"/>\n",
				       bitset.prefix,
				       total - bitset.count,
				       total - 1, bitset.last);
			}

			bitset.last = bit;
			bitset.count = 1;
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
	bitset = (struct bitset_state) {
		.prefix = prefix,
		.maximum = max
	};

	if (era_bitset_walk(md, root, bitset_cb, NULL) == -1)
		return -1;

	if (bitset.total < bitset.maximum)
	{
		error(0, "not enough bits in writeset: "
		         "expected %u, but got %u",
		         bitset.maximum, bitset.total);
		return -1;
	}

	if (bitset.overflow != ((64 - max % 64) & 63))
	{
		error(0, "too many bits in writeset: "
		         "expected overflow %u, but got %u",
		         ((64 - max % 64) & 63), bitset.overflow);
		return -1;
	}

	return 0;
}

/*
 * writeset tree walk callback state
 */

static struct writeset_state {
	const char *prefix;
	void *kbuffer;
	void *vbuffer;
	unsigned blocks;
	struct md *md;
} writeset;

/*
 * writeset tree walk callback
 */

static int era_writeset_cb(unsigned size, void *k, void *v)
{
	struct era_writeset *ws;
	uint64_t *keys;
	char spaces[32];
	unsigned i;

	if (size == 0)
		return 0;

	sprintf(spaces, "%s  ", writeset.prefix);

	memcpy(writeset.kbuffer, k, size * sizeof(*keys));
	memcpy(writeset.vbuffer, v, size * sizeof(*ws));

	keys = writeset.kbuffer;
	ws = writeset.vbuffer;

	for (i = 0; i < size; i++)
	{
		unsigned era = keys[i];
		uint32_t bits = ws[i].nr_bits;
		uint64_t root = ws[i].root;

		printf("%s<writeset era=\"%u\" bits=\"%u\">\n",
		       writeset.prefix, era, bits);

		if (bits != writeset.blocks)
		{
			error(0, "writeset bits mismatch");
			return -1;
		}

		if (dump_bitset(writeset.md, root, bits, spaces) == -1)
			return -1;

		printf("%s</writeset>\n", writeset.prefix);
	}

	return 0;
}

/*
 * dump writeset tree
 */

static int dump_writeset(struct md *md, uint64_t root, unsigned max,
                         const char *prefix)
{
	void *keys_buffer = malloc(MD_BLOCK_SIZE);
	void *vals_buffer = malloc(MD_BLOCK_SIZE);
	int rc;

	if (keys_buffer == NULL || vals_buffer == NULL)
	{
		error(ENOMEM, NULL);
		if (keys_buffer)
			free(keys_buffer);
		if (vals_buffer)
			free(vals_buffer);
		return -1;
	}

	writeset = (struct writeset_state) {
		.prefix  = prefix,
		.kbuffer = keys_buffer,
		.vbuffer = vals_buffer,
		.blocks  = max,
		.md      = md
	};

	rc = era_writeset_walk(md, root, era_writeset_cb, NULL);

	free(vals_buffer);
	free(keys_buffer);

	return rc;
}

/*
 * dumpsb command
 */

int era_dumpsb(int argc, char **argv)
{
	unsigned char *refcnt;
	struct era_superblock *sb;
	struct disk_sm_root *sm;
	unsigned nr_blocks;
	struct md *md;
	uint64_t root;

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
	if (sb == NULL)
		return -1;

	if (era_sb_check(sb))
		return -1;

	printf("--- superblock ----------------------------------------------\n");
	display_superblock(sb);

	if (verbose < 1)
		goto done;

	sm = (struct disk_sm_root *)sb->metadata_space_map_root;

	printf("\n--- spacemap root -------------------------------------------\n");
	display_spacemap(sm);

	if (verbose < 2)
		goto done;

	printf("\n--- btrees --------------------------------------------------\n");

	nr_blocks = le32toh(sb->nr_blocks);

	printf("<superblock block_size=\"%u\" blocks=\"%u\" era=\"%u\">\n",
	       le32toh(sb->data_block_size), nr_blocks,
	       le32toh(sb->current_era));

	/*
	 * dump current writeset
	 */

	if (sb->current_writeset.root != 0)
	{
		uint64_t root = le64toh(sb->current_writeset.root);
		uint32_t bits = le32toh(sb->current_writeset.nr_bits);

		if (bits != nr_blocks)
		{
			error(0, "current writeset bits count mismatch");
			goto out;
		}

		printf("  <current_writeset bits=\"%u\">\n", bits);

		if (dump_bitset(md, root, bits, "    "))
			goto out;

		printf("  </current_writeset>\n");
	}

	/*
	 * dump archived writesets
	 */

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		return -1;

	printf("  <writeset_tree>\n");

	root = le64toh(sb->writeset_tree_root);
	if (dump_writeset(md, root, nr_blocks, "    "))
		goto out;

	printf("  </writeset_tree>\n");

	/*
	 * dump era_array
	 */

	sb = md_block(md, MD_CACHED, 0, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL)
		return -1;

	printf("  <era_array>\n");

	root = le64toh(sb->era_array_root);
	if (dump_array(md, root, nr_blocks, "    "))
		goto out;

	printf("  </era_array>\n");

	printf("</superblock>\n");

	if (verbose < 2)
		goto done;

	printf("\n--- spacemap ------------------------------------------------\n");

	refcnt = malloc(md->blocks);
	if (refcnt == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	era_spacemap_walk(md, 9, refcnt);
	write(2, refcnt, md->blocks);

done:
	md_close(md);
	return 0;

out:
	md_close(md);
	return -1;
}
