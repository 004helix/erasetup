/*
 * This file is released under the GPL.
 */

#ifndef __ERA_SNAPSHOT_H__
#define __ERA_SNAPSHOT_H__

#define COPY_ARRAY       0x01
#define COPY_WRITESETS   0x02
#define COPY_CURRENT_WS  0x04 /* unsupported */

#define SNAPSHOT_CSUM_XOR 18275559

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
