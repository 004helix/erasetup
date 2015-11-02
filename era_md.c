/*
 * This file is released under the GPL.
 */

#define _GNU_SOURCE

#include <linux/fs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>

#include "crc32c.h"
#include "era.h"
#include "era_md.h"

// flush cached data
void md_flush(struct md *md)
{
	memset(md->cache_offset, 0xff, sizeof(unsigned) * md->dev_blocks);
	md->cache_used = 0;
}

// open metadata device
struct md *md_open(char *device)
{
	unsigned dev_blocks;
	uint64_t dev_size;
	struct md *md;
	int fd;

	fd = open(device, O_RDONLY | O_DIRECT);
	if (fd == -1)
	{
		fprintf(stderr, "can't open device %s: %s\n",
		        device, strerror(errno));
		return NULL;
	}

	if (ioctl(fd, BLKGETSIZE64, &dev_size))
	{
		fprintf(stderr, "can't get device size: %s\n",
		        strerror(errno));
		return NULL;
	}

	dev_blocks = dev_size / MD_BLOCK_SIZE;

	md = malloc(sizeof(struct md) + sizeof(unsigned) * dev_blocks);
	if (md == NULL)
		return NULL;

	md->buffer = mmap(NULL, MD_BLOCK_SIZE, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (md->buffer == MAP_FAILED)
	{
		fprintf(stderr, "not enough memory\n");
		free(md);
		return NULL;
	}

	md->cache = mmap(NULL, MD_BLOCK_SIZE, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (md->cache == MAP_FAILED)
	{
		fprintf(stderr, "not enough memory\n");
		munmap(md->buffer, MD_BLOCK_SIZE);
		free(md);
		return NULL;
	}

	md->fd = fd;
	md->dev_blocks = dev_blocks;
	md->cache_blocks = 1;

	md_flush(md);

	return md;
}

// read and cache metadata block
void *md_block(struct md *md, unsigned nr, int cached, uint32_t xor)
{
	struct generic_node *node;
	uint32_t csum;

	// non-cached read
	if (!cached)
	{
		node = md->buffer;

		// read block into buffer
		if (pread(md->fd, node, MD_BLOCK_SIZE,
		          nr * MD_BLOCK_SIZE) != MD_BLOCK_SIZE)
		{
			fprintf(stderr, "can't read meta-data device: %s\n",
			        strerror(errno));
			return NULL;
		}

		// check crc
		csum = crc_update(0xffffffff, node->data,
		                  sizeof(node->data)) ^ xor;
		if (csum != le32toh(node->csum))
		{
			fprintf(stderr, "bad block checksum: %llu\n",
			        (unsigned long long)nr);
			return NULL;
		}

		return node;
	}

	// block already in cache
	if (md->cache_offset[nr] != 0xffffffff)
	{
		// this block already in cache
		return md->cache +
		       MD_BLOCK_SIZE * md->cache_offset[nr];
	}

	// double cache size
	if (md->cache_used == md->cache_blocks)
	{
		unsigned new_blocks;
		void *new_cache;

		new_blocks = md->cache_blocks << 1;
		new_cache = mremap(md->cache,
		                   md->cache_blocks * MD_BLOCK_SIZE,
		                   new_blocks * MD_BLOCK_SIZE, MREMAP_MAYMOVE);

		if (new_cache == MAP_FAILED)
		{
			fprintf(stderr, "mremap() failed: %s\n",
			        strerror(errno));
			return NULL;
		}

		md->cache_blocks = new_blocks;
		md->cache = new_cache;
	}

	// node cache offset
	node = md->cache + MD_BLOCK_SIZE * md->cache_used;

	// read block
	if (pread(md->fd, node, MD_BLOCK_SIZE,
	          nr * MD_BLOCK_SIZE) != MD_BLOCK_SIZE)
	{
		fprintf(stderr, "can't read meta-data device: %s\n",
		        strerror(errno));
		return NULL;
	}

	// check crc
	csum = crc_update(0xffffffff, node->data, sizeof(node->data)) ^ xor;
	if (csum != le32toh(node->csum))
	{
		fprintf(stderr, "bad block checksum: %llu\n",
		        (unsigned long long)nr);
		return NULL;
	}

	// save in cache
	md->cache_offset[nr] = md->cache_used;
	md->cache_used++;

	return node;
}
