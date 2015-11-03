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

// open metadata device
struct md *md_open(const char *device)
{
	unsigned dev_blocks;
	uint64_t dev_size;
	struct stat st;
	struct md *md;
	int fd;

	fd = open(device, O_RDWR | O_DIRECT);
	if (fd == -1)
	{
		fprintf(stderr, "can't open device %s: %s\n",
		        device, strerror(errno));
		return NULL;
	}

	if (fstat(fd, &st) == -1)
	{
		fprintf(stderr, "can't stat device %s: %s\n",
		        device, strerror(errno));
		close(fd);
		return NULL;
	}

	if (!S_ISBLK(st.st_mode))
	{
		fprintf(stderr, "data device is not a block device\n");
		close(fd);
		return NULL;
	}

	if (ioctl(fd, BLKGETSIZE64, &dev_size))
	{
		fprintf(stderr, "can't get device size: %s\n",
		        strerror(errno));
		close(fd);
		return NULL;
	}

	dev_blocks = dev_size / MD_BLOCK_SIZE;

	md = malloc(sizeof(struct md) + sizeof(unsigned) * dev_blocks);
	if (md == NULL)
	{
		fprintf(stderr, "not enough memory\n");
		close(fd);
		return NULL;
	}

	md->buffer = mmap(NULL, MD_BLOCK_SIZE, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (md->buffer == MAP_FAILED)
	{
		fprintf(stderr, "not enough memory\n");
		close(fd);
		free(md);
		return NULL;
	}

	md->cache = mmap(NULL, MD_BLOCK_SIZE * 2, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (md->cache == MAP_FAILED)
	{
		fprintf(stderr, "not enough memory\n");
		munmap(md->buffer, MD_BLOCK_SIZE);
		close(fd);
		free(md);
		return NULL;
	}

	md->fd = fd;
	md->major = major(st.st_rdev);
	md->minor = minor(st.st_rdev);
	md->dev_size = dev_size;
	md->dev_blocks = dev_blocks;
	md->cache_blocks = 2;
	md->cache_used = 2;

	md_flush(md);

	return md;
}

// read, check and possible cache metadata block
void *md_block(struct md *md, int flags, unsigned nr, uint32_t xor)
{
	struct generic_node *node;

	// most used case: block already in cache
	if ((flags & MD_CACHED) && md->cache_offset[nr] != 0xffffffff)
		return md->cache + MD_BLOCK_SIZE * md->cache_offset[nr];

	// non-cached read
	if (!(flags & MD_CACHED))
	{
		node = md->buffer;

		// read block into buffer
		if (md_read(md, nr, node))
			return NULL;

		// check crc
		if (!(flags & MD_NOCRC))
		{
			uint32_t csum = crc_update(0xffffffff, node->data,
			                           sizeof(node->data)) ^ xor;
			if (csum != le32toh(node->csum))
			{
				fprintf(stderr, "bad block checksum: %llu\n",
				        (long long unsigned)nr);
				return NULL;
			}
		}

		return node;
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
	if (md_read(md, nr, node))
		return NULL;

	// check crc
	if (!(flags & MD_NOCRC))
	{
		uint32_t csum = crc_update(0xffffffff, node->data,
		                           sizeof(node->data)) ^ xor;
		if (csum != le32toh(node->csum))
		{
			fprintf(stderr, "bad block checksum: %llu\n",
			        (long long unsigned)nr);
			return NULL;
		}
	}

	// save in cache
	md->cache_offset[nr] = md->cache_used;
	md->cache_used++;

	return node;
}

// flush cached data
void md_flush(struct md *md)
{
	if (md->cache_used)
	{
		md->cache_used = 0;
		memset(md->cache_offset, 0xff,
		       sizeof(unsigned) * md->dev_blocks);
	}
}

// close metadata device
void md_close(struct md *md)
{
	munmap(md->cache, md->cache_blocks * MD_BLOCK_SIZE);
	munmap(md->buffer, MD_BLOCK_SIZE);
	close(md->fd);
	free(md);
}

// low-level metadata read
int md_read(struct md *md, unsigned nr, void *data)
{
	if (pread(md->fd, data, MD_BLOCK_SIZE,
	          nr * MD_BLOCK_SIZE) != MD_BLOCK_SIZE)
	{
		fprintf(stderr, "can't read meta-data device: %s\n",
		        strerror(errno));
		return -1;
	}

	return 0;
}

// low-level metadata write
int md_write(struct md *md, unsigned nr, const void *data)
{
	if (pwrite(md->fd, data, MD_BLOCK_SIZE,
	           nr * MD_BLOCK_SIZE) != MD_BLOCK_SIZE)
	{
		fprintf(stderr, "can't write meta-data device: %s\n",
		        strerror(errno));
		return -1;
	}

	return 0;
}
