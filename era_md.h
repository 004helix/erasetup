/*
 * This file is released under the GPL.
 */

#ifndef __ERA_MD_H__
#define __ERA_MD_H__

#define CACHED 1

/*
 * first 4 bytes in each block is the block checksum:
 * crc32c xored with block type specific data
 */
struct generic_node {
	__le32 csum;
	__u8 data[MD_BLOCK_SIZE - sizeof(__le32)];
} __attribute__ ((packed));

/*
 * metadata device
 */
struct md {
	void     *buffer;          /* block read buffer        */
	void     *cache;;          /* blocks cache             */
	int       fd;              /* device fd                */
	unsigned  dev_blocks;      /* device size / block size */
	unsigned  cache_blocks;    /* allocated cache blocks   */
	unsigned  cache_used;      /* user cache blocks        */
	unsigned  cache_offset[0]; /* block offsets in data    */
};

/*
 * metadata access functions
 */
struct md *md_open(char *device);
void *md_block(struct md *md, unsigned nr, int cached, uint32_t xor);
void md_flush(struct md *md);

#endif
