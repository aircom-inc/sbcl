/*
 * Coalescing of constant vectors for SAVE-LISP-AND-DIE
 */

/*
 * This software is part of the SBCL system. See the README file for
 * more information.
 *
 * This software is derived from the CMU CL system, which was
 * written at Carnegie Mellon University and released into the
 * public domain. The software is in the public domain and is
 * provided with absolutely no warranty. See the COPYING and CREDITS
 * files for more information.
 */

#include "sbcl.h"
#include "gc.h"
#include "gc-internal.h"
#include "gc-private.h"
#include "genesis/vector.h"
#include "genesis/gc-tables.h"
// FIXME: cheneygc needs layout.h but gencgc doesn't,
// which means it's leaking in from somewhere else. Yuck.
#include "genesis/layout.h"
#include "gc-internal.h"
#include "immobile-space.h"
#include "hopscotch.h"

static boolean gcable_pointer_p(lispobj pointer)
{
#ifdef LISP_FEATURE_CHENEYGC
   return pointer >= (lispobj)current_dynamic_space
       && pointer < (lispobj)dynamic_space_free_pointer;
#endif
#ifdef LISP_FEATURE_GENCGC
   return find_page_index((void*)pointer) >= 0 || immobile_space_p(pointer);
#endif
}

static boolean coalescible_number_p(lispobj* where)
{
    int widetag = widetag_of(*where);
    return widetag == BIGNUM_WIDETAG
        // Ratios and complex integers containing pointers to bignums don't work.
        || ((widetag == RATIO_WIDETAG || widetag == COMPLEX_WIDETAG)
            && fixnump(where[1]) && fixnump(where[2]))
#ifndef LISP_FEATURE_64_BIT
        || widetag == SINGLE_FLOAT_WIDETAG
#endif
        || widetag == DOUBLE_FLOAT_WIDETAG
        || widetag == COMPLEX_SINGLE_FLOAT_WIDETAG
        || widetag == COMPLEX_DOUBLE_FLOAT_WIDETAG;
}

/// Return true of fixnums, bignums, strings, symbols.
/// Strings are considered eql-comparable,
/// because they're coalesced before comparing.
static boolean eql_comparable_p(lispobj obj)
{
    if (fixnump(obj) || obj == NIL) return 1;
    if (lowtag_of(obj) != OTHER_POINTER_LOWTAG) return 0;
    int widetag = widetag_of(*native_pointer(obj));
    return widetag == BIGNUM_WIDETAG
        || widetag == SYMBOL_WIDETAG
#ifdef SIMPLE_CHARACTER_STRING_WIDETAG
        || widetag == SIMPLE_CHARACTER_STRING_WIDETAG
#endif
        || widetag == SIMPLE_BASE_STRING_WIDETAG;
}

static boolean vector_isevery(boolean (*pred)(lispobj), struct vector* v)
{
    int i;
    for (i = fixnum_value(v->length)-1; i >= 0; --i)
        if (!pred(v->data[i])) return 0;
    return 1;
}

static void coalesce_obj(lispobj* where, struct hopscotch_table* ht)
{
    lispobj ptr = *where;
    if (lowtag_of(ptr) != OTHER_POINTER_LOWTAG)
        return;

    extern char gc_coalesce_string_literals;
    // gc_coalesce_string_literals represents the "aggressiveness" level.
    // If 1, then we share vectors tagged as +VECTOR-SHAREABLE+,
    // but if >1, those and also +VECTOR-SHAREABLE-NONSTD+.
    int mask = gc_coalesce_string_literals > 1
      ? (VECTOR_SHAREABLE|VECTOR_SHAREABLE_NONSTD)<<N_WIDETAG_BITS
      : (VECTOR_SHAREABLE                        )<<N_WIDETAG_BITS;

    lispobj* obj = native_pointer(ptr);
    lispobj header = *obj;
    int widetag = widetag_of(header);

    if ((((header & mask) != 0) // optimistically assume it's a vector
         && ((widetag == SIMPLE_VECTOR_WIDETAG
              && vector_isevery(eql_comparable_p, (struct vector*)obj))
             || specialized_vector_widetag_p(widetag)))
        || coalescible_number_p(obj)) {
        if (widetag == SIMPLE_VECTOR_WIDETAG) {
            sword_t n_elts = fixnum_value(obj[1]), i;
            for (i = 2 ; i < n_elts+2 ; ++i)
                coalesce_obj(obj + i, ht);
        }
        int index = hopscotch_get(ht, (uword_t)obj, 0);
        if (!index) // Not found
            hopscotch_insert(ht, (uword_t)obj, 1);
        else {
            ptr = make_lispobj((void*)ht->keys[index-1],
                               OTHER_POINTER_LOWTAG);
            // Check for no read-only to dynamic-space pointer
            if ((uintptr_t)where >= READ_ONLY_SPACE_START &&
                (uintptr_t)where < READ_ONLY_SPACE_END &&
                gcable_pointer_p(ptr))
                lose("Coalesce produced RO->DS ptr");
            *where = ptr;
        }
    }
}

/* FIXME: there are nearly 10 variants of the skeleton of an object traverser.
 * Pick one and try to make it customizable. I tried a callback-based approach,
 * but it's too slow. Next best thing is a ".inc" file which defines the shape
 * of the function, with pieces inserted by #define.
 *
 * (1) gc-common's table-based mechanism
 * (2) gencgc's verify_range()
 * (3) immobile space {fixedobj,varyobj}_points_to_younger_p()
 *     and fixup_space() for defrag. [and the table-based thing is used too]
 * (4) fullcgc's trace_object()
 * (5) coreparse's relocate_space()
 * (6) traceroot's find_ref() and build_refs() which itself has two modes
 * (7) purify()
 * (8) this one - coalesce_range()
 * plus the Lisp variant:
 * (9) map-referencing-objects
 * and if you want to count 'print.c' as another, there's that.
 */

static uword_t coalesce_range(lispobj* where, lispobj* limit, uword_t arg)
{
    struct hopscotch_table* ht = (struct hopscotch_table*)arg;
    lispobj layout, bitmap, *next;
    sword_t nwords, i, j;

    for ( ; where < limit ; where = next ) {
        lispobj header = *where;
        if (is_cons_half(header)) {
            coalesce_obj(where+0, ht);
            coalesce_obj(where+1, ht);
            next = where + 2;
        } else {
            int widetag = widetag_of(header);
            nwords = sizetab[widetag](where);
            next = where + nwords;
            switch (widetag) {
            case INSTANCE_WIDETAG: // mixed boxed/unboxed objects
#ifdef LISP_FEATURE_COMPACT_INSTANCE_HEADER
            case FUNCALLABLE_INSTANCE_WIDETAG:
#endif
                layout = instance_layout(where);
                bitmap = LAYOUT(layout)->bitmap;
                for(i=1; i<nwords; ++i)
                    if (layout_bitmap_logbitp(i-1, bitmap))
                        coalesce_obj(where+i, ht);
                continue;
            case CODE_HEADER_WIDETAG:
                for_each_simple_fun(i, fun, (struct code*)where, 0, {
                    lispobj* fun_slots = SIMPLE_FUN_SCAV_START(fun);
                    for (j=0; j<SIMPLE_FUN_SCAV_NWORDS(fun); ++j)
                        coalesce_obj(fun_slots+j, ht);
                })
                nwords = code_header_words(header);
                break;
            default:
                if (unboxed_obj_widetag_p(widetag))
                    continue; // Ignore this object.
            }
            for(i=1; i<nwords; ++i)
                coalesce_obj(where+i, ht);
        }
    }
    return 0;
}

/* Do as good as job as we can to de-duplicate strings
 * This doesn't need to scan stacks or anything fancy.
 * It's not wrong to fail to coalesce things that could have been */
void coalesce_similar_objects()
{
    struct hopscotch_table ht;
    uword_t arg = (uword_t)&ht;

    hopscotch_create(&ht, HOPSCOTCH_VECTOR_HASH, 0, 1<<17, 0);
#ifndef LISP_FEATURE_WIN32
    // Apparently this triggers the "Unable to recommit" lossage message
    // in handle_access_violation() in src/runtime/win32-os.c
    coalesce_range((lispobj*)READ_ONLY_SPACE_START,
                   (lispobj*)READ_ONLY_SPACE_END,
                   arg);
    coalesce_range((lispobj*)STATIC_SPACE_START,
                   (lispobj*)STATIC_SPACE_END,
                   arg);
#endif
#ifdef LISP_FEATURE_IMMOBILE_SPACE
    coalesce_range((lispobj*)FIXEDOBJ_SPACE_START, fixedobj_free_pointer, arg);
    coalesce_range((lispobj*)VARYOBJ_SPACE_START, varyobj_free_pointer, arg);
#endif
#ifdef LISP_FEATURE_GENCGC
    walk_generation(coalesce_range, -1, arg);
#else
    coalesce_range(current_dynamic_space, dynamic_space_free_pointer, arg);
#endif
    hopscotch_destroy(&ht);
}