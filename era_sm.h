/*
 * This file is released under the GPL.
 */

#ifndef __ERA_SM_H__
#define __ERA_SM_H__

#define INDEX_CSUM_XOR 160478
#define BITMAP_CSUM_XOR 240779

struct disk_index_entry {
	__le64 blocknr;
	__le32 nr_free;
	__le32 none_free_before;
} __attribute__ ((packed));

#define MAX_METADATA_BITMAPS 255
struct disk_metadata_index {
	__le32 csum;
	__le32 padding;
	__le64 blocknr;

	struct disk_index_entry index[MAX_METADATA_BITMAPS];
} __attribute__ ((packed));

#define ENTRIES_PER_BYTE 4
struct disk_bitmap_header {
	__le32 csum;
	__le32 not_used;
	__le64 blocknr;
} __attribute__ ((packed));

#define BYTES_PER_BLOCK (MD_BLOCK_SIZE - sizeof(struct disk_bitmap_header))
#define ENTRIES_PER_BLOCK (BYTES_PER_BLOCK * ENTRIES_PER_BYTE)

int era_spacemap_walk(struct md *md, uint64_t root, unsigned char *refcnt);

#endif
