#ifndef ROARING_GROUP_BY_SOURCE_COMMON_H
#define ROARING_GROUP_BY_SOURCE_COMMON_H

/**
 * Type-independent cold-path bits shared between the 32-bit and 64-bit
 * implementations of rb_group_elements_by_source.
 *
 * The simplehash specialisation is templated per consuming translation
 * unit via roaring_group_by_source_hash_template.h.  Each consumer
 * predefines three preprocessor parameters and then #includes the
 * template header, which emits the entry struct, configures the SH_*
 * macros, pulls in lib/simplehash.h, and #undefs its parameters so it
 * can be re-included.  Each consumer picks its own SH_PREFIX, so the
 * 32-bit and 64-bit variants do not collide; SH_SCOPE static inline
 * keeps the duplication cost negligible.
 *
 * Typical usage in a .c file:
 *
 *   #include "roaring_group_by_source_common.h"
 *
 *   #define RB_GROUP_BY_SOURCE_HASH_PREFIX        roaring_group_by_source_group
 *   #define RB_GROUP_BY_SOURCE_HASH_MEMBERS_TYPE  roaring_bitmap_t *
 *   #define RB_GROUP_BY_SOURCE_HASH_BULK_CTX_TYPE roaring_bulk_context_t
 *   #include "roaring_group_by_source_hash_template.h"
 */

#include "postgres.h"

#include <stdint.h>
#include <string.h>

#include "utils/array.h"

#include "roaring.h"

/**
 * Heap node used by the k-way merge.
 *
 * The 'value' field is uint64_t so the same node type can be used by both
 * the 32-bit and 64-bit merges. The 32-bit caller simply stores its
 * uint32_t current value into this field — implicit widening, no
 * behavioural change.
 */
typedef struct roaring_group_by_source_heap_node_s {
    int src;        // 0-based index of iterator into the input bitmap array
    uint64_t value; // iterator's current value
} roaring_group_by_source_heap_node_t;

static inline void roaring_group_by_source_heap_sift_down(
    roaring_group_by_source_heap_node_t *heap, int size, int idx) {
    for (;;) {
        int left = (idx << 1) + 1;
        if (left >= size)
            break;
        int right = left + 1;
        int smallest = left;
        if (right < size && heap[right].value < heap[left].value)
            smallest = right;
        if (!(heap[smallest].value < heap[idx].value))
            break;
        roaring_group_by_source_heap_node_t tmp = heap[idx];
        heap[idx] = heap[smallest];
        heap[smallest] = tmp;
        idx = smallest;
    }
}

static inline void
roaring_group_by_source_heap_build(roaring_group_by_source_heap_node_t *heap,
                                   int size) {
    for (int i = (size >> 1) - 1; i >= 0; i--)
        roaring_group_by_source_heap_sift_down(heap, size, i);
}

static inline uint32_t roaring_group_by_source_hash_key(const uint64_t *words,
                                                        int nwords) {
    uint64_t h = 0;
    for (int i = 0; i < nwords; i++) {
        h ^= words[i];
        h ^= h >> 30;
        h *= 0xbf58476d1ce4e5b9ULL;
        h ^= h >> 27;
        h *= 0x94d049bb133111ebULL;
        h ^= h >> 31;
    }
    return (uint32_t)h;
}

/**
 * Type-independent private_data carried on the simplehash table.  Both
 * the 32-bit and 64-bit specialisations share this struct since it only
 * carries nwords.
 */
typedef struct roaring_group_by_source_group_private_s {
    int nwords;
} roaring_group_by_source_group_private_t;

/**
 * Convert a packed bitmask into a Postgres ArrayType containing the 1-based
 * indices of set bits.  Used by both the 32-bit and 64-bit next_row
 * implementations to emit the 'sources int[]' column.
 */
ArrayType *
roaring_group_by_source_bitmask_to_sources_array(const uint64_t *key,
                                                 int nwords);

#endif
