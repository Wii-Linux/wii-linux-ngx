/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _SFFS_FORMAT_H
#define _SFFS_FORMAT_H

#include <linux/types.h>

#define SFFS_MAGIC 0x53464653
#define SFFS_PAGES 0x40000U
#define SFFS_CLUSTERS 0x8000U
#define SFFS_ENTRIES 0x17ffU
#define SFFS_CLUSTER_SIZE 16384U
#define SFFS_METADATA_SIZE 0x40000U
#define SFFS_PAGE 2048U
#define SFFS_RECORD 2112U
#define SFFS_END 0xffffU
#define SFFS_EOC 0xfffbU
#define SFFS_IMAGE_SIZE ((u64)SFFS_PAGES * SFFS_RECORD)

struct sffs_entry {
	char name[12];
	u8 mode;
	u8 attributes;
	__be16 sub;
	__be16 sib;
	__be32 size;
	__be32 uid;
	__be16 gid;
	__be32 unknown;
} __packed;

struct sffs_metadata {
	u8 magic[4];
	__be32 generation;
	__be32 unknown;
	__be16 fat[SFFS_CLUSTERS];
	struct sffs_entry entries[SFFS_ENTRIES];
	u8 padding[20];
} __packed;

struct sffs_layout {
	const char *magic;
	u32 super_page;
	u32 generations;
	u16 first_cluster;
	bool slc;
};


bool sffs_is_file(const struct sffs_layout *l, const struct sffs_entry *e);
bool sffs_is_dir(const struct sffs_layout *l, const struct sffs_entry *e);
unsigned int sffs_namelen(const struct sffs_entry *e);
int sffs_validate(struct sffs_metadata *m, const struct sffs_layout *layout,
		  u16 *parents);
#endif
