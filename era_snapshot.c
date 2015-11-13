/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <endian.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "crc32c.h"
#include "era.h"
#include "era_md.h"
#include "era_btree.h"
#include "era_snapshot.h"

static inline void set_bit(unsigned long nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG - 1);
	bitmap[offset] |= 1UL << bit;
}

struct writeset {
	unsigned era;
	uint64_t root;
	unsigned nr_bits;
	unsigned long *bitmap;
};

struct writesets_state {
	unsigned total;
	unsigned maximum;
	struct writeset *ws;
	struct md *md;
};

struct bitset_state {
	unsigned total;
	unsigned maximum;
	unsigned long *bitmap;
};

struct array_state {
	struct md *sn;
	uint64_t nr;
	unsigned curr;
	unsigned total;
	unsigned maximum;
	unsigned ws_total;
	struct writeset *ws;
};

static int bitset_cb(void *arg, unsigned size, void *dummy, void *data)
{
	struct bitset_state *state = arg;
	uint64_t *values = data;
	unsigned i, j;

	for (i = 0; i < size; i++)
	{
		uint64_t val = le64toh(values[i]);

		if (val == 0)
		{
			state->total += 64;
			continue;
		}

		for (j = 0; j < 64; j++)
		{
			if (state->total >= state->maximum)
				return 0;

			if (!((val >> j) & 1))
			{
				state->total++;
				continue;
			}

			set_bit(state->total, state->bitmap);
			state->total++;
		}
	}

	return 0;
}

static int writesets_cb(void *arg, unsigned size, void *keys, void *values)
{
	struct writesets_state *state = arg;
	struct era_writeset *ews = values;
	uint64_t *eras = keys;
	struct writeset *ws;
	unsigned i, offset;

	if (size == 0)
		return 0;

	offset = state->total;
	ws = realloc(state->ws, sizeof(*ws) * (offset + size));

	if (ws == NULL)
	{
		error(ENOMEM, NULL);
		return -1;
	}

	state->ws = ws;
	state->total += size;

	for (i = 0; i < size; i++)
	{
		ws[offset + i].era = (unsigned)le64toh(eras[i]);
		ws[offset + i].root = le64toh(ews[i].root);
		ws[offset + i].nr_bits = le32toh(ews[i].nr_bits);
		ws[offset + i].bitmap = NULL;
	}

	for (i = offset; i < offset + size; i++)
	{
		unsigned bits = ws[i].nr_bits;
		uint64_t root = ws[i].root;
		struct bitset_state bst;

		ws[i].bitmap = malloc(sizeof(long) * LONGS(bits));

		if (ws[i].bitmap == NULL)
		{
			error(ENOMEM, NULL);
			return -1;
		}

		memset(ws[i].bitmap, 0, sizeof(long) * LONGS(bits));

		bst = (struct bitset_state) {
			.total = 0,
			.maximum = state->maximum,
			.bitmap = ws[i].bitmap,
		};

		if (era_bitset_walk(state->md, root,
		                    bitset_cb, &bst, NULL, NULL) == -1)
			return -1;
	}

	return 0;
}

static int snapshot_write(struct md *sn, uint64_t nr,
                          struct era_snapshot_node *node)
{
	uint32_t csum;

	node->blocknr = htole64(nr);
	node->flags = 0;

	csum = crc_update(0xffffffff, &node->flags,
	                  MD_BLOCK_SIZE - sizeof(node->csum));
	node->csum = htole32(csum ^ SNAPSHOT_CSUM_XOR);

	if (md_write(sn, nr, node))
		return -1;

	return 0;
}

static int array_cb(void *arg, unsigned size, void *dummy, void *data)
{
	struct array_state *state = arg;
	struct era_snapshot_node *node;
	__le32 *eras = data;
	unsigned i;

	node = state->sn->buffer;

	if (state->total == 0)
		memset(node, 0, MD_BLOCK_SIZE);

	if (size == 0)
		return state->curr == 0 ? 0 :
		       snapshot_write(state->sn, state->nr, node);

	for (i = 0; i < size; i++)
	{
		unsigned era;

		if (state->curr == ERAS_PER_BLOCK)
		{
			if (snapshot_write(state->sn, state->nr, node))
				return -1;

			state->nr++;
			state->curr = 0;
			memset(node, 0, MD_BLOCK_SIZE);
		}

		if (state->total >= state->maximum)
			return 0;

		era = le32toh(eras[i]);

		if (state->ws_total > 0)
		{
			unsigned long offset = state->total / BITS_PER_LONG;
			unsigned long sh = state->total & (BITS_PER_LONG - 1);
			unsigned j;

			for (j = 0; j < state->ws_total; j++)
			{
				if (state->total >= state->ws[j].nr_bits)
					continue;

				if (!((state->ws[j].bitmap[offset] >> sh) & 1))
					continue;

				if (state->ws[j].era > era)
					era = state->ws[j].era;
			}
		}

		node->era[state->curr] = htole32(era);
		state->total++;
		state->curr++;
	}

	return 0;
}

int era_snapshot_copy(struct md *md, struct md *sn,
                      uint64_t superblock, unsigned entries)
{
	struct array_state ast;
	struct writesets_state wst;
	struct era_superblock *sb;
	uint64_t writeset_tree_root;
	uint64_t era_array_root;
	int rc = -1;

	sb = md_block(md, MD_CACHED, superblock, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL || era_sb_check(sb))
		return -1;

	era_array_root = le64toh(sb->era_array_root);
	writeset_tree_root = le64toh(sb->writeset_tree_root);

	/*
	 * read writesets and all bisets
	 */

	wst = (struct writesets_state) {
		.total = 0,
		.md = md,
		.ws = NULL,
		.maximum = entries,
	};

	md_flush(md);

	if (era_writesets_walk(md, writeset_tree_root,
	                       writesets_cb, &wst, NULL, NULL))
		goto out;

	/*
	 * copy era_array
	 */

	ast = (struct array_state) {
		.sn = sn,
		.nr = 0,
		.curr = 0,
		.total = 0,
		.maximum = entries,
		.ws_total = wst.total,
		.ws = wst.ws,
	};

	md_flush(md);

	if (era_array_walk(md, era_array_root,
	                   array_cb, &ast, NULL, NULL))
		goto out;

	if (ast.total < entries)
	{
		// TODO: fill tail by zero eras
		error(0, "trunacted era array");
		goto out;
	}

	rc = 0;
out:

	/*
	 * free writesets memory
	 */

	if (wst.ws != NULL && wst.total > 0)
	{
		unsigned i;
		for (i = 0; i < wst.total; i++)
		{
			if (wst.ws[i].bitmap != NULL)
				free(wst.ws[i].bitmap);
		}
		free(wst.ws);
	}

	return rc;
}

struct writesets_find {
	unsigned find_era;
	uint32_t found_bits;
	uint64_t found_root;
};

static int writesets_search_cb(void *arg, unsigned size,
                               void *keys, void *values)
{
	struct writesets_find *state = arg;
	struct era_writeset *ews = values;
	uint64_t *eras = keys;
	unsigned i;

	if (state->found_root > 0)
		return 0;

	for (i = 0; i < size; i++)
	{
		if ((unsigned)le64toh(eras[i]) == state->find_era)
		{
			state->found_bits = le32toh(ews[i].nr_bits);
			state->found_root = le64toh(ews[i].root);
			return 0;
		}
	}

	return 0;
}

unsigned long *era_snapshot_getbitmap(struct md *md, unsigned era,
                                      uint64_t superblock, unsigned entries)
{
	struct era_superblock *sb;
	struct writesets_find wst;
	struct bitset_state bst;
	uint64_t writeset_tree_root;
	unsigned long *bitmap;

	sb = md_block(md, MD_CACHED, superblock, SUPERBLOCK_CSUM_XOR);
	if (sb == NULL || era_sb_check(sb))
		return NULL;

	writeset_tree_root = le64toh(sb->writeset_tree_root);

	wst = (struct writesets_find) {
		.find_era = era,
		.found_root = 0,
		.found_bits = 0,
	};

	md_flush(md);

	if (era_writesets_walk(md, writeset_tree_root,
	                       writesets_search_cb, &wst, NULL, NULL))
		return NULL;

	if (wst.found_root == 0 || wst.found_bits == 0)
	{
		error(0, "can't find writeset for era %u", era);
		return NULL;
	}

	if (wst.found_bits != entries)
	{
		error(0, "wrong bitset size: expected %u, but got %u",
		      entries, wst.found_bits);
		return NULL;
	}

	printv(1, "found bitset root in block %llu for era %u",
	          (long long unsigned)wst.found_root, era);

	bitmap = malloc(sizeof(long) * LONGS(entries));
	if (bitmap == NULL)
	{
		error(ENOMEM, NULL);
		return NULL;
	}

	memset(bitmap, 0, sizeof(long) * LONGS(entries));

	// .... !!!!!!!!!

	free(bitmap);

	return NULL;
}
