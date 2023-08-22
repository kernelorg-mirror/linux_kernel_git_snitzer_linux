/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2023 Red Hat
 */

#ifndef VDO_INT_MAP_H
#define VDO_INT_MAP_H

#include <linux/compiler.h>
#include <linux/types.h>

#include "hash-map.h"

/**
 * DOC: int_map
 *
 * An int_map associates pointers (void *) with integer keys (u64). NULL pointer values are
 * not supported.
 *
 * The map is implemented as hash table, which should provide constant-time insert, query, and
 * remove operations, although the insert may occasionally grow the table, which is linear in the
 * number of entries in the map. The table will grow as needed to hold new entries, but will not
 * shrink as entries are removed.
 */

static inline int __must_check
vdo_make_int_map(size_t initial_capacity, unsigned int initial_load, struct vdo_hash_map **map_ptr)
{
	return vdo_hash_map_create(HASH_MAP_TYPE_INT, initial_capacity, initial_load, map_ptr);
}

static inline void vdo_free_int_map(struct vdo_hash_map *map)
{
	vdo_hash_map_free(map);
}

static inline size_t vdo_int_map_size(const struct vdo_hash_map *map)
{
	return vdo_hash_map_size(map);
}

static inline void *vdo_int_map_get(struct vdo_hash_map *map, u64 key)
{
	return vdo_hash_map_get(map, &key);
}

static inline int __must_check
vdo_int_map_put(struct vdo_hash_map *map, u64 key, void *new_value,
		bool update, void **old_value_ptr)
{
	return vdo_hash_map_put(map, &key, new_value, update, old_value_ptr);
}

static inline void *vdo_int_map_remove(struct vdo_hash_map *map, u64 key)
{
	return vdo_hash_map_remove(map, &key);
}

#endif /* VDO_INT_MAP_H */
