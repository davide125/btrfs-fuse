// SPDX-License-Identifier: MIT

#include "metadata.h"
#include "messages.h"

void free_extent_buffer(struct extent_buffer *eb)
{
	if (!eb)
		return;
	ASSERT(eb->refs > 0);

	eb->refs--;
	if (eb->refs == 0) {
		rb_erase(&eb->node, &eb->fs_info->eb_root);
		free(eb);
	}
}


void btrfs_init_path(struct btrfs_path *path)
{
	memset(path, 0, sizeof(*path));
}

void btrfs_release_path(struct btrfs_path *path)
{
	int i;

	for (i = BTRFS_MAX_LEVEL - 1; i >= 0; i--) {
		free_extent_buffer(path->nodes[i]);
		path->nodes[i] = NULL;
		path->slots[i] = 0;
	}
}

/* Check the sanity of the tree block, before doing the csum check */
static int verify_tree_block(struct extent_buffer *eb, u8 level,
			     u64 transid, struct btrfs_key *first_key)
{
	if (btrfs_header_bytenr(eb) != eb->start) {
		error("tree block %llu bad bytenr, has %llu expect %llu",
			eb->start, btrfs_header_bytenr(eb), eb->start);
		return -EIO;
	}
	if (btrfs_header_level(eb) != level) {
		error("tree block %llu bad level, has %u expect %u",
			eb->start, btrfs_header_level(eb), level);
		return -EIO;
	}
	if (btrfs_header_generation(eb) != transid) {
		error("tree block %llu bad trasid, has %llu expect %llu",
			eb->start, btrfs_header_generation(eb), transid);
		return -EIO;
	}
	if (first_key) {
		struct btrfs_key found_key;

		if (btrfs_header_level(eb))
			btrfs_node_key_to_cpu(eb, &found_key, 0);
		else
			btrfs_item_key_to_cpu(eb, &found_key, 0);
		if (btrfs_comp_cpu_keys(first_key, &found_key)) {
			error(
	"tree block %llu key mismatch, has (%llu %u %llu) want (%llu %u %llu)",
			      eb->start, found_key.objectid, found_key.type,
			      found_key.offset, first_key->objectid,
			      first_key->type, first_key->offset);
			return -EIO;
		}
	}
	return 0;
}

struct extent_buffer *btrfs_read_tree_block(struct btrfs_fs_info *fs_info,
					    u64 logical, u8 level, u64 transid,
					    struct btrfs_key *first_key)
{
	struct rb_node **p = &fs_info->eb_root.rb_node;
	struct rb_node *parent = NULL;
	struct extent_buffer *eb;
	int ret = 0;

	while (*p) {
		parent = *p;
		eb = rb_entry(parent, struct extent_buffer, node);
		if (logical < eb->start) {
			p = &(*p)->rb_left;
		} else if (logical > eb->start) {
			p = &(*p)->rb_right;
		} else {
			/*
			 * Even for cached tree block, we still need to verify
			 * it in case of bad level/transid/first_key.
			 */
			ret = verify_tree_block(eb, level, transid, first_key);
			if (ret < 0)
				return ERR_PTR(ret);

			eb->refs++;
			return eb;
		}
	}

	eb = calloc(1, sizeof(*eb) + fs_info->nodesize);
	if (!eb)
		return ERR_PTR(-ENOMEM);
	eb->start = logical;
	eb->len = fs_info->nodesize;
	eb->refs = 1;
	eb->fs_info = fs_info;
	rb_link_node(&eb->node, parent, p);
	rb_insert_color(&eb->node, &fs_info->eb_root);

	/*
	 * TODO: need to co-operate with volumes.c to grab the chunk
	 * map and read from disk and verify them.
	 */

	return eb;
}

/*
 * Binary search inside an extent buffer.
 *
 * Since btrfs extent buffer has all its items/nodes put together sequentially,
 * we can do a binary search here.
 */
static int generic_bin_search(struct extent_buffer *eb, unsigned long p,
			      int item_size, const struct btrfs_key *key,
			      int max, int *slot)
{
	int low = 0;
	int high = max;
	int mid;
	int ret;
	unsigned long offset;

	while(low < high) {
		struct btrfs_disk_key *tmp;
		struct btrfs_key tmp_cpu_key;

		mid = (low + high) / 2;
		offset = p + mid * item_size;

		tmp = (struct btrfs_disk_key *)(eb->data + offset);
		btrfs_disk_key_to_cpu(&tmp_cpu_key, tmp);
		ret = btrfs_comp_cpu_keys(&tmp_cpu_key, key);

		if (ret < 0)
			low = mid + 1;
		else if (ret > 0)
			high = mid;
		else {
			*slot = mid;
			return 0;
		}
	}
	*slot = low;
	return 1;
}

/* Locate the slot inside the extent buffer */
static int search_slot_in_eb(struct extent_buffer *eb,
			     const struct btrfs_key *key, int *slot)
{
	if (btrfs_header_level(eb) == 0)
		return generic_bin_search(eb,
					  offsetof(struct btrfs_leaf, items),
					  sizeof(struct btrfs_item),
					  key, btrfs_header_nritems(eb),
					  slot);
	else
		return generic_bin_search(eb,
					  offsetof(struct btrfs_node, ptrs),
					  sizeof(struct btrfs_key_ptr),
					  key, btrfs_header_nritems(eb),
					  slot);
}

static struct extent_buffer *read_node_child(struct extent_buffer *parent,
					     int slot)
{
	struct btrfs_key first_key;
	u64 bytenr;
	u64 gen;

	ASSERT(btrfs_header_level(parent) > 0);
	ASSERT(slot < btrfs_header_nritems(parent));

	bytenr = btrfs_node_blockptr(parent, slot);
	gen = btrfs_node_ptr_generation(parent, slot);
	btrfs_node_key_to_cpu(parent, &first_key, slot);

	return btrfs_read_tree_block(parent->fs_info, bytenr,
			btrfs_header_level(parent) - 1, gen, &first_key);
}

/*
 * This is the equivalent of kernel/progs btrfs_search_slot(), without the CoW
 * part.
 *
 * Return 0 if a exact match is found.
 * Return <0 if an error occurred.
 * Return >0 if no exact match is found, and @path will point to the slow where
 * the key should be inserted.
 *
 * This means for >0 case, @path may point to an unused slot, which is not very
 * friendly to call.
 *
 * Thus it's recommened to call btrfs_search_key() and btrfs_search_key_range()
 * wrappers.
 */
static int __search_slot(struct btrfs_root *root, struct btrfs_path *path,
			 struct btrfs_key *key)
{
	int level;
	int ret = 0;

	/* The path must not hold any tree blocks, or we will leak some eb */
	ASSERT(path->nodes[0] == NULL);
	level = btrfs_header_level(root->node);
	path->nodes[level] = extent_buffer_get(root->node);

	for (; level >= 0; level--) {
		int slot;

		ASSERT(path->nodes[level]);
		ret = search_slot_in_eb(path->nodes[level], key, &slot);
		/*
		 * For nodes if we didn't found a match, we should go previous
		 * slot.
		 * As the current slot has key value larger than our target,
		 * continue search will never hit our target, like this example:
		 *
		 * key = (1, 1, 1)
		 *
		 * 	(1, 1, 0)		(1, 2, 0)
		 * 	    /			    \
		 * (1, 1, 0), (1, 1, 1)		(1, 2, 0), (1, 2, 1)
		 *
		 * In above example, we should go through the child of (1, 1, 0)
		 * other than the slot returned (1, 2, 0).
		 * Not to mention returned slot may be unused.
		 */
		if (level && ret && slot > 0)
			slot--;
		path->slots[level] = slot;

		/* Now read the node for next level */
		if (level > 0) {
			struct extent_buffer *eb;

			eb = read_node_child(path->nodes[level], slot);
			if (IS_ERR(eb)) {
				ret = PTR_ERR(eb);
				goto error;
			}
			path->nodes[level - 1] = eb;
		}
	}
	return ret;
error:
	btrfs_release_path(path);
	return ret;
}
