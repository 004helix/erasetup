/*
 * This file is released under the GPL.
 */

#define BITS_PER_LONG ((int)sizeof(long) * 8)
#define LONGS(bits) (((bits) + BITS_PER_LONG - 1) / BITS_PER_LONG)

static inline int test_bit(unsigned long nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG - 1);
	return (bitmap[offset] >> bit) & 1;
}

static inline void clear_bit(unsigned long int nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG-1);
	bitmap[offset] &= ~(1UL << bit);
}

static inline void set_bit(unsigned long nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG - 1);
	bitmap[offset] |= 1UL << bit;
}

static inline int test_and_set_bit(unsigned long nr, unsigned long *bitmap)
{
	unsigned long offset = nr / BITS_PER_LONG;
	unsigned long bit = nr & (BITS_PER_LONG - 1);
	int rc = (bitmap[offset] >> bit) & 1;
	bitmap[offset] |= 1UL << bit;
	return rc;
}
