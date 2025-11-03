#include "roaring_kmerge.h"

#include "roaringbitmap.h"
#include "utils/lsyscache.h"

/* ============================================================
 * roaring_kmerge implementation: k-way merge with source-set grouping
 * ============================================================
 *
 * Given N bitmaps, performs a k-way merge and groups elements by which
 * combination of input bitmaps contains them.  Returns one row per unique
 * source-set pattern with:
 *   sources int[]         – 1-based indices of input bitmaps
 *   members roaringbitmap – all elements sharing that source-set
 *
 * Uses PostgreSQL's lib/simplehash.h for the hash table and a hand-rolled
 * min-heap for the k-way merge.
 *
 * The source-set key is a variable-length uint64 bitmask (nwords words).
 * Where the i'th bit corresponds to the i'th input bitmap.
 * SH_KEY_TYPE is uint64 *, pointing to a palloc'd array per unique group.
 */

typedef struct RoaringKMergeHeapNode {
  int src;      /* 0-based index of iterator */
  uint32 value; /* iterator's current value */
} RoaringKMergeHeapNode;

static inline void roaring_kmerge_heap_sift_down(RoaringKMergeHeapNode *heap,
                                                 int size, int idx) {
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
    RoaringKMergeHeapNode tmp = heap[idx];
    heap[idx] = heap[smallest];
    heap[smallest] = tmp;
    idx = smallest;
  }
}

static inline void roaring_kmerge_heap_build(RoaringKMergeHeapNode *heap,
                                             int size) {
  for (int i = (size >> 1) - 1; i >= 0; i--)
    roaring_kmerge_heap_sift_down(heap, size, i);
}

static inline uint32 roaring_kmerge_hash_key(const uint64 *words, int nwords) {
  uint64 h = 0;
  for (int i = 0; i < nwords; i++) {
    h ^= words[i];
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
  }
  return (uint32)h;
}

typedef struct RoaringKMergeGroupPrivate {
  int nwords;
} RoaringKMergeGroupPrivate;

typedef struct RoaringKMergeGroupEntry {
  uint64 *key; /* palloc'd array of nwords uint64s */
  roaring_bitmap_t *members;
  roaring_bulk_context_t bulk_ctx;
  char status; /* required by simplehash */
} RoaringKMergeGroupEntry;

#define SH_PREFIX roaring_kmerge_group
#define SH_ELEMENT_TYPE RoaringKMergeGroupEntry
#define SH_KEY_TYPE uint64 *
#define SH_KEY key
#define SH_HASH_KEY(tb, k)                                                     \
  roaring_kmerge_hash_key(                                                     \
      (k), ((RoaringKMergeGroupPrivate *)(tb)->private_data)->nwords)
#define SH_EQUAL(tb, a, b)                                                     \
  (memcmp((a), (b),                                                            \
          ((RoaringKMergeGroupPrivate *)(tb)->private_data)->nwords *          \
              sizeof(uint64)) == 0)
#define SH_SCOPE static inline
#define SH_DECLARE
#define SH_DEFINE
#include "lib/simplehash.h"

/* ---- SRF state ---- */

struct RoaringKMergeState {
  roaring_kmerge_group_hash *ht;
  roaring_kmerge_group_iterator iter;
  int nwords;
  TupleDesc tupdesc;
};

/**
 * roaring_kmerge_build_iterators:
 * deserialise bitmaps and seed the min-heap.
 */
static void roaring_kmerge_build_iterators(ArrayType *arr, int nelems,
                                           const Datum *elem_values,
                                           const bool *elem_nulls,
                                           roaring_uint32_iterator_t **iters,
                                           RoaringKMergeHeapNode *heap,
                                           int *heap_size_out) {
  int heap_size = 0;

  for (int i = 0; i < nelems; i++) {
    if (elem_nulls[i])
      continue;

    bytea *data = (bytea *)DatumGetPointer(elem_values[i]);
    roaring_bitmap_t *rb = roaring_bitmap_portable_deserialize_safe(
        VARDATA(data), VARSIZE(data) - VARHDRSZ);
    if (!rb)
      ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
                      errmsg("bitmap format is error")));

    roaring_uint32_iterator_t *it = roaring_iterator_create(rb);
    iters[i] = it;
    if (it->has_value) {
      heap[heap_size].src = i;
      heap[heap_size].value = it->current_value;
      heap_size++;
    }
  }

  if (heap_size > 1)
    roaring_kmerge_heap_build(heap, heap_size);

  *heap_size_out = heap_size;
}

/**
 * roaring_kmerge_run_merge:
 * k-way merge loop, grouping by source-set into the hash table.
 */
static void roaring_kmerge_run_merge(roaring_uint32_iterator_t **iters,
                                     RoaringKMergeHeapNode *heap, int heap_size,
                                     roaring_kmerge_group_hash *ht,
                                     int nwords) {
  /* Reusable scratch buffer for the current element's source-set bitmask */
  uint64 *bitmask = (uint64 *)palloc0(nwords * sizeof(uint64));

  while (heap_size > 0) {
    uint32 current_val = heap[0].value;
    memset(bitmask, 0, nwords * sizeof(uint64));

    /* Collect all sources that contain current_val */
    do {
      int src = heap[0].src;
      bitmask[src / 64] |= ((uint64)1) << (src % 64);

      roaring_uint32_iterator_t *it = iters[src];
      roaring_uint32_iterator_advance(it);
      if (it->has_value) {
        heap[0].value = it->current_value;
        roaring_kmerge_heap_sift_down(heap, heap_size, 0);
      } else {
        heap[0] = heap[heap_size - 1];
        heap_size--;
        if (heap_size > 0)
          roaring_kmerge_heap_sift_down(heap, heap_size, 0);
      }
    } while (heap_size > 0 && heap[0].value == current_val);

    /* Insert into the group for this source-set */
    bool found;
    RoaringKMergeGroupEntry *entry =
        roaring_kmerge_group_insert(ht, bitmask, &found);
    if (!found) {
      /* New group: copy the scratch bitmask into a persistent allocation */
      uint64 *key_copy = (uint64 *)palloc(nwords * sizeof(uint64));
      memcpy(key_copy, bitmask, nwords * sizeof(uint64));
      entry->key = key_copy;
      entry->members = roaring_bitmap_create();
      memset(&entry->bulk_ctx, 0, sizeof(roaring_bulk_context_t));
    }
    roaring_bitmap_add_bulk(entry->members, &entry->bulk_ctx, current_val);
  }

  pfree(bitmask);
}

RoaringKMergeState *roaring_kmerge_build_state(ArrayType *arr,
                                               FuncCallContext *funcctx,
                                               FunctionCallInfo fcinfo) {
  int16 elmlen;
  bool elmbyval;
  char elmalign;
  Oid elmtype = ARR_ELEMTYPE(arr);
  get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

  Datum *elem_values;
  bool *elem_nulls;
  int nelems;
  deconstruct_array(arr, elmtype, elmlen, elmbyval, elmalign, &elem_values,
                    &elem_nulls, &nelems);

  int nwords = (nelems + 63) / 64;
  if (nwords < 1)
    nwords = 1;

  roaring_uint32_iterator_t **iters = (roaring_uint32_iterator_t **)palloc0(
      sizeof(roaring_uint32_iterator_t *) * Max(nelems, 1));
  RoaringKMergeHeapNode *heap = (RoaringKMergeHeapNode *)palloc(
      sizeof(RoaringKMergeHeapNode) * Max(nelems, 1));
  int heap_size;

  roaring_kmerge_build_iterators(arr, nelems, elem_values, elem_nulls, iters,
                                 heap, &heap_size);

  RoaringKMergeGroupPrivate *priv =
      (RoaringKMergeGroupPrivate *)palloc(sizeof(RoaringKMergeGroupPrivate));
  priv->nwords = nwords;

  roaring_kmerge_group_hash *ht =
      roaring_kmerge_group_create(funcctx->multi_call_memory_ctx, 256, priv);

  roaring_kmerge_run_merge(iters, heap, heap_size, ht, nwords);

  for (int i = 0; i < nelems; i++) {
    if (iters[i])
      roaring_uint32_iterator_free(iters[i]);
  }
  pfree(iters);
  pfree(heap);

  TupleDesc tupdesc;
  if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
    ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
                    errmsg("return type must be a row type")));
  BlessTupleDesc(tupdesc);

  RoaringKMergeState *state =
      (RoaringKMergeState *)palloc0(sizeof(RoaringKMergeState));
  state->ht = ht;
  state->nwords = nwords;
  state->tupdesc = tupdesc;
  roaring_kmerge_group_start_iterate(ht, &state->iter);

  return state;
}

HeapTuple roaring_kmerge_next_row(RoaringKMergeState *state) {
  RoaringKMergeGroupEntry *entry =
      roaring_kmerge_group_iterate(state->ht, &state->iter);
  if (entry == NULL)
    return NULL;

  /* Convert bitmask to int[] of 1-based source indices */
  int max_sources = state->nwords * 64;
  Datum *src_buf = (Datum *)palloc(max_sources * sizeof(Datum));
  int nsources = 0;
  for (int w = 0; w < state->nwords; w++) {
    uint64 v = entry->key[w];
    int base = w * 64;
    while (v) {
      int bitpos = __builtin_ctzll(v);
      src_buf[nsources++] = Int32GetDatum(base + bitpos + 1);
      v &= v - 1;
    }
  }
  ArrayType *src_array =
      construct_array(src_buf, nsources, INT4OID, sizeof(int32), true, 'i');
  pfree(src_buf);

  /* Serialize the members bitmap */
  size_t portable_size = roaring_bitmap_portable_size_in_bytes(entry->members);
  bytea *serialized = (bytea *)palloc(VARHDRSZ + portable_size);
  roaring_bitmap_portable_serialize(entry->members, VARDATA(serialized));
  SET_VARSIZE(serialized, VARHDRSZ + portable_size);

  Datum vals[2] = {PointerGetDatum(src_array), PointerGetDatum(serialized)};
  const bool nulls[2] = {false, false};

  return heap_form_tuple(state->tupdesc, vals, nulls);
}
