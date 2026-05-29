/**
 * Re-includable template header that emits one simplehash specialisation
 * for rb_group_elements_by_source, parameterised on:
 *
 *   RB_GROUP_BY_SOURCE_HASH_PREFIX         – the SH_PREFIX
 *   RB_GROUP_BY_SOURCE_HASH_MEMBERS_TYPE   – type of the members-bitmap
 *                                            pointer field (e.g.
 *                                            roaring_bitmap_t * or
 *                                            roaring64_bitmap_t *)
 *   RB_GROUP_BY_SOURCE_HASH_BULK_CTX_TYPE  – type of the bulk-add context
 *                                            field
 *
 * Each consuming translation unit predefines these three parameters and
 * then #includes this header.  The header emits the entry struct, sets
 * up the SH_* macros, includes lib/simplehash.h, and #undefs all three
 * parameter macros so the file is re-includable (e.g. if a single TU
 * ever needed two specialisations).
 *
 * Intentionally no include guard: this header is meant to be re-included
 * with different parameters.  Consumers are expected to invoke it at
 * most once per (prefix, types) tuple per TU.
 *
 * See roaring_group_by_source_common.h for the heap, hash, and helper
 * functions shared between specialisations.
 */

#ifndef RB_GROUP_BY_SOURCE_HASH_PREFIX
#error "roaring_group_by_source_hash_template.h: RB_GROUP_BY_SOURCE_HASH_PREFIX must be defined before include"
#endif
#ifndef RB_GROUP_BY_SOURCE_HASH_MEMBERS_TYPE
#error "roaring_group_by_source_hash_template.h: RB_GROUP_BY_SOURCE_HASH_MEMBERS_TYPE must be defined before include"
#endif
#ifndef RB_GROUP_BY_SOURCE_HASH_BULK_CTX_TYPE
#error "roaring_group_by_source_hash_template.h: RB_GROUP_BY_SOURCE_HASH_BULK_CTX_TYPE must be defined before include"
#endif

/* Helpers to construct the entry-type identifier as <prefix>_entry_t. */
#define RB_GROUP_BY_SOURCE_HASH_PASTE_(a, b) a##b
#define RB_GROUP_BY_SOURCE_HASH_PASTE(a, b) RB_GROUP_BY_SOURCE_HASH_PASTE_(a, b)
#define RB_GROUP_BY_SOURCE_HASH_ENTRY_T                                        \
    RB_GROUP_BY_SOURCE_HASH_PASTE(RB_GROUP_BY_SOURCE_HASH_PREFIX, _entry_t)
#define RB_GROUP_BY_SOURCE_HASH_ENTRY_S                                        \
    RB_GROUP_BY_SOURCE_HASH_PASTE(RB_GROUP_BY_SOURCE_HASH_PREFIX, _entry_s)

typedef struct RB_GROUP_BY_SOURCE_HASH_ENTRY_S {
    uint64_t *key; /* palloc'd bitmask of input bitmap indexes */
    RB_GROUP_BY_SOURCE_HASH_MEMBERS_TYPE members;
    RB_GROUP_BY_SOURCE_HASH_BULK_CTX_TYPE bulk_ctx;
    char status; /* required by simplehash */
} RB_GROUP_BY_SOURCE_HASH_ENTRY_T;

#define SH_PREFIX RB_GROUP_BY_SOURCE_HASH_PREFIX
#define SH_ELEMENT_TYPE RB_GROUP_BY_SOURCE_HASH_ENTRY_T
#define SH_KEY_TYPE uint64_t *
#define SH_KEY key
#define SH_HASH_KEY(tb, k)                                                     \
    roaring_group_by_source_hash_key(                                          \
        (k), ((roaring_group_by_source_group_private_t *)(tb)->private_data)   \
                 ->nwords)
#define SH_EQUAL(tb, a, b)                                                     \
    (memcmp((a), (b),                                                          \
            ((roaring_group_by_source_group_private_t *)(tb)->private_data)    \
                    ->nwords *                                                 \
                sizeof(uint64_t)) == 0)
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

#undef RB_GROUP_BY_SOURCE_HASH_PREFIX
#undef RB_GROUP_BY_SOURCE_HASH_MEMBERS_TYPE
#undef RB_GROUP_BY_SOURCE_HASH_BULK_CTX_TYPE
#undef RB_GROUP_BY_SOURCE_HASH_PASTE_
#undef RB_GROUP_BY_SOURCE_HASH_PASTE
#undef RB_GROUP_BY_SOURCE_HASH_ENTRY_T
#undef RB_GROUP_BY_SOURCE_HASH_ENTRY_S
