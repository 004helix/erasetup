/*
 * This file is released under the GPL.
 */

#ifndef __ERA_DM_H__
#define __ERA_DM_H__

#define UUID_PREFIX     "ERA-"
#define TARGET_ERA      "era"
#define TARGET_ZERO     "zero"
#define TARGET_LINEAR   "linear"
#define TARGET_SNAPSHOT "snapshot"
#define TARGET_ORIGIN   "snapshot-origin"

/* truncated dm_info */
struct era_dm_info {
	int exists;
	uint32_t major;
	uint32_t minor;
	int32_t open_count;
};

/* init / release device mapper library */
void era_dm_init(void);
void era_dm_exit(void);

/* create a device with the given name + uuid and load target + table */
int era_dm_create(const char *name, const char *uuid,
                  uint64_t start, uint64_t size,
                  const char *target, const char *table,
                  struct era_dm_info *info);

/* create a empty device with the given name + uuid */
int era_dm_create_empty(const char *name, const char *uuid,
                        struct era_dm_info *info);

/* load new target + table into the inactive table slot */
int era_dm_load(const char *name,
                uint64_t start, uint64_t size,
                const char *target, const char *table,
                struct era_dm_info *info);

/* suspend device */
int era_dm_suspend(const char *name);

/* un-suspend device */
int era_dm_resume(const char *name);

/* remove device */
int era_dm_remove(const char *name);

/* destroy the table in the inactive table slot */
int era_dm_clear(const char *name);

/* get info by name or uuid */
int era_dm_info(const char *name,
                const char *uuid,
                struct era_dm_info *info,
                size_t name_size, char *name_ptr,
                size_t uuid_size, char *uuid_ptr);

/* list era devices */
int era_dm_list(void);

#endif
