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
struct md *md_open(const char *device, int rw)
{
	uint64_t size;
	struct stat st;
	struct md *md;
	int fd, flags;

	flags = (rw ? O_RDWR : O_RDONLY) | O_DIRECT;

	fd = open(device, flags);
	if (fd == -1)
	{
		error(errno, "can't open device %s", device);
		return NULL;
	}

	if (fstat(fd, &st) == -1)
	{
		error(errno, "can't stat device %s", device);
		close(fd);
		return NULL;
	}

	if (!S_ISBLK(st.st_mode))
	{
		error(0, "data device is not a block device");
		close(fd);
		return NULL;
	}

	if (ioctl(fd, BLKGETSIZE64, &size))
	{
		error(errno, "can't get device size %s", device);
		close(fd);
		return NULL;
	}

	md = malloc(sizeof(struct md));
	if (md == NULL)
	{
		error(ENOMEM, NULL);
		close(fd);
		return NULL;
	}

	md->fd = fd;
	md->size = size;
	md->major = major(st.st_rdev);
	md->minor = minor(st.st_rdev);
	md->blocks = size / MD_BLOCK_SIZE;

	md->buffer = mmap(NULL, MD_BLOCK_SIZE, PROT_READ | PROT_WRITE,
	                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (md->buffer == MAP_FAILED)
	{
		error(ENOMEM, NULL);
		close(fd);
		free(md);
		return NULL;
	}

	md->cache_allocated = 2;
	md->cache = mmap(NULL, MD_BLOCK_SIZE * 2, PROT_READ | PROT_WRITE,
	                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (md->cache == MAP_FAILED)
	{
		error(ENOMEM, NULL);
		munmap(md->buffer, MD_BLOCK_SIZE);
		close(fd);
		free(md);
		return NULL;
	}

	md->cache_used = 2;

	md->offset_allocated = 16;
	md->offset = malloc(md->offset_allocated * sizeof(unsigned));
	if (md->offset == NULL)
	{
		error(ENOMEM, NULL);
		munmap(md->cache, MD_BLOCK_SIZE * md->cache_allocated);
		munmap(md->buffer, MD_BLOCK_SIZE);
		close(fd);
		free(md);
		return NULL;
	}

	md_flush(md);

	return md;
}

// read, check and possible cache metadata block
void *md_block(struct md *md, int flags, uint64_t nr, uint32_t xor)
{
	struct generic_node *node;

	// most used case: block already in cache
	if (flags & MD_CACHED && nr < md->offset_allocated &&
	    md->offset[nr] != 0xffffffff)
		return md->cache + MD_BLOCK_SIZE * md->offset[nr];

	// non-cached read
	if (!(flags & MD_CACHED))
	{
		node = md->buffer;

		// read block into buffer
		if (md_read(md, nr, node))
			return NULL;

		// check checksum
		if (!(flags & MD_NOCRC))
		{
			uint32_t csum = crc_update(0xffffffff, node->data,
			                           sizeof(node->data)) ^ xor;
			if (csum != le32toh(node->csum))
			{
				error(0, "bad block checksum: %llu",
				         (long long unsigned)nr);
				return NULL;
			}
		}

		return node;
	}

	// double cache size
	if (md->cache_used == md->cache_allocated)
	{
		unsigned new_alloc;
		void *new_cache;

		new_alloc = md->cache_allocated << 1;
		new_cache = mremap(md->cache,
		                   md->cache_allocated * MD_BLOCK_SIZE,
		                   new_alloc * MD_BLOCK_SIZE,
		                   MREMAP_MAYMOVE);

		if (new_cache == MAP_FAILED)
		{
			error(errno, "mremap failed");
			return NULL;
		}

		md->cache_allocated = new_alloc;
		md->cache = new_cache;
	}

	// node cache offset
	node = md->cache + MD_BLOCK_SIZE * md->cache_used;

	// read block
	if (md_read(md, nr, node))
		return NULL;

	// check checksum
	if (!(flags & MD_NOCRC))
	{
		uint32_t csum = crc_update(0xffffffff, node->data,
		                           sizeof(node->data)) ^ xor;
		if (csum != le32toh(node->csum))
		{
			error(0, "bad block checksum: %llu",
			         (long long unsigned)nr);
			return NULL;
		}
	}

	// increase cache offest size
	if (nr >= md->offset_allocated)
	{
		unsigned new_alloc = md->offset_allocated;
		void *new_offset;

		while (nr >= new_alloc)
			new_alloc <<= 1;

		new_offset = realloc(md->offset, sizeof(unsigned) * new_alloc);

		if (new_offset == NULL)
		{
			error(ENOMEM, NULL);
			return NULL;
		}

		md->offset = new_offset;

		memset(&md->offset[md->offset_allocated], 0xff,
		       sizeof(unsigned) * (new_alloc - md->offset_allocated));

		md->offset_allocated = new_alloc;
	}

	// save in cache
	md->offset[nr] = md->cache_used;
	md->cache_used++;

	return node;
}

// flush cached data
void md_flush(struct md *md)
{
	if (md->cache_used)
	{
		md->cache_used = 0;
		memset(md->offset, 0xff,
		       sizeof(unsigned) * md->offset_allocated);
	}
}

// close metadata device
void md_close(struct md *md)
{
	close(md->fd);
	munmap(md->cache, md->cache_allocated * MD_BLOCK_SIZE);
	munmap(md->buffer, MD_BLOCK_SIZE);
	free(md->offset);
	free(md);
}

// low-level metadata read
int md_read(struct md *md, uint64_t nr, void *data)
{
	if (nr >= md->blocks)
	{
		error(0, "can't read meta-data device: "
		         "block number exceeds total blocks: "
		         "%llu >= %llu",
		         (long long unsigned)nr,
		         (long long unsigned)md->blocks);
		return -1;
	}

	if (pread(md->fd, data, MD_BLOCK_SIZE,
	          nr * MD_BLOCK_SIZE) != MD_BLOCK_SIZE)
	{
		error(errno, "can't read meta-data device");
		return -1;
	}

	return 0;
}

// low-level metadata write
int md_write(struct md *md, uint64_t nr, const void *data)
{
	if (nr >= md->blocks)
	{
		error(0, "can't write meta-data device: "
		         "block number exceeds total blocks: "
		         "%llu >= %llu",
		         (long long unsigned)nr,
		         (long long unsigned)md->blocks);
		return -1;
	}

	if (pwrite(md->fd, data, MD_BLOCK_SIZE,
	           nr * MD_BLOCK_SIZE) != MD_BLOCK_SIZE)
	{
		error(errno, "can't write meta-data device");
		return -1;
	}

	return 0;
}
