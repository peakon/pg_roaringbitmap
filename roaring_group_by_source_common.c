/**
 * Type-independent helpers shared between the 32-bit and 64-bit
 * rb_group_elements_by_source implementations.
 *
 * The hot-path heap, hash, and simplehash specialisation live in
 * roaring_group_by_source_common.h as static inline so each consuming
 * translation unit gets its own private copies.  This file only carries
 * out-of-line helpers.
 */

#include "roaring_group_by_source_common.h"

#include "catalog/pg_type.h"

ArrayType *
roaring_group_by_source_bitmask_to_sources_array(const uint64_t *key,
                                                 int nwords) {
    int max_sources = nwords * 64;
    Datum *src_buf = (Datum *)palloc(max_sources * sizeof(Datum));
    int nsources = 0;
    for (int w = 0; w < nwords; w++) {
        uint64_t v = key[w];
        int base = w * 64;
        while (v) {
            int bitpos = roaring_trailing_zeroes(v);
            src_buf[nsources++] = Int32GetDatum(base + bitpos + 1);
            v &= v - 1;
        }
    }
    ArrayType *src_array =
        construct_array(src_buf, nsources, INT4OID, sizeof(int32_t), true, 'i');
    pfree(src_buf);
    return src_array;
}
