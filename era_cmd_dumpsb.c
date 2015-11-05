/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <endian.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "era.h"
#include "era_md.h"
#include "era_dm.h"
#include "era_btree.h"

// verbose printf macro
#define printvf(v, f, ...) \
  do { \
    if ((v) <= verbose) \
      printf((f), __VA_ARGS__); \
  } while (0)

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

static int dump_array(struct md *md, unsigned max, const char *prefix)
{
	array = (struct array_state) {
		.prefix = prefix,
		.maximum = max,
	};

	if (era_array_walk(md, era_array_cb))
		return -1;

	if (array.total < array.maximum)
	{
		fprintf(stderr, "not enough records in era_array: "
		        "expected %u, but got %u\n",
		        array.maximum, array.total);
		return -1;
	}

	if (array.overflow > 0)
	{
		fprintf(stderr, "too many records in era_array: "
		        "overflow: %u\n", array.overflow);
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

static int dump_bitset(struct md *md, unsigned max, const char *prefix,
                       uint64_t root)
{
	bitset = (struct bitset_state) {
		.prefix = prefix,
		.maximum = max
	};

	if (era_bitset_walk(md, root, bitset_cb) == -1)
		return -1;

	if (bitset.total < bitset.maximum)
	{
		fprintf(stderr, "not enough bits in writeset: "
		        "expected %u, but got %u\n",
		        bitset.maximum, bitset.total);
		return -1;
	}

	if (bitset.overflow != ((64 - max % 64) & 63))
	{
		fprintf(stderr, "too many bits in writeset: "
		        "expected overflow %u, but got %u\n",
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

		printf("\n%s<writeset era=\"%u\" bits=\"%u\">\n",
		       writeset.prefix, era, bits);

		if (bits != writeset.blocks)
		{
			fprintf(stderr, "writeset bits mismatch\n");
			return -1;
		}

		if (dump_bitset(writeset.md, bits, spaces, root) == -1)
			return -1;

		printf("%s</writeset>\n", writeset.prefix);
	}

	return 0;
}

/*
 * dump writeset tree
 */

static int dump_writeset(struct md *md, unsigned max, const char *prefix)
{
	void *keys_buffer = malloc(MD_BLOCK_SIZE);
	void *vals_buffer = malloc(MD_BLOCK_SIZE);
	int rc;

	if (keys_buffer == NULL || vals_buffer == NULL)
	{
		fprintf(stderr, "not enough memory\n");
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

	rc = era_writeset_walk(md, era_writeset_cb);

	free(vals_buffer);
	free(keys_buffer);

	return rc;
}

/*
 * dumpsb command
 */

int era_dumpsb(int argc, char **argv)
{
	struct era_superblock *sb;
	unsigned nr_blocks;
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

	printf("--- superblock ----------------------------------------------\n");

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

	if (verbose <= 1)
		goto done;

	nr_blocks = le32toh(sb->nr_blocks);

	printf("\n--- btrees --------------------------------------------------\n");
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
			fprintf(stderr, "current writeset bits mismatch\n");
			goto out;
		}

		printf("\n  <current_writeset bits=\"%u\">\n", bits);

		if (dump_bitset(md, bits, "    ", root))
			goto out;

		printf("  </current_writeset>\n");
	}

	/*
	 * dump archived writesets
	 */

	printf("\n  <writeset_tree>\n");

	if (dump_writeset(md, nr_blocks, "    "))
		goto out;

	printf("\n  </writeset_tree>\n");

	/*
	 * dump era_array
	 */

	printf("\n  <era_array>\n");

	if (dump_array(md, nr_blocks, "    "))
		goto out;

	printf("  </era_array>\n");

	printf("\n</superblock>\n");

done:
	md_close(md);
	return 0;

out:
	md_close(md);
	return -1;
}
