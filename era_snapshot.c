/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include "era.h"
#include "era_md.h"
#include "era_btree.h"
#include "era_snapshot.h"

int era_snapshot_copy(struct md *md, struct md *sn,
                      uint64_t superblock, int flags)
{
	struct era_superblock *sb;

	sb = md_block(md, 0, superblock, SUPERBLOCK_CSUM_XOR);

	if (era_sb_check(sb))
		return -1;

	return 0;
}
