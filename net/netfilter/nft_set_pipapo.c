// SPDX-License-Identifier: GPL-2.0-only

/* PIPAPO: PIle PAcket POlicies: set for arbitrary concatenations of ranges
 *
 * Copyright (c) 2019-2020 Red Hat GmbH
 *
 * Author: Stefano Brivio <sbrivio@redhat.com>
 */

/**
 * DOC: Theory of Operation
 *
 *
 * Problem
 * -------
 *
 * Match packet bytes against entries composed of ranged or non-ranged packet
 * field specifiers, mapping them to arbitrary references. For example:
 *
 * ::
 *
 *               --- fields --->
 *      |    [net],[port],[net]... => [reference]
 *   entries [net],[port],[net]... => [reference]
 *      |    [net],[port],[net]... => [reference]
 *      V    ...
 *
 * where [net] fields can be IP ranges or netmasks, and [port] fields are port
 * ranges. Arbitrary packet fields can be matched.
 *
 *
 * Algorithm Overview
 * ------------------
 *
 * This algorithm is loosely inspired by [Ligatti 2010], and fundamentally
 * relies on the consideration that every contiguous range in a space of b bits
 * can be converted into b * 2 netmasks, from Theorem 3 in [Rottenstreich 2010],
 * as also illustrated in Section 9 of [Kogan 2014].
 *
 * Classification against a number of entries, that require matching given bits
 * of a packet field, is performed by grouping those bits in sets of arbitrary
 * size, and classifying packet bits one group at a time.
 *
 * Example:
 *   to match the source port (16 bits) of a packet, we can divide those 16 bits
 *   in 4 groups of 4 bits each. Given the entry:
 *      0000 0001 0101 1001
 *   and a packet with source port:
 *      0000 0001 1010 1001
 *   first and second groups match, but the third doesn't. We conclude that the
 *   packet doesn't match the given entry.
 *
 * Translate the set to a sequence of lookup tables, one per field. Each table
 * has two dimensions: bit groups to be matched for a single packet field, and
 * all the possible values of said groups (buckets). Input entries are
 * represented as one or more rules, depending on the number of composing
 * netmasks for the given field specifier, and a group match is indicated as a
 * set bit, with number corresponding to the rule index, in all the buckets
 * whose value matches the entry for a given group.
 *
 * Rules are mapped between fields through an array of x, n pairs, with each
 * item mapping a matched rule to one or more rules. The position of the pair in
 * the array indicates the matched rule to be mapped to the next field, x
 * indicates the first rule index in the next field, and n the amount of
 * next-field rules the current rule maps to.
 *
 * The mapping array for the last field maps to the desired references.
 *
 * To match, we perform table lookups using the values of grouped packet bits,
 * and use a sequence of bitwise operations to progressively evaluate rule
 * matching.
 *
 * A stand-alone, reference implementation, also including notes about possible
 * future optimisations, is available at:
 *    https://pipapo.lameexcu.se/
 *
 * Insertion
 * ---------
 *
 * - For each packet field:
 *
 *   - divide the b packet bits we want to classify into groups of size t,
 *     obtaining ceil(b / t) groups
 *
 *      Example: match on destination IP address, with t = 4: 32 bits, 8 groups
 *      of 4 bits each
 *
 *   - allocate a lookup table with one column ("bucket") for each possible
 *     value of a group, and with one row for each group
 *
 *      Example: 8 groups, 2^4 buckets:
 *
 * ::
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0
 *        1
 *        2
 *        3
 *        4
 *        5
 *        6
 *        7
 *
 *   - map the bits we want to classify for the current field, for a given
 *     entry, to a single rule for non-ranged and netmask set items, and to one
 *     or multiple rules for ranges. Ranges are expanded to composing netmasks
 *     by pipapo_expand().
 *
 *      Example: 2 entries, 10.0.0.5:1024 and 192.168.1.0-192.168.2.1:2048
 *      - rule #0: 10.0.0.5
 *      - rule #1: 192.168.1.0/24
 *      - rule #2: 192.168.2.0/31
 *
 *   - insert references to the rules in the lookup table, selecting buckets
 *     according to bit values of a rule in the given group. This is done by
 *     pipapo_insert().
 *
 *      Example: given:
 *      - rule #0: 10.0.0.5 mapping to buckets
 *        < 0 10  0 0   0 0  0 5 >
 *      - rule #1: 192.168.1.0/24 mapping to buckets
 *        < 12 0  10 8  0 1  < 0..15 > < 0..15 > >
 *      - rule #2: 192.168.2.0/31 mapping to buckets
 *        < 12 0  10 8  0 2  0 < 0..1 > >
 *
 *      these bits are set in the lookup table:
 *
 * ::
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0    0                                              1,2
 *        1   1,2                                      0
 *        2    0                                      1,2
 *        3    0                              1,2
 *        4  0,1,2
 *        5    0   1   2
 *        6  0,1,2 1   1   1   1   1   1   1   1   1   1   1   1   1   1   1
 *        7   1,2 1,2  1   1   1  0,1  1   1   1   1   1   1   1   1   1   1
 *
 *   - if this is not the last field in the set, fill a mapping array that maps
 *     rules from the lookup table to rules belonging to the same entry in
 *     the next lookup table, done by pipapo_map().
 *
 *     Note that as rules map to contiguous ranges of rules, given how netmask
 *     expansion and insertion is performed, &union nft_pipapo_map_bucket stores
 *     this information as pairs of first rule index, rule count.
 *
 *      Example: 2 entries, 10.0.0.5:1024 and 192.168.1.0-192.168.2.1:2048,
 *      given lookup table #0 for field 0 (see example above):
 *
 * ::
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0    0                                              1,2
 *        1   1,2                                      0
 *        2    0                                      1,2
 *        3    0                              1,2
 *        4  0,1,2
 *        5    0   1   2
 *        6  0,1,2 1   1   1   1   1   1   1   1   1   1   1   1   1   1   1
 *        7   1,2 1,2  1   1   1  0,1  1   1   1   1   1   1   1   1   1   1
 *
 *      and lookup table #1 for field 1 with:
 *      - rule #0: 1024 mapping to buckets
 *        < 0  0  4  0 >
 *      - rule #1: 2048 mapping to buckets
 *        < 0  0  5  0 >
 *
 * ::
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0   0,1
 *        1   0,1
 *        2                    0   1
 *        3   0,1
 *
 *      we need to map rules for 10.0.0.5 in lookup table #0 (rule #0) to 1024
 *      in lookup table #1 (rule #0) and rules for 192.168.1.0-192.168.2.1
 *      (rules #1, #2) to 2048 in lookup table #2 (rule #1):
 *
 * ::
 *
 *       rule indices in current field: 0    1    2
 *       map to rules in next field:    0    1    1
 *
 *   - if this is the last field in the set, fill a mapping array that maps
 *     rules from the last lookup table to element pointers, also done by
 *     pipapo_map().
 *
 *     Note that, in this implementation, we have two elements (start, end) for
 *     each entry. The pointer to the end element is stored in this array, and
 *     the pointer to the start element is linked from it.
 *
 *      Example: entry 10.0.0.5:1024 has a corresponding &struct nft_pipapo_elem
 *      pointer, 0x66, and element for 192.168.1.0-192.168.2.1:2048 is at 0x42.
 *      From the rules of lookup table #1 as mapped above:
 *
 * ::
 *
 *       rule indices in last field:    0    1
 *       map to elements:             0x66  0x42
 *
 *
 * Matching
 * --------
 *
 * We use a result bitmap, with the size of a single lookup table bucket, to
 * represent the matching state that applies at every algorithm step. This is
 * done by pipapo_lookup().
 *
 * - For each packet field:
 *
 *   - start with an all-ones result bitmap (res_map in pipapo_lookup())
 *
 *   - perform a lookup into the table corresponding to the current field,
 *     for each group, and at every group, AND the current result bitmap with
 *     the value from the lookup table bucket
 *
 * ::
 *
 *      Example: 192.168.1.5 < 12 0  10 8  0 1  0 5 >, with lookup table from
 *      insertion examples.
 *      Lookup table buckets are at least 3 bits wide, we'll assume 8 bits for
 *      convenience in this example. Initial result bitmap is 0xff, the steps
 *      below show the value of the result bitmap after each group is processed:
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0    0                                              1,2
 *        result bitmap is now: 0xff & 0x6 [bucket 12] = 0x6
 *
 *        1   1,2                                      0
 *        result bitmap is now: 0x6 & 0x6 [bucket 0] = 0x6
 *
 *        2    0                                      1,2
 *        result bitmap is now: 0x6 & 0x6 [bucket 10] = 0x6
 *
 *        3    0                              1,2
 *        result bitmap is now: 0x6 & 0x6 [bucket 8] = 0x6
 *
 *        4  0,1,2
 *        result bitmap is now: 0x6 & 0x7 [bucket 0] = 0x6
 *
 *        5    0   1   2
 *        result bitmap is now: 0x6 & 0x2 [bucket 1] = 0x2
 *
 *        6  0,1,2 1   1   1   1   1   1   1   1   1   1   1   1   1   1   1
 *        result bitmap is now: 0x2 & 0x7 [bucket 0] = 0x2
 *
 *        7   1,2 1,2  1   1   1  0,1  1   1   1   1   1   1   1   1   1   1
 *        final result bitmap for this field is: 0x2 & 0x3 [bucket 5] = 0x2
 *
 *   - at the next field, start with a new, all-zeroes result bitmap. For each
 *     bit set in the previous result bitmap, fill the new result bitmap
 *     (fill_map in pipapo_lookup()) with the rule indices from the
 *     corresponding buckets of the mapping field for this field, done by
 *     pipapo_refill()
 *
 *      Example: with mapping table from insertion examples, with the current
 *      result bitmap from the previous example, 0x02:
 *
 * ::
 *
 *       rule indices in current field: 0    1    2
 *       map to rules in next field:    0    1    1
 *
 *      the new result bitmap will be 0x02: rule 1 was set, and rule 1 will be
 *      set.
 *
 *      We can now extend this example to cover the second iteration of the step
 *      above (lookup and AND bitmap): assuming the port field is
 *      2048 < 0  0  5  0 >, with starting result bitmap 0x2, and lookup table
 *      for "port" field from pre-computation example:
 *
 * ::
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0   0,1
 *        1   0,1
 *        2                    0   1
 *        3   0,1
 *
 *       operations are: 0x2 & 0x3 [bucket 0] & 0x3 [bucket 0] & 0x2 [bucket 5]
 *       & 0x3 [bucket 0], resulting bitmap is 0x2.
 *
 *   - if this is the last field in the set, look up the value from the mapping
 *     array corresponding to the final result bitmap
 *
 *      Example: 0x2 resulting bitmap from 192.168.1.5:2048, mapping array for
 *      last field from insertion example:
 *
 * ::
 *
 *       rule indices in last field:    0    1
 *       map to elements:             0x66  0x42
 *
 *      the matching element is at 0x42.
 *
 *
 * References
 * ----------
 *
 * [Ligatti 2010]
 *      A Packet-classification Algorithm for Arbitrary Bitmask Rules, with
 *      Automatic Time-space Tradeoffs
 *      Jay Ligatti, Josh Kuhn, and Chris Gage.
 *      Proceedings of the IEEE International Conference on Computer
 *      Communication Networks (ICCCN), August 2010.
 *      https://www.cse.usf.edu/~ligatti/papers/grouper-conf.pdf
 *
 * [Rottenstreich 2010]
 *      Worst-Case TCAM Rule Expansion
 *      Ori Rottenstreich and Isaac Keslassy.
 *      2010 Proceedings IEEE INFOCOM, San Diego, CA, 2010.
 *      http://citeseerx.ist.psu.edu/viewdoc/download?doi=10.1.1.212.4592&rep=rep1&type=pdf
 *
 * [Kogan 2014]
 *      SAX-PAC (Scalable And eXpressive PAcket Classification)
 *      Kirill Kogan, Sergey Nikolenko, Ori Rottenstreich, William Culhane,
 *      and Patrick Eugster.
 *      Proceedings of the 2014 ACM conference on SIGCOMM, August 2014.
 *      https://www.sigcomm.org/sites/default/files/ccr/papers/2014/August/2619239-2626294.pdf
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables_core.h>
#include <uapi/linux/netfilter/nf_tables.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>

#include "nft_set_pipapo_avx2.h"
#include "nft_set_pipapo.h"

/**
 * pipapo_refill() - For each set bit, set bits from selected mapping table item
 * @map:	Bitmap to be scanned for set bits
 * @len:	Length of bitmap in longs
 * @rules:	Number of rules in field
 * @dst:	Destination bitmap
 * @mt:		Mapping table containing bit set specifiers
 * @match_only:	Find a single bit and return, don't fill
 *
 * Iteration over set bits with __builtin_ctzl(): Daniel Lemire, public domain.
 *
 * For each bit set in map, select the bucket from mapping table with index
 * corresponding to the position of the bit set. Use start bit and amount of
 * bits specified in bucket to fill region in dst.
 *
 * Return: -1 on no match, bit position on 'match_only', 0 otherwise.
 */
int pipapo_refill(unsigned long *map, unsigned int len, unsigned int rules,
		  unsigned long *dst,
		  const union nft_pipapo_map_bucket *mt, bool match_only)
{
	unsigned long bitset;
	unsigned int k;
	int ret = -1;

	for (k = 0; k < len; k++) {
		bitset = map[k];
		while (bitset) {
			unsigned long t = bitset & -bitset;
			int r = __builtin_ctzl(bitset);
			int i = k * BITS_PER_LONG + r;

			if (unlikely(i >= rules)) {
				map[k] = 0;
				return -1;
			}

			if (match_only) {
				bitmap_clear(map, i, 1);
				return i;
			}

			ret = 0;

			bitmap_set(dst, mt[i].to, mt[i].n);

			bitset ^= t;
		}
		map[k] = 0;
	}

	return ret;
}

/**
 * pipapo_get() - Get matching element reference given key data
 * @m:		storage containing the set elements
 * @data:	Key data to be matched against existing elements
 * @genmask:	If set, check that element is active in given genmask
 * @tstamp:	timestamp to check for expired elements
 *
 * For more details, see DOC: Theory of Operation.
 *
 * This is the main lookup function.  It matches key data against either
 * the working match set or the uncommitted copy, depending on what the
 * caller passed to us.
 * nft_pipapo_get (lookup from userspace/control plane) and nft_pipapo_lookup
 * (datapath lookup) pass the active copy.
 * The insertion path will pass the uncommitted working copy.
 *
 * Return: pointer to &struct nft_pipapo_elem on match, NULL otherwise.
 */
static struct nft_pipapo_elem *pipapo_get(const struct nft_pipapo_match *m,
					  const u8 *data, u8 genmask,
					  u64 tstamp)
{
	struct nft_pipapo_scratch *scratch;
	unsigned long *res_map, *fill_map;
	const struct nft_pipapo_field *f;
	bool map_index;
	int i;

	local_bh_disable();

	if (unlikely(!raw_cpu_ptr(m->scratch)))
		goto out;

	scratch = *raw_cpu_ptr(m->scratch);

	map_index = scratch->map_index;

	res_map  = scratch->map + (map_index ? m->bsize_max : 0);
	fill_map = scratch->map + (map_index ? 0 : m->bsize_max);

	pipapo_resmap_init(m, res_map);

	nft_pipapo_for_each_field(f, i, m) {
		bool last = i == m->field_count - 1;
		int b;

		/* For each bit group: select lookup table bucket depending on
		 * packet bytes value, then AND bucket value
		 */
		if (likely(f->bb == 8))
			pipapo_and_field_buckets_8bit(f, res_map, data);
		else
			pipapo_and_field_buckets_4bit(f, res_map, data);
		NFT_PIPAPO_GROUP_BITS_ARE_8_OR_4;

		data += f->groups / NFT_PIPAPO_GROUPS_PER_BYTE(f);

		/* Now populate the bitmap for the next field, unless this is
		 * the last field, in which case return the matched 'ext'
		 * pointer if any.
		 *
		 * Now res_map contains the matching bitmap, and fill_map is the
		 * bitmap for the next field.
		 */
next_match:
		b = pipapo_refill(res_map, f->bsize, f->rules, fill_map, f->mt,
				  last);
		if (b < 0) {
			scratch->map_index = map_index;
			local_bh_enable();

			return NULL;
		}

		if (last) {
			struct nft_pipapo_elem *e;

			e = f->mt[b].e;
			if (unlikely(__nft_set_elem_expired(&e->ext, tstamp) ||
				     !nft_set_elem_active(&e->ext, genmask)))
				goto next_match;

			/* Last field: we're just returning the key without
			 * filling the initial bitmap for the next field, so the
			 * current inactive bitmap is clean and can be reused as
			 * *next* bitmap (not initial) for the next packet.
			 */
			scratch->map_index = map_index;
			local_bh_enable();
			return e;
		}

		/* Swap bitmap indices: res_map is the initial bitmap for the
		 * next field, and fill_map is guaranteed to be all-zeroes at
		 * this point.
		 */
		map_index = !map_index;
		swap(res_map, fill_map);

		data += NFT_PIPAPO_GROUPS_PADDING(f);
	}

out:
	local_bh_enable();
	return NULL;
}

/**
 * nft_pipapo_lookup() - Dataplane fronted for main lookup function
 * @net:	Network namespace
 * @set:	nftables API set representation
 * @key:	pointer to nft registers containing key data
 *
 * This function is called from the data path.  It will search for
 * an element matching the given key in the current active copy.
 *
 * Return: ntables API extension pointer or NULL if no match.
 */
const struct nft_set_ext *
nft_pipapo_lookup(const struct net *net, const struct nft_set *set,
		  const u32 *key)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	u8 genmask = nft_genmask_cur(net);
	const struct nft_pipapo_match *m;
	const struct nft_pipapo_elem *e;

	m = rcu_dereference(priv->match);
	e = pipapo_get(m, (const u8 *)key, genmask, get_jiffies_64());

	return e ? &e->ext : NULL;
}

/**
 * nft_pipapo_get() - Get matching element reference given key data
 * @net:	Network namespace
 * @set:	nftables API set representation
 * @elem:	nftables API element representation containing key data
 * @flags:	Unused
 *
 * This function is called from the control plane path under
 * RCU read lock.
 *
 * Return: set element private pointer or ERR_PTR(-ENOENT).
 */
static struct nft_elem_priv *
nft_pipapo_get(const struct net *net, const struct nft_set *set,
	       const struct nft_set_elem *elem, unsigned int flags)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	struct nft_pipapo_match *m = rcu_dereference(priv->match);
	struct nft_pipapo_elem *e;

	e = pipapo_get(m, (const u8 *)elem->key.val.data,
		       nft_genmask_cur(net), get_jiffies_64());
	if (!e)
		return ERR_PTR(-ENOENT);

	return &e->priv;
}

/**
 * pipapo_realloc_mt() - Reallocate mapping table if needed upon resize
 * @f:		Field containing mapping table
 * @old_rules:	Amount of existing mapped rules
 * @rules:	Amount of new rules to map
 *
 * Return: 0 on success, negative error code on failure.
 */
static int pipapo_realloc_mt(struct nft_pipapo_field *f,
			     unsigned int old_rules, unsigned int rules)
{
	union nft_pipapo_map_bucket *new_mt = NULL, *old_mt = f->mt;
	const unsigned int extra = PAGE_SIZE / sizeof(*new_mt);
	unsigned int rules_alloc = rules;

	might_sleep();

	if (unlikely(rules == 0))
		goto out_free;

	/* growing and enough space left, no action needed */
	if (rules > old_rules && f->rules_alloc > rules)
		return 0;

	/* downsize and extra slack has not grown too large */
	if (rules < old_rules) {
		unsigned int remove = f->rules_alloc - rules;

		if (remove < (2u * extra))
			return 0;
	}

	/* If set needs more than one page of memory for rules then
	 * allocate another extra page to avoid frequent reallocation.
	 */
	if (rules > extra &&
	    check_add_overflow(rules, extra, &rules_alloc))
		return -EOVERFLOW;

	if (rules_alloc > (INT_MAX / sizeof(*new_mt)))
		return -ENOMEM;

	new_mt = kvmalloc_array(rules_alloc, sizeof(*new_mt), GFP_KERNEL_ACCOUNT);
	if (!new_mt)
		return -ENOMEM;

	if (old_mt)
		memcpy(new_mt, old_mt, min(old_rules, rules) * sizeof(*new_mt));

	if (rules > old_rules) {
		memset(new_mt + old_rules, 0,
		       (rules - old_rules) * sizeof(*new_mt));
	}
out_free:
	f->rules_alloc = rules_alloc;
	f->mt = new_mt;

	kvfree(old_mt);

	return 0;
}


/**
 * lt_calculate_size() - Get storage size for lookup table with overflow check
 * @groups:	Amount of bit groups
 * @bb:		Number of bits grouped together in lookup table buckets
 * @bsize:	Size of each bucket in lookup table, in longs
 *
 * Return: allocation size including alignment overhead, negative on overflow
 */
static ssize_t lt_calculate_size(unsigned int groups, unsigned int bb,
				 unsigned int bsize)
{
	ssize_t ret = groups * NFT_PIPAPO_BUCKETS(bb) * sizeof(long);

	if (check_mul_overflow(ret, bsize, &ret))
		return -1;
	if (check_add_overflow(ret, NFT_PIPAPO_ALIGN_HEADROOM, &ret))
		return -1;
	if (ret > INT_MAX)
		return -1;

	return ret;
}

/**
 * pipapo_resize() - Resize lookup or mapping table, or both
 * @f:		Field containing lookup and mapping tables
 * @old_rules:	Previous amount of rules in field
 * @rules:	New amount of rules
 *
 * Increase, decrease or maintain tables size depending on new amount of rules,
 * and copy data over. In case the new size is smaller, throw away data for
 * highest-numbered rules.
 *
 * Return: 0 on success, -ENOMEM on allocation failure.
 */
static int pipapo_resize(struct nft_pipapo_field *f,
			 unsigned int old_rules, unsigned int rules)
{
	long *new_lt = NULL, *new_p, *old_lt = f->lt, *old_p;
	unsigned int new_bucket_size, copy;
	int group, bucket, err;
	ssize_t lt_size;

	if (rules >= NFT_PIPAPO_RULE0_MAX)
		return -ENOSPC;

	new_bucket_size = DIV_ROUND_UP(rules, BITS_PER_LONG);
#ifdef NFT_PIPAPO_ALIGN
	new_bucket_size = roundup(new_bucket_size,
				  NFT_PIPAPO_ALIGN / sizeof(*new_lt));
#endif

	if (new_bucket_size == f->bsize)
		goto mt;

	if (new_bucket_size > f->bsize)
		copy = f->bsize;
	else
		copy = new_bucket_size;

	lt_size = lt_calculate_size(f->groups, f->bb, new_bucket_size);
	if (lt_size < 0)
		return -ENOMEM;

	new_lt = kvzalloc(lt_size, GFP_KERNEL_ACCOUNT);
	if (!new_lt)
		return -ENOMEM;

	new_p = NFT_PIPAPO_LT_ALIGN(new_lt);
	old_p = NFT_PIPAPO_LT_ALIGN(old_lt);

	for (group = 0; group < f->groups; group++) {
		for (bucket = 0; bucket < NFT_PIPAPO_BUCKETS(f->bb); bucket++) {
			memcpy(new_p, old_p, copy * sizeof(*new_p));
			new_p += copy;
			old_p += copy;

			if (new_bucket_size > f->bsize)
				new_p += new_bucket_size - f->bsize;
			else
				old_p += f->bsize - new_bucket_size;
		}
	}

mt:
	err = pipapo_realloc_mt(f, old_rules, rules);
	if (err) {
		kvfree(new_lt);
		return err;
	}

	if (new_lt) {
		f->bsize = new_bucket_size;
		f->lt = new_lt;
		kvfree(old_lt);
	}

	return 0;
}

/**
 * pipapo_bucket_set() - Set rule bit in bucket given group and group value
 * @f:		Field containing lookup table
 * @rule:	Rule index
 * @group:	Group index
 * @v:		Value of bit group
 */
static void pipapo_bucket_set(struct nft_pipapo_field *f, int rule, int group,
			      int v)
{
	unsigned long *pos;

	pos = NFT_PIPAPO_LT_ALIGN(f->lt);
	pos += f->bsize * NFT_PIPAPO_BUCKETS(f->bb) * group;
	pos += f->bsize * v;

	__set_bit(rule, pos);
}

/**
 * pipapo_lt_4b_to_8b() - Switch lookup table group width from 4 bits to 8 bits
 * @old_groups:	Number of current groups
 * @bsize:	Size of one bucket, in longs
 * @old_lt:	Pointer to the current lookup table
 * @new_lt:	Pointer to the new, pre-allocated lookup table
 *
 * Each bucket with index b in the new lookup table, belonging to group g, is
 * filled with the bit intersection between:
 * - bucket with index given by the upper 4 bits of b, from group g, and
 * - bucket with index given by the lower 4 bits of b, from group g + 1
 *
 * That is, given buckets from the new lookup table N(x, y) and the old lookup
 * table O(x, y), with x bucket index, and y group index:
 *
 *	N(b, g) := O(b / 16, g) & O(b % 16, g + 1)
 *
 * This ensures equivalence of the matching results on lookup. Two examples in
 * pictures:
 *
 *              bucket
 *  group  0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 16 17 18 ... 254 255
 *    0                ^
 *    1                |                                                 ^
 *   ...             ( & )                                               |
 *                  /     \                                              |
 *                 /       \                                         .-( & )-.
 *                /  bucket \                                        |       |
 *      group  0 / 1   2   3 \ 4   5   6   7   8   9  10  11  12  13 |14  15 |
 *        0     /             \                                      |       |
 *        1                    \                                     |       |
 *        2                                                          |     --'
 *        3                                                          '-
 *       ...
 */
static void pipapo_lt_4b_to_8b(int old_groups, int bsize,
			       unsigned long *old_lt, unsigned long *new_lt)
{
	int g, b, i;

	for (g = 0; g < old_groups / 2; g++) {
		int src_g0 = g * 2, src_g1 = g * 2 + 1;

		for (b = 0; b < NFT_PIPAPO_BUCKETS(8); b++) {
			int src_b0 = b / NFT_PIPAPO_BUCKETS(4);
			int src_b1 = b % NFT_PIPAPO_BUCKETS(4);
			int src_i0 = src_g0 * NFT_PIPAPO_BUCKETS(4) + src_b0;
			int src_i1 = src_g1 * NFT_PIPAPO_BUCKETS(4) + src_b1;

			for (i = 0; i < bsize; i++) {
				*new_lt = old_lt[src_i0 * bsize + i] &
					  old_lt[src_i1 * bsize + i];
				new_lt++;
			}
		}
	}
}

/**
 * pipapo_lt_8b_to_4b() - Switch lookup table group width from 8 bits to 4 bits
 * @old_groups:	Number of current groups
 * @bsize:	Size of one bucket, in longs
 * @old_lt:	Pointer to the current lookup table
 * @new_lt:	Pointer to the new, pre-allocated lookup table
 *
 * Each bucket with index b in the new lookup table, belonging to group g, is
 * filled with the bit union of:
 * - all the buckets with index such that the upper four bits of the lower byte
 *   equal b, from group g, with g odd
 * - all the buckets with index such that the lower four bits equal b, from
 *   group g, with g even
 *
 * That is, given buckets from the new lookup table N(x, y) and the old lookup
 * table O(x, y), with x bucket index, and y group index:
 *
 *	- with g odd:  N(b, g) := U(O(x, g) for each x : x = (b & 0xf0) >> 4)
 *	- with g even: N(b, g) := U(O(x, g) for each x : x = b & 0x0f)
 *
 * where U() denotes the arbitrary union operation (binary OR of n terms). This
 * ensures equivalence of the matching results on lookup.
 */
static void pipapo_lt_8b_to_4b(int old_groups, int bsize,
			       unsigned long *old_lt, unsigned long *new_lt)
{
	int g, b, bsrc, i;

	memset(new_lt, 0, old_groups * 2 * NFT_PIPAPO_BUCKETS(4) * bsize *
			  sizeof(unsigned long));

	for (g = 0; g < old_groups * 2; g += 2) {
		int src_g = g / 2;

		for (b = 0; b < NFT_PIPAPO_BUCKETS(4); b++) {
			for (bsrc = NFT_PIPAPO_BUCKETS(8) * src_g;
			     bsrc < NFT_PIPAPO_BUCKETS(8) * (src_g + 1);
			     bsrc++) {
				if (((bsrc & 0xf0) >> 4) != b)
					continue;

				for (i = 0; i < bsize; i++)
					new_lt[i] |= old_lt[bsrc * bsize + i];
			}

			new_lt += bsize;
		}

		for (b = 0; b < NFT_PIPAPO_BUCKETS(4); b++) {
			for (bsrc = NFT_PIPAPO_BUCKETS(8) * src_g;
			     bsrc < NFT_PIPAPO_BUCKETS(8) * (src_g + 1);
			     bsrc++) {
				if ((bsrc & 0x0f) != b)
					continue;

				for (i = 0; i < bsize; i++)
					new_lt[i] |= old_lt[bsrc * bsize + i];
			}

			new_lt += bsize;
		}
	}
}

/**
 * pipapo_lt_bits_adjust() - Adjust group size for lookup table if needed
 * @f:		Field containing lookup table
 */
static void pipapo_lt_bits_adjust(struct nft_pipapo_field *f)
{
	unsigned int groups, bb;
	unsigned long *new_lt;
	ssize_t lt_size;

	lt_size = f->groups * NFT_PIPAPO_BUCKETS(f->bb) * f->bsize *
		  sizeof(*f->lt);

	if (f->bb == NFT_PIPAPO_GROUP_BITS_SMALL_SET &&
	    lt_size > NFT_PIPAPO_LT_SIZE_HIGH) {
		groups = f->groups * 2;
		bb = NFT_PIPAPO_GROUP_BITS_LARGE_SET;

		lt_size = lt_calculate_size(groups, bb, f->bsize);
		if (lt_size < 0)
			return;
	} else if (f->bb == NFT_PIPAPO_GROUP_BITS_LARGE_SET &&
		   lt_size < NFT_PIPAPO_LT_SIZE_LOW) {
		groups = f->groups / 2;
		bb = NFT_PIPAPO_GROUP_BITS_SMALL_SET;

		lt_size = lt_calculate_size(groups, bb, f->bsize);
		if (lt_size < 0)
			return;

		/* Don't increase group width if the resulting lookup table size
		 * would exceed the upper size threshold for a "small" set.
		 */
		if (lt_size > NFT_PIPAPO_LT_SIZE_HIGH)
			return;
	} else {
		return;
	}

	new_lt = kvzalloc(lt_size, GFP_KERNEL_ACCOUNT);
	if (!new_lt)
		return;

	NFT_PIPAPO_GROUP_BITS_ARE_8_OR_4;
	if (f->bb == 4 && bb == 8) {
		pipapo_lt_4b_to_8b(f->groups, f->bsize,
				   NFT_PIPAPO_LT_ALIGN(f->lt),
				   NFT_PIPAPO_LT_ALIGN(new_lt));
	} else if (f->bb == 8 && bb == 4) {
		pipapo_lt_8b_to_4b(f->groups, f->bsize,
				   NFT_PIPAPO_LT_ALIGN(f->lt),
				   NFT_PIPAPO_LT_ALIGN(new_lt));
	} else {
		BUG();
	}

	f->groups = groups;
	f->bb = bb;
	kvfree(f->lt);
	f->lt = new_lt;
}

/**
 * pipapo_insert() - Insert new rule in field given input key and mask length
 * @f:		Field containing lookup table
 * @k:		Input key for classification, without nftables padding
 * @mask_bits:	Length of mask; matches field length for non-ranged entry
 *
 * Insert a new rule reference in lookup buckets corresponding to k and
 * mask_bits.
 *
 * Return: 1 on success (one rule inserted), negative error code on failure.
 */
static int pipapo_insert(struct nft_pipapo_field *f, const uint8_t *k,
			 int mask_bits)
{
	unsigned int rule = f->rules, group, ret, bit_offset = 0;

	ret = pipapo_resize(f, f->rules, f->rules + 1);
	if (ret)
		return ret;

	f->rules++;

	for (group = 0; group < f->groups; group++) {
		int i, v;
		u8 mask;

		v = k[group / (BITS_PER_BYTE / f->bb)];
		v &= GENMASK(BITS_PER_BYTE - bit_offset - 1, 0);
		v >>= (BITS_PER_BYTE - bit_offset) - f->bb;

		bit_offset += f->bb;
		bit_offset %= BITS_PER_BYTE;

		if (mask_bits >= (group + 1) * f->bb) {
			/* Not masked */
			pipapo_bucket_set(f, rule, group, v);
		} else if (mask_bits <= group * f->bb) {
			/* Completely masked */
			for (i = 0; i < NFT_PIPAPO_BUCKETS(f->bb); i++)
				pipapo_bucket_set(f, rule, group, i);
		} else {
			/* The mask limit falls on this group */
			mask = GENMASK(f->bb - 1, 0);
			mask >>= mask_bits - group * f->bb;
			for (i = 0; i < NFT_PIPAPO_BUCKETS(f->bb); i++) {
				if ((i & ~mask) == (v & ~mask))
					pipapo_bucket_set(f, rule, group, i);
			}
		}
	}

	pipapo_lt_bits_adjust(f);

	return 1;
}

/**
 * pipapo_step_diff() - Check if setting @step bit in netmask would change it
 * @base:	Mask we are expanding
 * @step:	Step bit for given expansion step
 * @len:	Total length of mask space (set and unset bits), bytes
 *
 * Convenience function for mask expansion.
 *
 * Return: true if step bit changes mask (i.e. isn't set), false otherwise.
 */
static bool pipapo_step_diff(u8 *base, int step, int len)
{
	/* Network order, byte-addressed */
#ifdef __BIG_ENDIAN__
	return !(BIT(step % BITS_PER_BYTE) & base[step / BITS_PER_BYTE]);
#else
	return !(BIT(step % BITS_PER_BYTE) &
		 base[len - 1 - step / BITS_PER_BYTE]);
#endif
}

/**
 * pipapo_step_after_end() - Check if mask exceeds range end with given step
 * @base:	Mask we are expanding
 * @end:	End of range
 * @step:	Step bit for given expansion step, highest bit to be set
 * @len:	Total length of mask space (set and unset bits), bytes
 *
 * Convenience function for mask expansion.
 *
 * Return: true if mask exceeds range setting step bits, false otherwise.
 */
static bool pipapo_step_after_end(const u8 *base, const u8 *end, int step,
				  int len)
{
	u8 tmp[NFT_PIPAPO_MAX_BYTES];
	int i;

	memcpy(tmp, base, len);

	/* Network order, byte-addressed */
	for (i = 0; i <= step; i++)
#ifdef __BIG_ENDIAN__
		tmp[i / BITS_PER_BYTE] |= BIT(i % BITS_PER_BYTE);
#else
		tmp[len - 1 - i / BITS_PER_BYTE] |= BIT(i % BITS_PER_BYTE);
#endif

	return memcmp(tmp, end, len) > 0;
}

/**
 * pipapo_base_sum() - Sum step bit to given len-sized netmask base with carry
 * @base:	Netmask base
 * @step:	Step bit to sum
 * @len:	Netmask length, bytes
 */
static void pipapo_base_sum(u8 *base, int step, int len)
{
	bool carry = false;
	int i;

	/* Network order, byte-addressed */
#ifdef __BIG_ENDIAN__
	for (i = step / BITS_PER_BYTE; i < len; i++) {
#else
	for (i = len - 1 - step / BITS_PER_BYTE; i >= 0; i--) {
#endif
		if (carry)
			base[i]++;
		else
			base[i] += 1 << (step % BITS_PER_BYTE);

		if (base[i])
			break;

		carry = true;
	}
}

/**
 * pipapo_expand() - Expand to composing netmasks, insert into lookup table
 * @f:		Field containing lookup table
 * @start:	Start of range
 * @end:	End of range
 * @len:	Length of value in bits
 *
 * Expand range to composing netmasks and insert corresponding rule references
 * in lookup buckets.
 *
 * Return: number of inserted rules on success, negative error code on failure.
 */
static int pipapo_expand(struct nft_pipapo_field *f,
			 const u8 *start, const u8 *end, int len)
{
	int step, masks = 0, bytes = DIV_ROUND_UP(len, BITS_PER_BYTE);
	u8 base[NFT_PIPAPO_MAX_BYTES];

	memcpy(base, start, bytes);
	while (memcmp(base, end, bytes) <= 0) {
		int err;

		step = 0;
		while (pipapo_step_diff(base, step, bytes)) {
			if (pipapo_step_after_end(base, end, step, bytes))
				break;

			step++;
			if (step >= len) {
				if (!masks) {
					err = pipapo_insert(f, base, 0);
					if (err < 0)
						return err;
					masks = 1;
				}
				goto out;
			}
		}

		err = pipapo_insert(f, base, len - step);

		if (err < 0)
			return err;

		masks++;
		pipapo_base_sum(base, step, bytes);
	}
out:
	return masks;
}

/**
 * pipapo_map() - Insert rules in mapping tables, mapping them between fields
 * @m:		Matching data, including mapping table
 * @map:	Table of rule maps: array of first rule and amount of rules
 *		in next field a given rule maps to, for each field
 * @e:		For last field, nft_set_ext pointer matching rules map to
 */
static void pipapo_map(struct nft_pipapo_match *m,
		       union nft_pipapo_map_bucket map[NFT_PIPAPO_MAX_FIELDS],
		       struct nft_pipapo_elem *e)
{
	struct nft_pipapo_field *f;
	int i, j;

	for (i = 0, f = m->f; i < m->field_count - 1; i++, f++) {
		for (j = 0; j < map[i].n; j++) {
			f->mt[map[i].to + j].to = map[i + 1].to;
			f->mt[map[i].to + j].n = map[i + 1].n;
		}
	}

	/* Last field: map to ext instead of mapping to next field */
	for (j = 0; j < map[i].n; j++)
		f->mt[map[i].to + j].e = e;
}

/**
 * pipapo_free_scratch() - Free per-CPU map at original (not aligned) address
 * @m:		Matching data
 * @cpu:	CPU number
 */
static void pipapo_free_scratch(const struct nft_pipapo_match *m, unsigned int cpu)
{
	struct nft_pipapo_scratch *s;
	void *mem;

	s = *per_cpu_ptr(m->scratch, cpu);
	if (!s)
		return;

	mem = s;
	mem -= s->align_off;
	kvfree(mem);
}

/**
 * pipapo_realloc_scratch() - Reallocate scratch maps for partial match results
 * @clone:	Copy of matching data with pending insertions and deletions
 * @bsize_max:	Maximum bucket size, scratch maps cover two buckets
 *
 * Return: 0 on success, -ENOMEM on failure.
 */
static int pipapo_realloc_scratch(struct nft_pipapo_match *clone,
				  unsigned long bsize_max)
{
	int i;

	for_each_possible_cpu(i) {
		struct nft_pipapo_scratch *scratch;
#ifdef NFT_PIPAPO_ALIGN
		void *scratch_aligned;
		u32 align_off;
#endif
		scratch = kvzalloc_node(struct_size(scratch, map, bsize_max * 2) +
					NFT_PIPAPO_ALIGN_HEADROOM,
					GFP_KERNEL_ACCOUNT, cpu_to_node(i));
		if (!scratch) {
			/* On failure, there's no need to undo previous
			 * allocations: this means that some scratch maps have
			 * a bigger allocated size now (this is only called on
			 * insertion), but the extra space won't be used by any
			 * CPU as new elements are not inserted and m->bsize_max
			 * is not updated.
			 */
			return -ENOMEM;
		}

		pipapo_free_scratch(clone, i);

#ifdef NFT_PIPAPO_ALIGN
		/* Align &scratch->map (not the struct itself): the extra
		 * %NFT_PIPAPO_ALIGN_HEADROOM bytes passed to kzalloc_node()
		 * above guarantee we can waste up to those bytes in order
		 * to align the map field regardless of its offset within
		 * the struct.
		 */
		BUILD_BUG_ON(offsetof(struct nft_pipapo_scratch, map) > NFT_PIPAPO_ALIGN_HEADROOM);

		scratch_aligned = NFT_PIPAPO_LT_ALIGN(&scratch->map);
		scratch_aligned -= offsetof(struct nft_pipapo_scratch, map);
		align_off = scratch_aligned - (void *)scratch;

		scratch = scratch_aligned;
		scratch->align_off = align_off;
#endif
		*per_cpu_ptr(clone->scratch, i) = scratch;
	}

	return 0;
}

static bool nft_pipapo_transaction_mutex_held(const struct nft_set *set)
{
#ifdef CONFIG_PROVE_LOCKING
	const struct net *net = read_pnet(&set->net);

	return lockdep_is_held(&nft_pernet(net)->commit_mutex);
#else
	return true;
#endif
}

static struct nft_pipapo_match *pipapo_clone(struct nft_pipapo_match *old);

/**
 * pipapo_maybe_clone() - Build clone for pending data changes, if not existing
 * @set:	nftables API set representation
 *
 * Return: newly created or existing clone, if any. NULL on allocation failure
 */
static struct nft_pipapo_match *pipapo_maybe_clone(const struct nft_set *set)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	struct nft_pipapo_match *m;

	if (priv->clone)
		return priv->clone;

	m = rcu_dereference_protected(priv->match,
				      nft_pipapo_transaction_mutex_held(set));
	priv->clone = pipapo_clone(m);

	return priv->clone;
}

/**
 * nft_pipapo_insert() - Validate and insert ranged elements
 * @net:	Network namespace
 * @set:	nftables API set representation
 * @elem:	nftables API element representation containing key data
 * @elem_priv:	Filled with pointer to &struct nft_set_ext in inserted element
 *
 * Return: 0 on success, error pointer on failure.
 */
static int nft_pipapo_insert(const struct net *net, const struct nft_set *set,
			     const struct nft_set_elem *elem,
			     struct nft_elem_priv **elem_priv)
{
	const struct nft_set_ext *ext = nft_set_elem_ext(set, elem->priv);
	union nft_pipapo_map_bucket rulemap[NFT_PIPAPO_MAX_FIELDS];
	const u8 *start = (const u8 *)elem->key.val.data, *end;
	struct nft_pipapo_match *m = pipapo_maybe_clone(set);
	u8 genmask = nft_genmask_next(net);
	struct nft_pipapo_elem *e, *dup;
	u64 tstamp = nft_net_tstamp(net);
	struct nft_pipapo_field *f;
	const u8 *start_p, *end_p;
	int i, bsize_max, err = 0;

	if (!m)
		return -ENOMEM;

	if (nft_set_ext_exists(ext, NFT_SET_EXT_KEY_END))
		end = (const u8 *)nft_set_ext_key_end(ext)->data;
	else
		end = start;

	dup = pipapo_get(m, start, genmask, tstamp);
	if (dup) {
		/* Check if we already have the same exact entry */
		const struct nft_data *dup_key, *dup_end;

		dup_key = nft_set_ext_key(&dup->ext);
		if (nft_set_ext_exists(&dup->ext, NFT_SET_EXT_KEY_END))
			dup_end = nft_set_ext_key_end(&dup->ext);
		else
			dup_end = dup_key;

		if (!memcmp(start, dup_key->data, sizeof(*dup_key->data)) &&
		    !memcmp(end, dup_end->data, sizeof(*dup_end->data))) {
			*elem_priv = &dup->priv;
			return -EEXIST;
		}

		return -ENOTEMPTY;
	}

	/* Look for partially overlapping entries */
	dup = pipapo_get(m, end, nft_genmask_next(net), tstamp);
	if (dup) {
		*elem_priv = &dup->priv;
		return -ENOTEMPTY;
	}

	/* Validate */
	start_p = start;
	end_p = end;

	/* some helpers return -1, or 0 >= for valid rule pos,
	 * so we cannot support more than INT_MAX rules at this time.
	 */
	BUILD_BUG_ON(NFT_PIPAPO_RULE0_MAX > INT_MAX);

	nft_pipapo_for_each_field(f, i, m) {
		if (f->rules >= NFT_PIPAPO_RULE0_MAX)
			return -ENOSPC;

		if (memcmp(start_p, end_p,
			   f->groups / NFT_PIPAPO_GROUPS_PER_BYTE(f)) > 0)
			return -EINVAL;

		start_p += NFT_PIPAPO_GROUPS_PADDED_SIZE(f);
		end_p += NFT_PIPAPO_GROUPS_PADDED_SIZE(f);
	}

	/* Insert */
	bsize_max = m->bsize_max;

	nft_pipapo_for_each_field(f, i, m) {
		int ret;

		rulemap[i].to = f->rules;

		ret = memcmp(start, end,
			     f->groups / NFT_PIPAPO_GROUPS_PER_BYTE(f));
		if (!ret)
			ret = pipapo_insert(f, start, f->groups * f->bb);
		else
			ret = pipapo_expand(f, start, end, f->groups * f->bb);

		if (ret < 0)
			return ret;

		if (f->bsize > bsize_max)
			bsize_max = f->bsize;

		rulemap[i].n = ret;

		start += NFT_PIPAPO_GROUPS_PADDED_SIZE(f);
		end += NFT_PIPAPO_GROUPS_PADDED_SIZE(f);
	}

	if (!*get_cpu_ptr(m->scratch) || bsize_max > m->bsize_max) {
		put_cpu_ptr(m->scratch);

		err = pipapo_realloc_scratch(m, bsize_max);
		if (err)
			return err;

		m->bsize_max = bsize_max;
	} else {
		put_cpu_ptr(m->scratch);
	}

	e = nft_elem_priv_cast(elem->priv);
	*elem_priv = &e->priv;

	pipapo_map(m, rulemap, e);

	return 0;
}

/**
 * pipapo_clone() - Clone matching data to create new working copy
 * @old:	Existing matching data
 *
 * Return: copy of matching data passed as 'old' or NULL.
 */
static struct nft_pipapo_match *pipapo_clone(struct nft_pipapo_match *old)
{
	struct nft_pipapo_field *dst, *src;
	struct nft_pipapo_match *new;
	int i;

	new = kmalloc(struct_size(new, f, old->field_count), GFP_KERNEL_ACCOUNT);
	if (!new)
		return NULL;

	new->field_count = old->field_count;
	new->bsize_max = old->bsize_max;

	new->scratch = alloc_percpu(*new->scratch);
	if (!new->scratch)
		goto out_scratch;

	for_each_possible_cpu(i)
		*per_cpu_ptr(new->scratch, i) = NULL;

	if (pipapo_realloc_scratch(new, old->bsize_max))
		goto out_scratch_realloc;

	rcu_head_init(&new->rcu);

	src = old->f;
	dst = new->f;

	for (i = 0; i < old->field_count; i++) {
		unsigned long *new_lt;
		ssize_t lt_size;

		memcpy(dst, src, offsetof(struct nft_pipapo_field, lt));

		lt_size = lt_calculate_size(src->groups, src->bb, src->bsize);
		if (lt_size < 0)
			goto out_lt;

		new_lt = kvzalloc(lt_size, GFP_KERNEL_ACCOUNT);
		if (!new_lt)
			goto out_lt;

		dst->lt = new_lt;

		memcpy(NFT_PIPAPO_LT_ALIGN(new_lt),
		       NFT_PIPAPO_LT_ALIGN(src->lt),
		       src->bsize * sizeof(*dst->lt) *
		       src->groups * NFT_PIPAPO_BUCKETS(src->bb));

		if (src->rules > 0) {
			if (src->rules_alloc > (INT_MAX / sizeof(*src->mt)))
				goto out_mt;

			dst->mt = kvmalloc_array(src->rules_alloc,
						 sizeof(*src->mt),
						 GFP_KERNEL_ACCOUNT);
			if (!dst->mt)
				goto out_mt;

			memcpy(dst->mt, src->mt, src->rules * sizeof(*src->mt));
		} else {
			dst->mt = NULL;
			dst->rules_alloc = 0;
		}

		src++;
		dst++;
	}

	return new;

out_mt:
	kvfree(dst->lt);
out_lt:
	for (dst--; i > 0; i--) {
		kvfree(dst->mt);
		kvfree(dst->lt);
		dst--;
	}
out_scratch_realloc:
	for_each_possible_cpu(i)
		pipapo_free_scratch(new, i);
out_scratch:
	free_percpu(new->scratch);
	kfree(new);

	return NULL;
}

/**
 * pipapo_rules_same_key() - Get number of rules originated from the same entry
 * @f:		Field containing mapping table
 * @first:	Index of first rule in set of rules mapping to same entry
 *
 * Using the fact that all rules in a field that originated from the same entry
 * will map to the same set of rules in the next field, or to the same element
 * reference, return the cardinality of the set of rules that originated from
 * the same entry as the rule with index @first, @first rule included.
 *
 * In pictures:
 *				rules
 *	field #0		0    1    2    3    4
 *		map to:		0    1   2-4  2-4  5-9
 *				.    .    .......   . ...
 *				|    |    |    | \   \
 *				|    |    |    |  \   \
 *				|    |    |    |   \   \
 *				'    '    '    '    '   \
 *	in field #1		0    1    2    3    4    5 ...
 *
 * if this is called for rule 2 on field #0, it will return 3, as also rules 2
 * and 3 in field 0 map to the same set of rules (2, 3, 4) in the next field.
 *
 * For the last field in a set, we can rely on associated entries to map to the
 * same element references.
 *
 * Return: Number of rules that originated from the same entry as @first.
 */
static unsigned int pipapo_rules_same_key(struct nft_pipapo_field *f, unsigned int first)
{
	struct nft_pipapo_elem *e = NULL; /* Keep gcc happy */
	unsigned int r;

	for (r = first; r < f->rules; r++) {
		if (r != first && e != f->mt[r].e)
			return r - first;

		e = f->mt[r].e;
	}

	if (r != first)
		return r - first;

	return 0;
}

/**
 * pipapo_unmap() - Remove rules from mapping tables, renumber remaining ones
 * @mt:		Mapping array
 * @rules:	Original amount of rules in mapping table
 * @start:	First rule index to be removed
 * @n:		Amount of rules to be removed
 * @to_offset:	First rule index, in next field, this group of rules maps to
 * @is_last:	If this is the last field, delete reference from mapping array
 *
 * This is used to unmap rules from the mapping table for a single field,
 * maintaining consistency and compactness for the existing ones.
 *
 * In pictures: let's assume that we want to delete rules 2 and 3 from the
 * following mapping array:
 *
 *                 rules
 *               0      1      2      3      4
 *      map to:  4-10   4-10   11-15  11-15  16-18
 *
 * the result will be:
 *
 *                 rules
 *               0      1      2
 *      map to:  4-10   4-10   11-13
 *
 * for fields before the last one. In case this is the mapping table for the
 * last field in a set, and rules map to pointers to &struct nft_pipapo_elem:
 *
 *                      rules
 *                        0      1      2      3      4
 *  element pointers:  0x42   0x42   0x33   0x33   0x44
 *
 * the result will be:
 *
 *                      rules
 *                        0      1      2
 *  element pointers:  0x42   0x42   0x44
 */
static void pipapo_unmap(union nft_pipapo_map_bucket *mt, unsigned int rules,
			 unsigned int start, unsigned int n,
			 unsigned int to_offset, bool is_last)
{
	int i;

	memmove(mt + start, mt + start + n, (rules - start - n) * sizeof(*mt));
	memset(mt + rules - n, 0, n * sizeof(*mt));

	if (is_last)
		return;

	for (i = start; i < rules - n; i++)
		mt[i].to -= to_offset;
}

/**
 * pipapo_drop() - Delete entry from lookup and mapping tables, given rule map
 * @m:		Matching data
 * @rulemap:	Table of rule maps, arrays of first rule and amount of rules
 *		in next field a given entry maps to, for each field
 *
 * For each rule in lookup table buckets mapping to this set of rules, drop
 * all bits set in lookup table mapping. In pictures, assuming we want to drop
 * rules 0 and 1 from this lookup table:
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0    0                                              1,2
 *        1   1,2                                      0
 *        2    0                                      1,2
 *        3    0                              1,2
 *        4  0,1,2
 *        5    0   1   2
 *        6  0,1,2 1   1   1   1   1   1   1   1   1   1   1   1   1   1   1
 *        7   1,2 1,2  1   1   1  0,1  1   1   1   1   1   1   1   1   1   1
 *
 * rule 2 becomes rule 0, and the result will be:
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0                                                    0
 *        1    0
 *        2                                            0
 *        3                                    0
 *        4    0
 *        5            0
 *        6    0
 *        7    0   0
 *
 * once this is done, call unmap() to drop all the corresponding rule references
 * from mapping tables.
 */
static void pipapo_drop(struct nft_pipapo_match *m,
			union nft_pipapo_map_bucket rulemap[])
{
	struct nft_pipapo_field *f;
	int i;

	nft_pipapo_for_each_field(f, i, m) {
		int g;

		for (g = 0; g < f->groups; g++) {
			unsigned long *pos;
			int b;

			pos = NFT_PIPAPO_LT_ALIGN(f->lt) + g *
			      NFT_PIPAPO_BUCKETS(f->bb) * f->bsize;

			for (b = 0; b < NFT_PIPAPO_BUCKETS(f->bb); b++) {
				bitmap_cut(pos, pos, rulemap[i].to,
					   rulemap[i].n,
					   f->bsize * BITS_PER_LONG);

				pos += f->bsize;
			}
		}

		pipapo_unmap(f->mt, f->rules, rulemap[i].to, rulemap[i].n,
			     rulemap[i + 1].n, i == m->field_count - 1);
		if (pipapo_resize(f, f->rules, f->rules - rulemap[i].n)) {
			/* We can ignore this, a failure to shrink tables down
			 * doesn't make tables invalid.
			 */
			;
		}
		f->rules -= rulemap[i].n;

		pipapo_lt_bits_adjust(f);
	}
}

static void nft_pipapo_gc_deactivate(struct net *net, struct nft_set *set,
				     struct nft_pipapo_elem *e)

{
	nft_setelem_data_deactivate(net, set, &e->priv);
}

/**
 * pipapo_gc() - Drop expired entries from set, destroy start and end elements
 * @set:	nftables API set representation
 * @m:		Matching data
 */
static void pipapo_gc(struct nft_set *set, struct nft_pipapo_match *m)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	struct net *net = read_pnet(&set->net);
	unsigned int rules_f0, first_rule = 0;
	u64 tstamp = nft_net_tstamp(net);
	struct nft_pipapo_elem *e;
	struct nft_trans_gc *gc;

	gc = nft_trans_gc_alloc(set, 0, GFP_KERNEL);
	if (!gc)
		return;

	while ((rules_f0 = pipapo_rules_same_key(m->f, first_rule))) {
		union nft_pipapo_map_bucket rulemap[NFT_PIPAPO_MAX_FIELDS];
		const struct nft_pipapo_field *f;
		unsigned int i, start, rules_fx;

		start = first_rule;
		rules_fx = rules_f0;

		nft_pipapo_for_each_field(f, i, m) {
			rulemap[i].to = start;
			rulemap[i].n = rules_fx;

			if (i < m->field_count - 1) {
				rules_fx = f->mt[start].n;
				start = f->mt[start].to;
			}
		}

		/* Pick the last field, and its last index */
		f--;
		i--;
		e = f->mt[rulemap[i].to].e;

		/* synchronous gc never fails, there is no need to set on
		 * NFT_SET_ELEM_DEAD_BIT.
		 */
		if (__nft_set_elem_expired(&e->ext, tstamp)) {
			gc = nft_trans_gc_queue_sync(gc, GFP_KERNEL);
			if (!gc)
				return;

			nft_pipapo_gc_deactivate(net, set, e);
			pipapo_drop(m, rulemap);
			nft_trans_gc_elem_add(gc, e);

			/* And check again current first rule, which is now the
			 * first we haven't checked.
			 */
		} else {
			first_rule += rules_f0;
		}
	}

	gc = nft_trans_gc_catchall_sync(gc);
	if (gc) {
		nft_trans_gc_queue_sync_done(gc);
		priv->last_gc = jiffies;
	}
}

/**
 * pipapo_free_fields() - Free per-field tables contained in matching data
 * @m:		Matching data
 */
static void pipapo_free_fields(struct nft_pipapo_match *m)
{
	struct nft_pipapo_field *f;
	int i;

	nft_pipapo_for_each_field(f, i, m) {
		kvfree(f->lt);
		kvfree(f->mt);
	}
}

static void pipapo_free_match(struct nft_pipapo_match *m)
{
	int i;

	for_each_possible_cpu(i)
		pipapo_free_scratch(m, i);

	free_percpu(m->scratch);
	pipapo_free_fields(m);

	kfree(m);
}

/**
 * pipapo_reclaim_match - RCU callback to free fields from old matching data
 * @rcu:	RCU head
 */
static void pipapo_reclaim_match(struct rcu_head *rcu)
{
	struct nft_pipapo_match *m;

	m = container_of(rcu, struct nft_pipapo_match, rcu);
	pipapo_free_match(m);
}

/**
 * nft_pipapo_commit() - Replace lookup data with current working copy
 * @set:	nftables API set representation
 *
 * While at it, check if we should perform garbage collection on the working
 * copy before committing it for lookup, and don't replace the table if the
 * working copy doesn't have pending changes.
 *
 * We also need to create a new working copy for subsequent insertions and
 * deletions.
 */
static void nft_pipapo_commit(struct nft_set *set)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	struct nft_pipapo_match *old;

	if (!priv->clone)
		return;

	if (time_after_eq(jiffies, priv->last_gc + nft_set_gc_interval(set)))
		pipapo_gc(set, priv->clone);

	old = rcu_replace_pointer(priv->match, priv->clone,
				  nft_pipapo_transaction_mutex_held(set));
	priv->clone = NULL;

	if (old)
		call_rcu(&old->rcu, pipapo_reclaim_match);
}

static void nft_pipapo_abort(const struct nft_set *set)
{
	struct nft_pipapo *priv = nft_set_priv(set);

	if (!priv->clone)
		return;
	pipapo_free_match(priv->clone);
	priv->clone = NULL;
}

/**
 * nft_pipapo_activate() - Mark element reference as active given key, commit
 * @net:	Network namespace
 * @set:	nftables API set representation
 * @elem_priv:	nftables API element representation containing key data
 *
 * On insertion, elements are added to a copy of the matching data currently
 * in use for lookups, and not directly inserted into current lookup data. Both
 * nft_pipapo_insert() and nft_pipapo_activate() are called once for each
 * element, hence we can't purpose either one as a real commit operation.
 */
static void nft_pipapo_activate(const struct net *net,
				const struct nft_set *set,
				struct nft_elem_priv *elem_priv)
{
	struct nft_pipapo_elem *e = nft_elem_priv_cast(elem_priv);

	nft_clear(net, &e->ext);
}

/**
 * nft_pipapo_deactivate() - Search for element and make it inactive
 * @net:	Network namespace
 * @set:	nftables API set representation
 * @elem:	nftables API element representation containing key data
 *
 * Return: deactivated element if found, NULL otherwise.
 */
static struct nft_elem_priv *
nft_pipapo_deactivate(const struct net *net, const struct nft_set *set,
		      const struct nft_set_elem *elem)
{
	struct nft_pipapo_match *m = pipapo_maybe_clone(set);
	struct nft_pipapo_elem *e;

	/* removal must occur on priv->clone, if we are low on memory
	 * we have no choice and must fail the removal request.
	 */
	if (!m)
		return NULL;

	e = pipapo_get(m, (const u8 *)elem->key.val.data,
		       nft_genmask_next(net), nft_net_tstamp(net));
	if (!e)
		return NULL;

	nft_set_elem_change_active(net, set, &e->ext);

	return &e->priv;
}

/**
 * nft_pipapo_flush() - make element inactive
 * @net:	Network namespace
 * @set:	nftables API set representation
 * @elem_priv:	nftables API element representation containing key data
 *
 * This is functionally the same as nft_pipapo_deactivate(), with a slightly
 * different interface, and it's also called once for each element in a set
 * being flushed, so we can't implement, strictly speaking, a flush operation,
 * which would otherwise be as simple as allocating an empty copy of the
 * matching data.
 *
 * Note that we could in theory do that, mark the set as flushed, and ignore
 * subsequent calls, but we would leak all the elements after the first one,
 * because they wouldn't then be freed as result of API calls.
 *
 * Return: true if element was found and deactivated.
 */
static void nft_pipapo_flush(const struct net *net, const struct nft_set *set,
			     struct nft_elem_priv *elem_priv)
{
	struct nft_pipapo_elem *e = nft_elem_priv_cast(elem_priv);

	nft_set_elem_change_active(net, set, &e->ext);
}

/**
 * pipapo_get_boundaries() - Get byte interval for associated rules
 * @f:		Field including lookup table
 * @first_rule:	First rule (lowest index)
 * @rule_count:	Number of associated rules
 * @left:	Byte expression for left boundary (start of range)
 * @right:	Byte expression for right boundary (end of range)
 *
 * Given the first rule and amount of rules that originated from the same entry,
 * build the original range associated with the entry, and calculate the length
 * of the originating netmask.
 *
 * In pictures:
 *
 *                     bucket
 *      group  0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
 *        0                                                   1,2
 *        1   1,2
 *        2                                           1,2
 *        3                                   1,2
 *        4   1,2
 *        5        1   2
 *        6   1,2  1   1   1   1   1   1   1   1   1   1   1   1   1   1   1
 *        7   1,2 1,2  1   1   1   1   1   1   1   1   1   1   1   1   1   1
 *
 * this is the lookup table corresponding to the IPv4 range
 * 192.168.1.0-192.168.2.1, which was expanded to the two composing netmasks,
 * rule #1: 192.168.1.0/24, and rule #2: 192.168.2.0/31.
 *
 * This function fills @left and @right with the byte values of the leftmost
 * and rightmost bucket indices for the lowest and highest rule indices,
 * respectively. If @first_rule is 1 and @rule_count is 2, we obtain, in
 * nibbles:
 *   left:  < 12, 0, 10, 8, 0, 1, 0, 0 >
 *   right: < 12, 0, 10, 8, 0, 2, 2, 1 >
 * corresponding to bytes:
 *   left:  < 192, 168, 1, 0 >
 *   right: < 192, 168, 2, 1 >
 * with mask length irrelevant here, unused on return, as the range is already
 * defined by its start and end points. The mask length is relevant for a single
 * ranged entry instead: if @first_rule is 1 and @rule_count is 1, we ignore
 * rule 2 above: @left becomes < 192, 168, 1, 0 >, @right becomes
 * < 192, 168, 1, 255 >, and the mask length, calculated from the distances
 * between leftmost and rightmost bucket indices for each group, would be 24.
 *
 * Return: mask length, in bits.
 */
static int pipapo_get_boundaries(struct nft_pipapo_field *f, int first_rule,
				 int rule_count, u8 *left, u8 *right)
{
	int g, mask_len = 0, bit_offset = 0;
	u8 *l = left, *r = right;

	for (g = 0; g < f->groups; g++) {
		int b, x0, x1;

		x0 = -1;
		x1 = -1;
		for (b = 0; b < NFT_PIPAPO_BUCKETS(f->bb); b++) {
			unsigned long *pos;

			pos = NFT_PIPAPO_LT_ALIGN(f->lt) +
			      (g * NFT_PIPAPO_BUCKETS(f->bb) + b) * f->bsize;
			if (test_bit(first_rule, pos) && x0 == -1)
				x0 = b;
			if (test_bit(first_rule + rule_count - 1, pos))
				x1 = b;
		}

		*l |= x0 << (BITS_PER_BYTE - f->bb - bit_offset);
		*r |= x1 << (BITS_PER_BYTE - f->bb - bit_offset);

		bit_offset += f->bb;
		if (bit_offset >= BITS_PER_BYTE) {
			bit_offset %= BITS_PER_BYTE;
			l++;
			r++;
		}

		if (x1 - x0 == 0)
			mask_len += 4;
		else if (x1 - x0 == 1)
			mask_len += 3;
		else if (x1 - x0 == 3)
			mask_len += 2;
		else if (x1 - x0 == 7)
			mask_len += 1;
	}

	return mask_len;
}

/**
 * pipapo_match_field() - Match rules against byte ranges
 * @f:		Field including the lookup table
 * @first_rule:	First of associated rules originating from same entry
 * @rule_count:	Amount of associated rules
 * @start:	Start of range to be matched
 * @end:	End of range to be matched
 *
 * Return: true on match, false otherwise.
 */
static bool pipapo_match_field(struct nft_pipapo_field *f,
			       int first_rule, int rule_count,
			       const u8 *start, const u8 *end)
{
	u8 right[NFT_PIPAPO_MAX_BYTES] = { 0 };
	u8 left[NFT_PIPAPO_MAX_BYTES] = { 0 };

	pipapo_get_boundaries(f, first_rule, rule_count, left, right);

	return !memcmp(start, left,
		       f->groups / NFT_PIPAPO_GROUPS_PER_BYTE(f)) &&
	       !memcmp(end, right, f->groups / NFT_PIPAPO_GROUPS_PER_BYTE(f));
}

/**
 * nft_pipapo_remove() - Remove element given key, commit
 * @net:	Network namespace
 * @set:	nftables API set representation
 * @elem_priv:	nftables API element representation containing key data
 *
 * Similarly to nft_pipapo_activate(), this is used as commit operation by the
 * API, but it's called once per element in the pending transaction, so we can't
 * implement this as a single commit operation. Closest we can get is to remove
 * the matched element here, if any, and commit the updated matching data.
 */
static void nft_pipapo_remove(const struct net *net, const struct nft_set *set,
			      struct nft_elem_priv *elem_priv)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	struct nft_pipapo_match *m = priv->clone;
	unsigned int rules_f0, first_rule = 0;
	struct nft_pipapo_elem *e;
	const u8 *data;

	e = nft_elem_priv_cast(elem_priv);
	data = (const u8 *)nft_set_ext_key(&e->ext);

	while ((rules_f0 = pipapo_rules_same_key(m->f, first_rule))) {
		union nft_pipapo_map_bucket rulemap[NFT_PIPAPO_MAX_FIELDS];
		const u8 *match_start, *match_end;
		struct nft_pipapo_field *f;
		int i, start, rules_fx;

		match_start = data;

		if (nft_set_ext_exists(&e->ext, NFT_SET_EXT_KEY_END))
			match_end = (const u8 *)nft_set_ext_key_end(&e->ext)->data;
		else
			match_end = data;

		start = first_rule;
		rules_fx = rules_f0;

		nft_pipapo_for_each_field(f, i, m) {
			bool last = i == m->field_count - 1;

			if (!pipapo_match_field(f, start, rules_fx,
						match_start, match_end))
				break;

			rulemap[i].to = start;
			rulemap[i].n = rules_fx;

			rules_fx = f->mt[start].n;
			start = f->mt[start].to;

			match_start += NFT_PIPAPO_GROUPS_PADDED_SIZE(f);
			match_end += NFT_PIPAPO_GROUPS_PADDED_SIZE(f);

			if (last && f->mt[rulemap[i].to].e == e) {
				pipapo_drop(m, rulemap);
				return;
			}
		}

		first_rule += rules_f0;
	}

	WARN_ON_ONCE(1); /* elem_priv not found */
}

/**
 * nft_pipapo_do_walk() - Walk over elements in m
 * @ctx:	nftables API context
 * @set:	nftables API set representation
 * @m:		matching data pointing to key mapping array
 * @iter:	Iterator
 *
 * As elements are referenced in the mapping array for the last field, directly
 * scan that array: there's no need to follow rule mappings from the first
 * field. @m is protected either by RCU read lock or by transaction mutex.
 */
static void nft_pipapo_do_walk(const struct nft_ctx *ctx, struct nft_set *set,
			       const struct nft_pipapo_match *m,
			       struct nft_set_iter *iter)
{
	const struct nft_pipapo_field *f;
	unsigned int i, r;

	for (i = 0, f = m->f; i < m->field_count - 1; i++, f++)
		;

	for (r = 0; r < f->rules; r++) {
		struct nft_pipapo_elem *e;

		if (r < f->rules - 1 && f->mt[r + 1].e == f->mt[r].e)
			continue;

		if (iter->count < iter->skip)
			goto cont;

		e = f->mt[r].e;

		iter->err = iter->fn(ctx, set, iter, &e->priv);
		if (iter->err < 0)
			return;

cont:
		iter->count++;
	}
}

/**
 * nft_pipapo_walk() - Walk over elements
 * @ctx:	nftables API context
 * @set:	nftables API set representation
 * @iter:	Iterator
 *
 * Test if destructive action is needed or not, clone active backend if needed
 * and call the real function to work on the data.
 */
static void nft_pipapo_walk(const struct nft_ctx *ctx, struct nft_set *set,
			    struct nft_set_iter *iter)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	const struct nft_pipapo_match *m;

	switch (iter->type) {
	case NFT_ITER_UPDATE:
		m = pipapo_maybe_clone(set);
		if (!m) {
			iter->err = -ENOMEM;
			return;
		}

		nft_pipapo_do_walk(ctx, set, m, iter);
		break;
	case NFT_ITER_READ:
		rcu_read_lock();
		m = rcu_dereference(priv->match);
		nft_pipapo_do_walk(ctx, set, m, iter);
		rcu_read_unlock();
		break;
	default:
		iter->err = -EINVAL;
		WARN_ON_ONCE(1);
		break;
	}
}

/**
 * nft_pipapo_privsize() - Return the size of private data for the set
 * @nla:	netlink attributes, ignored as size doesn't depend on them
 * @desc:	Set description, ignored as size doesn't depend on it
 *
 * Return: size of private data for this set implementation, in bytes
 */
static u64 nft_pipapo_privsize(const struct nlattr * const nla[],
			       const struct nft_set_desc *desc)
{
	return sizeof(struct nft_pipapo);
}

/**
 * nft_pipapo_estimate() - Set size, space and lookup complexity
 * @desc:	Set description, element count and field description used
 * @features:	Flags: NFT_SET_INTERVAL needs to be there
 * @est:	Storage for estimation data
 *
 * Return: true if set description is compatible, false otherwise
 */
static bool nft_pipapo_estimate(const struct nft_set_desc *desc, u32 features,
				struct nft_set_estimate *est)
{
	if (!(features & NFT_SET_INTERVAL) ||
	    desc->field_count < NFT_PIPAPO_MIN_FIELDS)
		return false;

	est->size = pipapo_estimate_size(desc);
	if (!est->size)
		return false;

	est->lookup = NFT_SET_CLASS_O_LOG_N;

	est->space = NFT_SET_CLASS_O_N;

	return true;
}

/**
 * nft_pipapo_init() - Initialise data for a set instance
 * @set:	nftables API set representation
 * @desc:	Set description
 * @nla:	netlink attributes
 *
 * Validate number and size of fields passed as NFTA_SET_DESC_CONCAT netlink
 * attributes, initialise internal set parameters, current instance of matching
 * data and a copy for subsequent insertions.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int nft_pipapo_init(const struct nft_set *set,
			   const struct nft_set_desc *desc,
			   const struct nlattr * const nla[])
{
	struct nft_pipapo *priv = nft_set_priv(set);
	struct nft_pipapo_match *m;
	struct nft_pipapo_field *f;
	int err, i, field_count;

	BUILD_BUG_ON(offsetof(struct nft_pipapo_elem, priv) != 0);

	field_count = desc->field_count ? : 1;

	BUILD_BUG_ON(NFT_PIPAPO_MAX_FIELDS > 255);
	BUILD_BUG_ON(NFT_PIPAPO_MAX_FIELDS != NFT_REG32_COUNT);

	if (field_count > NFT_PIPAPO_MAX_FIELDS)
		return -EINVAL;

	m = kmalloc(struct_size(m, f, field_count), GFP_KERNEL);
	if (!m)
		return -ENOMEM;

	m->field_count = field_count;
	m->bsize_max = 0;

	m->scratch = alloc_percpu(struct nft_pipapo_scratch *);
	if (!m->scratch) {
		err = -ENOMEM;
		goto out_scratch;
	}
	for_each_possible_cpu(i)
		*per_cpu_ptr(m->scratch, i) = NULL;

	rcu_head_init(&m->rcu);

	nft_pipapo_for_each_field(f, i, m) {
		unsigned int len = desc->field_len[i] ? : set->klen;

		/* f->groups is u8 */
		BUILD_BUG_ON((NFT_PIPAPO_MAX_BYTES *
			      BITS_PER_BYTE / NFT_PIPAPO_GROUP_BITS_LARGE_SET) >= 256);

		f->bb = NFT_PIPAPO_GROUP_BITS_INIT;
		f->groups = len * NFT_PIPAPO_GROUPS_PER_BYTE(f);

		priv->width += round_up(len, sizeof(u32));

		f->bsize = 0;
		f->rules = 0;
		f->rules_alloc = 0;
		f->lt = NULL;
		f->mt = NULL;
	}

	rcu_assign_pointer(priv->match, m);

	return 0;

out_scratch:
	kfree(m);

	return err;
}

/**
 * nft_set_pipapo_match_destroy() - Destroy elements from key mapping array
 * @ctx:	context
 * @set:	nftables API set representation
 * @m:		matching data pointing to key mapping array
 */
static void nft_set_pipapo_match_destroy(const struct nft_ctx *ctx,
					 const struct nft_set *set,
					 struct nft_pipapo_match *m)
{
	struct nft_pipapo_field *f;
	unsigned int i, r;

	for (i = 0, f = m->f; i < m->field_count - 1; i++, f++)
		;

	for (r = 0; r < f->rules; r++) {
		struct nft_pipapo_elem *e;

		if (r < f->rules - 1 && f->mt[r + 1].e == f->mt[r].e)
			continue;

		e = f->mt[r].e;

		nf_tables_set_elem_destroy(ctx, set, &e->priv);
	}
}

/**
 * nft_pipapo_destroy() - Free private data for set and all committed elements
 * @ctx:	context
 * @set:	nftables API set representation
 */
static void nft_pipapo_destroy(const struct nft_ctx *ctx,
			       const struct nft_set *set)
{
	struct nft_pipapo *priv = nft_set_priv(set);
	struct nft_pipapo_match *m;

	m = rcu_dereference_protected(priv->match, true);

	if (priv->clone) {
		nft_set_pipapo_match_destroy(ctx, set, priv->clone);
		pipapo_free_match(priv->clone);
		priv->clone = NULL;
	} else {
		nft_set_pipapo_match_destroy(ctx, set, m);
	}

	pipapo_free_match(m);
}

/**
 * nft_pipapo_gc_init() - Initialise garbage collection
 * @set:	nftables API set representation
 *
 * Instead of actually setting up a periodic work for garbage collection, as
 * this operation requires a swap of matching data with the working copy, we'll
 * do that opportunistically with other commit operations if the interval is
 * elapsed, so we just need to set the current jiffies timestamp here.
 */
static void nft_pipapo_gc_init(const struct nft_set *set)
{
	struct nft_pipapo *priv = nft_set_priv(set);

	priv->last_gc = jiffies;
}

const struct nft_set_type nft_set_pipapo_type = {
	.features	= NFT_SET_INTERVAL | NFT_SET_MAP | NFT_SET_OBJECT |
			  NFT_SET_TIMEOUT,
	.ops		= {
		.lookup		= nft_pipapo_lookup,
		.insert		= nft_pipapo_insert,
		.activate	= nft_pipapo_activate,
		.deactivate	= nft_pipapo_deactivate,
		.flush		= nft_pipapo_flush,
		.remove		= nft_pipapo_remove,
		.walk		= nft_pipapo_walk,
		.get		= nft_pipapo_get,
		.privsize	= nft_pipapo_privsize,
		.estimate	= nft_pipapo_estimate,
		.init		= nft_pipapo_init,
		.destroy	= nft_pipapo_destroy,
		.gc_init	= nft_pipapo_gc_init,
		.commit		= nft_pipapo_commit,
		.abort		= nft_pipapo_abort,
		.elemsize	= offsetof(struct nft_pipapo_elem, ext),
	},
};

#if defined(CONFIG_X86_64) && !defined(CONFIG_UML)
const struct nft_set_type nft_set_pipapo_avx2_type = {
	.features	= NFT_SET_INTERVAL | NFT_SET_MAP | NFT_SET_OBJECT |
			  NFT_SET_TIMEOUT,
	.ops		= {
		.lookup		= nft_pipapo_avx2_lookup,
		.insert		= nft_pipapo_insert,
		.activate	= nft_pipapo_activate,
		.deactivate	= nft_pipapo_deactivate,
		.flush		= nft_pipapo_flush,
		.remove		= nft_pipapo_remove,
		.walk		= nft_pipapo_walk,
		.get		= nft_pipapo_get,
		.privsize	= nft_pipapo_privsize,
		.estimate	= nft_pipapo_avx2_estimate,
		.init		= nft_pipapo_init,
		.destroy	= nft_pipapo_destroy,
		.gc_init	= nft_pipapo_gc_init,
		.commit		= nft_pipapo_commit,
		.abort		= nft_pipapo_abort,
		.elemsize	= offsetof(struct nft_pipapo_elem, ext),
	},
};
#endif
