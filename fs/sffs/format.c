// SPDX-License-Identifier: GPL-2.0-only
/*
 * BroadOn/Nintendo SFFS filesystem
 *
 * Copyright (C) 2026 Michael "Techflash" Garofalo
 *
 * Based on code from NPLL:
 * Copyright (C) 2026 Techflash
 */

#include <linux/bitmap.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include "format.h"

bool sffs_is_file(const struct sffs_layout *l, const struct sffs_entry *e)
{
	return l->slc ? !!(e->mode & 1) : (e->mode & 3) == 1;
}

bool sffs_is_dir(const struct sffs_layout *l, const struct sffs_entry *e)
{
	return l->slc ? !(e->mode & 1) : (e->mode & 3) == 2;
}

unsigned int sffs_namelen(const struct sffs_entry *e)
{
	return strnlen(e->name, sizeof(e->name));
}

/* Validate only reachable entries, unused FST slots need not be initialized */
int sffs_validate(struct sffs_metadata *m, const struct sffs_layout *layout,
		  u16 *parents)
{
	unsigned long *clusters;
	u16 *queue, idx, sub, other;
	u32 head = 0, tail = 1, count;
	unsigned int len, i;
	struct sffs_entry *e, *a, *b;
	int ret = -EFSCORRUPTED;

	clusters = bitmap_zalloc(SFFS_CLUSTERS, GFP_KERNEL);
	queue = kmalloc_array(SFFS_ENTRIES, sizeof(*queue), GFP_KERNEL);
	if (!clusters || !queue) {
		ret = -ENOMEM;
		goto out;
	}

	memset(parents, 0xff, SFFS_ENTRIES * sizeof(*parents));
	parents[0] = 0;
	queue[0] = 0;
	if (!sffs_is_dir(layout, &m->entries[0]))
		goto out;

	while (head < tail) {
		idx = queue[head++];
		e = &m->entries[idx];
		sub = be16_to_cpu(e->sub);

		if (idx) {
			len = sffs_namelen(e);

			if (!len || memchr(e->name, '/', len) ||
			    (len == 1 && e->name[0] == '.') ||
			    (len == 2 && !memcmp(e->name, "..", 2)))
				goto out;
		}
		if (sffs_is_file(layout, e)) {
			count = DIV_ROUND_UP((u64)be32_to_cpu(e->size), SFFS_CLUSTER_SIZE);

			if (count > SFFS_CLUSTERS)
				goto out;
			for (i = 0; i < count; i++) {
				if (sub < layout->first_cluster ||
				    sub >= layout->super_page / 8 ||
				    test_and_set_bit(sub, clusters))
					goto out;
				sub = be16_to_cpu(m->fat[sub]);
			}
			if (count && sub != SFFS_EOC)
				goto out;
		} else if (sffs_is_dir(layout, e)) {
			while (sub != SFFS_END) {
				if (sub >= SFFS_ENTRIES || parents[sub] != SFFS_END ||
				    tail == SFFS_ENTRIES)
					goto out;

				/* Duplicate names would make VFS lookup ambiguous */
				for (other = be16_to_cpu(e->sub); other != sub;
				     other = be16_to_cpu(m->entries[other].sib)) {
					a = &m->entries[other];
					b = &m->entries[sub];

					if (sffs_namelen(a) == sffs_namelen(b) &&
					    !memcmp(a->name, b->name, sffs_namelen(a)))
						goto out;
				}

				parents[sub] = idx;
				queue[tail++] = sub;
				sub = be16_to_cpu(m->entries[sub].sib);
			}
		} else
			goto out;
	}
	ret = 0;
out:
	bitmap_free(clusters);
	kfree(queue);
	return ret;
}
