/*
 * This file is released under the GPL.
 */

#ifndef __ERA_MD_H__
#define __ERA_MD_H__

// md block size
#define MD_BLOCK_SIZE 4096

// md_block read flags
#define MD_NONE   0x00  // read info buffer
#define MD_CACHED 0x01  // read into cache
#define MD_NOCRC  0x02  // don't check crc

/*
 * first 4 bytes in each block is the block checksum:
 * crc32c xored with block type specific value
 */
struct generic_node {
	__le32 csum;
	__u8 data[MD_BLOCK_SIZE - sizeof(__le32)];
} __attribute__ ((packed));

/*
 * metadata device
 */
struct md {
	int       fd;              /* device fd                */
	unsigned  major;           /* device major number      */
	unsigned  minor;           /* device minor number      */
	unsigned  blocks;          /* device size / block size */
	uint64_t  size;            /* device size in bytes     */

	void     *buffer;          /* block read buffer        */
	void     *cache;           /* blocks cache             */

	unsigned  cache_blocks;    /* allocated cache blocks   */
	unsigned  cache_used;      /* user cache blocks        */
	unsigned  offset_size;     /* TODO: dynamic cache offset size */
	unsigned  cache_offset[0]; /* block offsets in data    */
};

/*
 * metadata access functions
 */

struct md *md_open(const char *device, int rw);
void *md_block(struct md *md, int flags, unsigned nr, uint32_t xor);
void md_flush(struct md *md);
void md_close(struct md *md);

int md_read(struct md *md, unsigned nr, void *data);
int md_write(struct md *md, unsigned nr, const void *data);

#endif
