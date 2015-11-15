/*
 * This file is released under the GPL.
 */

#ifndef __ERA_SNAPSHOT_H__
#define __ERA_SNAPSHOT_H__

#define SNAP_SUPERBLOCK_CSUM_XOR 13116488
#define SNAP_SUPERBLOCK_MAGIC 118135908
#define SNAP_VERSION 1

struct era_snapshot_superblock {
	__le32 csum;
	__le32 flags;
	__le64 blocknr;

	__u8 uuid[UUID_LEN];
	__le64 magic;
	__le32 version;

	__le32 data_block_size;
	__le32 metadata_block_size;
	__le32 nr_blocks;
} __attribute__ ((packed));

#define SNAP_ARRAY_CSUM_XOR 18275559

struct era_snapshot_node {
	__le32 csum;
	__le32 flags;
	__le64 blocknr;

	__le32 era[0];
} __attribute__ ((packed));

#define ERAS_PER_BLOCK \
	((MD_BLOCK_SIZE - sizeof(struct era_snapshot_node)) / sizeof(uint32_t))

int era_snapshot_copy(struct md *md, struct md *sn,
                      uint64_t superblock, unsigned entries);

unsigned long *era_snapshot_getbitmap(struct md *md, unsigned era,
                                      uint64_t superblock, unsigned entries);

#endif