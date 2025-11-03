#ifndef RB_KMERGE_H
#define RB_KMERGE_H

#include "postgres.h"

#include "funcapi.h"
#include "utils/array.h"

/*
 * Opaque state for the roaring_kmerge set-returning function.
 * The definition lives in roaring_kmerge.c; callers treat it as a black box.
 */
typedef struct RoaringKMergeState RoaringKMergeState;

/*
 * Build iterators from the input bitmap array, run the k-way merge, and
 * return an initialised RoaringKMergeState ready for repeated
 * roaring_kmerge_next_row() calls. Must be called inside SRF_IS_FIRSTCALL(),
 * with funcctx already initialised.
 */
RoaringKMergeState *roaring_kmerge_build_state(ArrayType *arr,
                                               FuncCallContext *funcctx,
                                               FunctionCallInfo fcinfo);

/*
 * Fetch the next (sources int[], members roaringbitmap) row from state.
 * Returns a HeapTuple, or NULL when exhausted.
 */
HeapTuple roaring_kmerge_next_row(RoaringKMergeState *state);

#endif /* RB_KMERGE_H */
