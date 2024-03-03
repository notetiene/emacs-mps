/* Incremental, generational, concurrent GC using MPS.
   Copyright (C) 2024 Free Software Foundation, Inc.

Author: Gerd Möllmann <gerd@gnu.org>

This file is part of GNU Emacs.

GNU Emacs is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

GNU Emacs is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>. */

/* Todo:

   + create area root, area scanner
   + staticpro roots
   + built-in symbols root
   + buffer-locals roots
   + specpdl
   + pdumper
   + intervals, overlays
   + run MPS tests
   + --enable-checking
   + mark_lread
   + marl_window
   + alloc conses

   - dump_context

   I think this is handled by scanning what mem_insert has, since
   intervals and overlays are allocated from blocks that are
registered with mem_insert.
   + thread roots (control stack), main thread
   + thread-local allocation points
   + mps_arena_step, idle time.
   + face cache
   + glyph matrices
   + HAVE_TEXT_CONVERSION - can't do it
   - complete cons_skip etc.

   - telemetry
   - symbols, strings etc
   - emacs_abort -> something nicer

*/

// clang-format off

#include <config.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef HAVE_MPS

#include <mps.h>
#include <mpsavm.h>
#include <mpscamc.h>
#include <mpscams.h>
#include <stdlib.h>
#include "lisp.h"
#include "buffer.h"
#include "thread.h"
#include "pdumper.h"
#include "dispextern.h"
#include "igc.h"
#include "puresize.h"

/* For simplicity, I don't suport some stuff.  Should maybe done in
   configure.ac.  */

#ifndef USE_LSB_TAG
#error "USE_LSB_TAG required"
#endif
#ifdef WIDE_EMACS_INT
#error "WIDE_EMACS_INT not supported"
#endif

/* Frames have stuff for text conversion which contains Lisp_Objects, so
   this must be scanned/fixed for MPS, and must be some form of root in
   a mixed GC, so that scanning the Lisp_Objects woiuld prevent moving
   them in memory.  MacOS doesn't HAVE_TEXT_CONVERSION, so that I can't
   do this.  */

#ifdef HAVE_TEXT_CONVERSION
#error "HAVE_TEXT_CONVERSION not supported"
#endif

#ifdef IGC_DEBUG_POOL
#define IGC_CHECK_POOL()				\
do							\
  {							\
    mps_pool_check_fenceposts (global_igc->cons_pool);	\
    mps_pool_check_free_space (global_igc->cons_pool);	\
    mps_pool_check_fenceposts (global_igc->symbol_pool);	\
    mps_pool_check_free_space (global_igc->symbol_pool);	\
  } while (0)
#else
#define IGC_CHECK_POOL() (void) 0
#endif

#ifdef IGC_DEBUG
#define IGC_ASSERT(expr)      if (!(expr)) emacs_abort (); else
#define IGC_ASSERT_ALIGNED(p) IGC_ASSERT ((uintptr_t) (p) % GCALIGNMENT == 0)
#else
#define IGC_ASSERT(expr)
#define IGC_ASSERT_ALIGNED(p)
#endif

/* In MPS scan functions it is not easy to call C functions (see the MPS
   documentation).  Rather than taking the risk of using functions from
   lisp.h, which may may not be inlined, I'm therfore using macros. I
   assume that Lisp_Objects are EMACS_INTs, and we are using the 3
   lowest bits for tags, for simplicity.  */

#define IGC_TAG_MASK	(~ VALMASK)

#define IGC_TAG(x) ((mps_word_t) (x) & IGC_TAG_MASK)
#define IGC_VAL(x) ((mps_word_t) (x) & ~IGC_TAG_MASK)

static mps_res_t scan_staticvec (mps_ss_t ss, void *start,
				 void *end, void *closure);
static mps_res_t scan_faces_by_id (mps_ss_t ss, void *start, void *end,
				    void *closure);
static mps_res_t scan_glyph_rows (mps_ss_t ss, void *start, void *end,
				  void *closure);

#define IGC_CHECK_RES(res)			\
  if ((res) != MPS_RES_OK)			\
    emacs_abort ();				\
  else

#define IGC_WITH_PARKED(gc)			\
  for (int i = (mps_arena_park(gc->arena), 1);	\
       i;					\
       i = (mps_arena_release (gc->arena), 0))

/* Very poor man's template for double-linked lists.  */

#define IGC_DEFINE_LIST(data)				\
  typedef struct data##_list				\
  {							\
    struct data##_list *next, *prev;			\
    data d;						\
  } data##_list;					\
							\
  static data##_list *					\
  data##_list_push (data##_list **head, data *d)	\
  {							\
    data##_list *r = xzalloc (sizeof *r);		\
    r->d = *d;						\
    r->next = *head;					\
    r->prev = NULL;					\
							\
    if (r->next)					\
      r->next->prev = r;				\
    *head = r;						\
    return r;						\
  }							\
							\
  static void						\
  data##_list_remove (data *d, data##_list **head,	\
		      data##_list *r)			\
  {							\
    if (r->next)					\
      r->next->prev = r->prev;				\
    if (r->prev)					\
      r->prev->next = r->next;				\
    else						\
      *head = r->next;					\
    *d = r->d;						\
    xfree (r);						\
  }

struct igc_root
{
  struct igc *gc;
  mps_root_t root;
  void *start, *end;
};

typedef struct igc_root igc_root;
IGC_DEFINE_LIST (igc_root);

struct igc_thread
{
  struct igc *gc;
  mps_thr_t thr;
  void *cold;
  struct igc_root_list *specpdl_root;
  mps_ap_t cons_ap, symbol_ap;
};

typedef struct igc_thread igc_thread;
IGC_DEFINE_LIST (igc_thread);

struct igc
{
  mps_arena_t arena;
  mps_chain_t chain;
  mps_pool_t cons_pool;
  mps_fmt_t cons_fmt;
  mps_pool_t symbol_pool;
  mps_fmt_t symbol_fmt;
  struct igc_root_list *roots;
  struct igc_thread_list *threads;
};

static struct igc *global_igc;


/***********************************************************************
				Roots
 ***********************************************************************/

/* Add ROOT to the root registry of GC.  Value is a pointer to a new
   igc_root_list struct for the root.  */

static struct igc_root_list *
register_root (struct igc *gc, mps_root_t root, void *start, void *end)
{
  struct igc_root r = { .gc = gc, .root = root, .start = start, .end = end };
  return igc_root_list_push (&gc->roots, &r);
}

static igc_root_list *
find_root_with_start (struct igc *gc, void *start)
{
  for (struct igc_root_list *r = gc->roots; r; r = r->next)
    if (r->d.start == start)
      return r;
  return NULL;
}

/* Remove root R from its root registry, and free it.  Value is the MPS
   root that was registered.  */

static mps_root_t
deregister_root (struct igc_root_list *r)
{
  struct igc_root root;
  igc_root_list_remove (&root, &r->d.gc->roots, r);
  return root.root;
}

/* Destroy the MPS root in R, and deregister it.  */

static void
remove_root (struct igc_root_list *r)
{
  mps_root_destroy (deregister_root (r));
}

/* Destroy all registered roots of GC.  */

static void
remove_all_roots (struct igc *gc)
{
  while (gc->roots)
    remove_root (gc->roots);
}

static mps_root_t
make_ambig_root (struct igc *gc, void *start, void *end)
{
  mps_root_t root;
  mps_res_t res
    = mps_root_create_area_tagged (&root,
				   gc->arena,
				   mps_rank_ambig (),
				   0, /* MPS_PROT_... */
				   start,
				   end,
				   mps_scan_area_masked,
				   IGC_TAG_MASK,
				   0);
  IGC_CHECK_RES (res);
  return root;
}

/* Called from mem_insert.  Create an MPS root for the memory area
   between START and END, and remember it in the root registry of
   global_igc.  */

void *
igc_on_mem_insert (void *start, void *end)
{
  mps_root_t root = make_ambig_root (global_igc, start, end);
  return register_root (global_igc, root, start, end);
}

/* Called from mem_delete.  Remove the correspoing node INFO from the
   registry.  */

void
igc_on_mem_delete (void *info)
{
  remove_root ((struct igc_root_list *) info);
}

void *
igc_xalloc_ambig_root (size_t size)
{
  char *start = xzalloc (size);
  char *end = start + size;
  mps_root_t root = make_ambig_root (global_igc, start, end);
  register_root (global_igc, root, start, end);
  return start;
}

void
igc_xfree_ambig_root (void *p)
{
  if (p == NULL)
    return;

  struct igc_root_list *r = find_root_with_start (global_igc, p);
  IGC_ASSERT (r != NULL);
  remove_root (r);
}

/* Add a root for staticvec to GC.  */

static void
add_staticvec_root (struct igc *gc)
{
  void *start =staticvec, *end = staticvec + ARRAYELTS (staticvec);
  mps_root_t root;
  mps_res_t res
    = mps_root_create_area (&root, gc->arena, mps_rank_ambig (), 0,
			    start, end, scan_staticvec, NULL);
  IGC_CHECK_RES (res);
  register_root (gc, root, start, end);
}

static void
add_builtin_symbols_root (struct igc *gc)
{
  void *start = lispsym, *end = lispsym + ARRAYELTS (lispsym);
  mps_root_t root = make_ambig_root (gc, start, end);
  register_root (gc, root, start, end);
}

/* Odeally, we shoudl not scan the entire area, only to the current
   ptr. And ptr might change in the mutator.  Don't know how this could
   be done with MPS running concurrently.  Instead, make sure that the
   part of the stack that is not used is zeroed.  */

static void
add_specpdl_root (struct igc_thread_list *t)
{
  // For the initial thread, specpdl will be initialzed by
  // init_eval_once, and will be NULL until that happens.
  if (specpdl)
    {
      struct igc *gc = t->d.gc;
      void *start = specpdl, *end = specpdl_end;
      mps_root_t root = make_ambig_root (gc, start, end);
      t->d.specpdl_root = register_root (gc, root, start, end);
    }
}

void
igc_on_specbinding_unused (union specbinding *b)
{
  memset (b, 0, sizeof *b);
}

void
igc_on_alloc_main_thread_specpdl (void)
{
  struct igc_thread_list *t = current_thread->gc_info;
  add_specpdl_root (t);
}

/* Called when specpdl gets reallacated.  */

void
igc_on_grow_specpdl (void)
{
  struct igc_thread_list *t = current_thread->gc_info;
  // FIXME: can we avoid parking?
  IGC_WITH_PARKED (t->d.gc)
    {
      remove_root (t->d.specpdl_root);
      t->d.specpdl_root = NULL;
      add_specpdl_root (t);
    }
}

/* Add a root to GC for scanning buffer B.  */

static void
add_buffer_root (struct igc *gc, struct buffer *b)
{
  void *start = &b->name_, *end = &b->own_text;
  mps_root_t root = make_ambig_root (gc, start, end);
  register_root (gc, root, start, end);
}

/* All all known static roots in Emacs to GC.  */

static void
add_static_roots (struct igc *gc)
{
  add_buffer_root (gc, &buffer_defaults);
  add_buffer_root (gc, &buffer_local_symbols);
  add_staticvec_root (gc);
  add_builtin_symbols_root (gc);
}

/* Add a root for a thread given by T.  */

static void
add_thread_root (struct igc_thread_list *t)
{
  struct igc *gc = t->d.gc;
  mps_root_t root;
  mps_res_t res = mps_root_create_thread_tagged
    (&root,
     gc->arena,
     mps_rank_ambig (),
     0,
     t->d.thr,
     mps_scan_area_masked,
     /* Docs of mps_scan_area_masked.  The mask and pattern are passed
	to the scan function via its closure argument.  The mask is for
	the tag bits, not to get the value without tag bits.  */
     IGC_TAG_MASK,
     /* The pattern is unused by mps_scan_area_masked.  */
     0,
     t->d.cold);
  IGC_CHECK_RES (res);
  register_root (gc, root, t->d.cold, NULL);
}

/* Called after a pdump has been loaded.  Add the area as root.  */

void
igc_on_pdump_loaded (void)
{
  struct igc *gc = global_igc;
  void *start = (void *) dump_public.start, *end = (void *) dump_public.end;
  mps_root_t root = make_ambig_root (gc, start, end);
  register_root (gc, root, start, end);
}

/* For all faces in a face cache, we need to fix the lface vector of
   Lisp_Objects.  */

void
igc_on_make_face_cache (void *c)
{
  struct face_cache *cache = c;
  struct igc *gc = global_igc;
  void *start = (void *) cache->faces_by_id;
  void *end = (void *) (cache->faces_by_id + cache->size);
  mps_root_t root;
  mps_res_t res
    = mps_root_create_area (&root, gc->arena, mps_rank_ambig (), 0,
			    start, end, scan_faces_by_id, NULL);
  IGC_CHECK_RES (res);
  cache->igc_info = register_root (gc, root, start, end);
}

void
igc_on_free_face_cache (void *c)
{
  struct face_cache *cache = c;
  remove_root (cache->igc_info);
  cache->igc_info = NULL;
}

void
igc_on_face_cache_change (void *c)
{
  /* FIXME: can we avoid parking? The idea would be to add a new root
     first, and then remove the old one, so that there is no gap in
     which we don't have no root.  Alas, MPS says that no two roots may
     overlap, which could be the case with realloc.  */
  IGC_WITH_PARKED (global_igc)
    {
      igc_on_free_face_cache (c);
      igc_on_make_face_cache (c);
    }
}

void
igc_on_adjust_glyph_matrix (void *m)
{
  struct igc *gc = global_igc;
  struct glyph_matrix *matrix = m;
  IGC_WITH_PARKED (gc)
    {
      if (matrix->igc_info)
	remove_root (matrix->igc_info);
      mps_root_t root;
      void *start = matrix->rows;
      void *end = (void *) (matrix->rows + matrix->rows_allocated);
      mps_res_t res
	= mps_root_create_area (&root, gc->arena, mps_rank_ambig (), 0,
				start, end,
				scan_glyph_rows, NULL);
      IGC_CHECK_RES (res);
      matrix->igc_info = register_root (gc, root, start, end);
    }
}

void
igc_on_free_glyph_matrix (void *m)
{
  struct glyph_matrix *matrix = m;
  if (matrix->igc_info)
    {
      remove_root (matrix->igc_info);
      matrix->igc_info = NULL;
    }
}

void *
igc_on_grow_read_stack (void *info, void *start, void *end)
{
  struct igc *gc = global_igc;
  IGC_WITH_PARKED (gc)
    {
      if (info)
	remove_root (info);
      mps_root_t root = make_ambig_root (gc, start, end);
      info = register_root (gc, root, start, end);
    }

  return info;
}

static void
release_arena (void)
{
  mps_arena_release (global_igc->arena);
}

specpdl_ref
igc_inhibit_garbage_collection (void)
{
  specpdl_ref count = SPECPDL_INDEX ();
  mps_arena_park (global_igc->arena);
  record_unwind_protect_void (release_arena);
  return count;
}


/***********************************************************************
			   Allocation Points
 ***********************************************************************/

static void
make_thread_aps (struct igc_thread *t)
{
  struct igc *gc = t->gc;
  mps_res_t res;

  res = mps_ap_create_k (&t->cons_ap, gc->cons_pool, mps_args_none);
  IGC_CHECK_RES (res);
  res = mps_ap_create_k (&t->symbol_ap, gc->symbol_pool, mps_args_none);
  IGC_CHECK_RES (res);
}

static void
free_thread_aps (struct igc_thread_list *t)
{
  mps_ap_destroy (t->d.cons_ap);
  t->d.cons_ap = NULL;
}


/***********************************************************************
				Threads
 ***********************************************************************/

static struct igc_thread_list *
register_thread (struct igc *gc, mps_thr_t thr, void *cold)
{
  struct igc_thread t = { .gc = gc, .thr = thr, .cold = cold };
  return igc_thread_list_push (&gc->threads, &t);
}

static mps_thr_t
deregister_thread (struct igc_thread_list *t)
{
  struct igc_thread thread;
  igc_thread_list_remove (&thread, &t->d.gc->threads, t);
  return thread.thr;
}

/* Called from run_thread.  */

void *
igc_thread_add (const void *cold)
{
  mps_thr_t thr;
  mps_res_t res = mps_thread_reg (&thr, global_igc->arena);
  IGC_CHECK_RES (res);

  struct igc_thread_list *t
    = register_thread (global_igc, thr, (void *) cold);

  add_thread_root (t);
  add_specpdl_root (t);
  make_thread_aps (&t->d);
  return t;
}

/* Called from run_thread.  */

void
igc_thread_remove (void *info)
{
  struct igc_thread_list *t = info;
  free_thread_aps (t);
  mps_thread_dereg (deregister_thread (t));
}

static void
free_all_threads (struct igc *gc)
{
  while (gc->threads)
    igc_thread_remove (gc->threads);
}

static void
add_main_thread (void)
{
  current_thread->gc_info = igc_thread_add (stack_bottom);
}



/***********************************************************************
				Scanning
 ***********************************************************************/

#define IGC_FIX(ss, x)							\
  do									\
    {									\
      mps_word_t *p_ = (mps_word_t *) (x);				\
      mps_word_t word = *p_;						\
      mps_word_t tag = word & IGC_TAG_MASK;				\
      if (tag != Lisp_Int0 && tag != Lisp_Int1)				\
	{								\
	  mps_word_t off = word ^ tag;					\
	  mps_addr_t ref = (mps_addr_t) off;				\
	  if (tag == Lisp_Symbol)					\
	    ref = (char *) lispsym + off;				\
	  if (MPS_FIX1 (ss, ref))					\
	    {								\
	      mps_res_t res = MPS_FIX2 (ss, &ref);			\
	      if (res != MPS_RES_OK)					\
		return res;						\
	      if (tag == Lisp_Symbol)					\
		ref = (mps_addr_t) ((char *) ref - (char *) lispsym);	\
	      *p_ = (mps_word_t) ref | tag;				\
	    }								\
	}								\
    }									\
   while (0)

/* Horrible shit to avoid unused variable warnings.  */

static int fwdsig;
#define IGC_FWDSIG ((mps_addr_t) &fwdsig)

struct igc_fwd {
  mps_addr_t sig;
  mps_addr_t new;
};

static void
forward (mps_addr_t old, mps_addr_t new)
{
  struct igc_fwd m = { .sig = IGC_FWDSIG, .new = new };
  struct igc_fwd *f = old;
  *f = m;
}

static mps_addr_t
is_forwarded (mps_addr_t addr)
{
  struct igc_fwd *f = addr;
  return f->sig == IGC_FWDSIG ? f->new : NULL;
}

static int padsig;
#define IGC_PADSIG ((mps_addr_t) &padsig)

struct igc_pad {
  mps_addr_t sig;
};

static void
pad (mps_addr_t addr, size_t size)
{
  struct igc_pad padding = { .sig = IGC_PADSIG };
  IGC_ASSERT (size <= sizeof padding);

  *(struct igc_pad *) addr = padding;
  char *p = (char *) addr + sizeof padding;
  char *end = (char *) addr + size;
  while (p < end)
    {
      static const char string[] = "padding";
      const size_t n = min (sizeof string, end - p);
      memcpy (p, string, n);
      p += n;
    }
}

static bool
is_padding (mps_addr_t addr)
{
  struct igc_pad *p = addr;
  return p->sig == IGC_PADSIG;
}

/* These may come from MPS_SCAN_BEGIN / END.  */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"

/* Scan a vector of glyph_rows.  */

static mps_res_t
scan_glyph_rows (mps_ss_t ss, void *start, void *end, void *closure)
{
  MPS_SCAN_BEGIN (ss)
    {
      for (struct glyph_row *row = start; row < (struct glyph_row *) end; ++row)
	{
	  struct glyph *glyph = row->glyphs[LEFT_MARGIN_AREA];
	  struct glyph *end = row->glyphs[LAST_AREA];
	  for (; glyph < end; ++glyph)
	    IGC_FIX (ss, &glyph->object);
	}
    }
  MPS_SCAN_END (ss);
  return MPS_RES_OK;
}

static mps_res_t
scan_faces_by_id (mps_ss_t ss, void *start, void *end, void *closure)
{
  MPS_SCAN_BEGIN (ss)
  {
    for (struct face **p = start; p < (struct face **) end; ++p)
      if (*p)
	{
	  struct face *face = *p;
	  for (int i = 0; i < ARRAYELTS (face->lface); ++i)
	    IGC_FIX (ss, &face->lface[i]);
	}
  }
  MPS_SCAN_END (ss);
  return MPS_RES_OK;
}

/* Scan staticvec in the interval [START, END). SS is the MPS scan
   state.  CLOSURE is ignored.  */

static mps_res_t
scan_staticvec (mps_ss_t ss, void *start, void *end, void *closure)
{
  MPS_SCAN_BEGIN (ss)
    {
      /* I don't want to rely on staticidx ATM. Instead, ignore NULL
	 entries.  */
      for (Lisp_Object **p = start; p < (Lisp_Object **) end; ++p)
	if (*p)
	  IGC_FIX (ss, *p);
    }
  MPS_SCAN_END (ss);
  return MPS_RES_OK;
}

#pragma GCC diagnostic pop

/* Scan a Lisp_Cons.  Must be able to handle padding and forwaring
   objects. */

static mps_res_t
cons_scan (mps_ss_t ss, mps_addr_t base, mps_addr_t limit)
{
  MPS_SCAN_BEGIN (ss)
    {
      for (struct Lisp_Cons *cons = (struct Lisp_Cons *) base;
	   cons < (struct Lisp_Cons *) limit;
	   ++cons)
	{
	  if (is_forwarded (cons) || is_padding (cons))
	    continue;

	  IGC_FIX (ss, &cons->u.s.car);
	  IGC_FIX (ss, &cons->u.s.u.cdr);
	}
    }
  MPS_SCAN_END (ss);
  return MPS_RES_OK;
}

static mps_addr_t
cons_skip (mps_addr_t addr)
{
  return (char *) addr + sizeof (struct Lisp_Cons);
}

/* Called by MPS when object at OLD has been moved to NEW.  Must replace
   *OLD with a forwarding marker that points to NEW.  */

static void
cons_fwd (mps_addr_t old, mps_addr_t new)
{
  IGC_ASSERT (false);
  forward (old, new);
}

static mps_addr_t
cons_isfwd (mps_addr_t addr)
{
  IGC_ASSERT (false);
  return is_forwarded (addr);
}

static void
cons_pad (mps_addr_t addr, size_t size)
{
  pad (addr, size);
}

static mps_res_t
symbol_scan (mps_ss_t ss, mps_addr_t base, mps_addr_t limit)
{
  MPS_SCAN_BEGIN (ss)
    {
      for (struct Lisp_Symbol *sym = (struct Lisp_Symbol *) base;
	   sym < (struct Lisp_Symbol *) limit;
	   ++sym)
	{
	  if (is_forwarded (sym) || is_padding (sym))
	    continue;

	  IGC_FIX (ss, &sym->u.s.name);
	  if (sym->u.s.redirect == SYMBOL_PLAINVAL)
	    IGC_FIX (ss, &sym->u.s.val.value);
	  IGC_FIX (ss, &sym->u.s.function);
	  IGC_FIX (ss, &sym->u.s.plist);
	  IGC_FIX (ss, &sym->u.s.package);
	}
    }
  MPS_SCAN_END (ss);
  return MPS_RES_OK;
}

static mps_addr_t
symbol_skip (mps_addr_t addr)
{
  return (char *) addr + sizeof (struct Lisp_Symbol);
}

static void
symbol_fwd (mps_addr_t old, mps_addr_t new)
{
  IGC_ASSERT (false);
  forward (old, new);
}

static mps_addr_t
symbol_isfwd (mps_addr_t addr)
{
  IGC_ASSERT (false);
  return is_forwarded (addr);
}

static void
symbol_pad (mps_addr_t addr, size_t size)
{
  pad (addr, size);
}



/***********************************************************************
				Walking
 ***********************************************************************/

struct igc_walk
{
  void (* fun) (Lisp_Object);
  int count;
};

/* Visit Lisp_Object contained in a cons.  */

static void
mark_old_object (Lisp_Object obj)
{
  switch (XTYPE (obj))
    {
      /* No need to mark_object.  */
    case Lisp_Int0:
    case Lisp_Int1:
      break;

      /* Not mamanged by old GC.  */
    case Lisp_Cons:
    case Lisp_Symbol:
      return;

    default:
      mark_object (obj);
      break;
    }
}

static mps_res_t
mark_cons_area (mps_ss_t ss, mps_addr_t base, mps_addr_t limit,
		void *closure)
{
  for (struct Lisp_Cons *p = base; p < (struct Lisp_Cons *) limit; ++p)
    {
      mark_old_object (p->u.s.car);
      mark_old_object (p->u.s.u.cdr);
    }

  return MPS_RES_OK;
}

static mps_res_t
mark_symbol_area (mps_ss_t ss, mps_addr_t base, mps_addr_t limit,
		  void *closure)
{
  for (struct Lisp_Symbol *p = base; p < (struct Lisp_Symbol *) limit; ++p)
    {
      mark_old_object (p->u.s.name);

      switch (p->u.s.redirect)
	{
	case SYMBOL_PLAINVAL:
	  mark_old_object (p->u.s.val.value);
	  break;
	case SYMBOL_VARALIAS:
	  {
	    Lisp_Object tem;
	    XSETSYMBOL (tem, SYMBOL_ALIAS (p));
	    mark_old_object (tem);
	    break;
	  }
	case SYMBOL_LOCALIZED:
	  {
	    struct Lisp_Buffer_Local_Value *blv = SYMBOL_BLV (p);
	    Lisp_Object where = blv->where;
	    /* If the value is set up for a killed buffer,
	       restore its global binding.  */
	    if (BUFFERP (where) && !BUFFER_LIVE_P (XBUFFER (where)))
	      swap_in_global_binding (p);
	    mark_old_object (blv->where);
	    mark_old_object (blv->valcell);
	    mark_old_object (blv->defcell);
	  }
	  break;
	case SYMBOL_FORWARDED:
	  /* If the value is forwarded to a buffer or keyboard field,
	     these are marked when we see the corresponding object.
	     And if it's forwarded to a C variable, either it's not
	     a Lisp_Object var, or it's staticpro'd already.  */
	  break;
	default:
	  emacs_abort ();
	}
      if (!PURE_P (XSTRING (p->u.s.name)))
	set_string_marked (XSTRING (p->u.s.name));
      mark_interval_tree (string_intervals (p->u.s.name));

      mark_old_object (p->u.s.function);
      mark_old_object (p->u.s.plist);
      mark_old_object (p->u.s.package);
    }

  return MPS_RES_OK;
}

void
igc_mark_old_objects_referenced_from_pools (void)
{
  struct igc *gc = global_igc;
  IGC_WITH_PARKED (gc)
    {
      mps_pool_walk (gc->cons_pool, mark_cons_area, NULL);
      mps_pool_walk (gc->symbol_pool, mark_symbol_area, NULL);
    }
}

/***********************************************************************
				Finalization
 ***********************************************************************/

/* ADDR is a block registered for finalization with mps_finalize.
   AFAICT, this is always a PVEC_FINALIZER.  */

static void
do_finalize (struct igc *gc, mps_addr_t addr)
{
  struct Lisp_Finalizer *fin = addr;
  if (!NILP (fin->function))
    {
      Lisp_Object fun = fin->function;
      fin->function = Qnil;
      run_finalizer_function (fun);
    }
}

static void
handle_messages (struct igc *gc)
{
  mps_message_type_t type;
  while (mps_message_queue_type (&type, gc->arena))
    {
      mps_message_t msg;
      if (mps_message_get (&msg, gc->arena, type))
	{
	  IGC_ASSERT (type == mps_message_type_finalization ());
	  mps_addr_t addr;
	  mps_message_finalization_ref (&addr, gc->arena, msg);
	  do_finalize (gc, addr);
	  mps_message_discard (gc->arena, msg);
	}
    }
}

static void
enable_finalization (struct igc *gc, bool enable)
{
  mps_message_type_t type = mps_message_type_finalization ();
  if (enable)
    mps_message_type_enable (gc->arena, type);
  else
    mps_message_type_disable (gc->arena, type);
}

void
igc_handle_messages (void)
{
  handle_messages (global_igc);
}

void
igc_on_idle (void)
{
  mps_arena_step (global_igc->arena, 0.01, 0);
}


/***********************************************************************
			    Allocation
 ***********************************************************************/

static mps_ap_t
current_cons_ap (void)
{
  struct igc_thread_list *t = current_thread->gc_info;
  return t->d.cons_ap;
}

static mps_ap_t
current_symbol_ap (void)
{
  struct igc_thread_list *t = current_thread->gc_info;
  return t->d.symbol_ap;
}

void igc_break (void)
{
}

Lisp_Object
igc_make_cons (Lisp_Object car, Lisp_Object cdr)
{
  mps_ap_t ap = current_cons_ap ();
  size_t size = sizeof (struct Lisp_Cons);
  mps_addr_t p;
  do
    {
      mps_res_t res = mps_reserve (&p, ap, size);
      IGC_CHECK_RES (res);
      struct Lisp_Cons *cons = p;
      cons->u.s.car = car;
      cons->u.s.u.cdr = cdr;
    }
  while (!mps_commit (ap, p, size));

  return make_lisp_ptr (p, Lisp_Cons);
}

Lisp_Object
igc_alloc_symbol (void)
{
  mps_ap_t ap = current_symbol_ap ();
  size_t size = sizeof (struct Lisp_Symbol);
  mps_addr_t p;
  do
    {
      mps_res_t res = mps_reserve (&p, ap, size);
      IGC_CHECK_RES (res);
    }
  while (!mps_commit (ap, p, size));

  return make_lisp_symbol ((struct Lisp_Symbol *) p);
}


/***********************************************************************
			    Setup/Tear down
 ***********************************************************************/

static struct igc *
make_igc (void)
{
  struct igc *gc = xzalloc (sizeof *gc);
  mps_res_t res;

  // Arena
  MPS_ARGS_BEGIN (args)
  {
    res = mps_arena_create_k (&gc->arena, mps_arena_class_vm (), args);
  }
  MPS_ARGS_END (args);
  IGC_CHECK_RES (res);

  // Generations
  mps_gen_param_s gen_params[]
    = { { 32000, 0.8 }, { 5 * 32009, 0.4 } };
  res = mps_chain_create (&gc->chain, gc->arena, ARRAYELTS (gen_params),
			  gen_params);
  IGC_CHECK_RES (res);

  // Object format for conses.
  MPS_ARGS_BEGIN (args)
  {
    MPS_ARGS_ADD (args, MPS_KEY_FMT_ALIGN, GCALIGNMENT);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_HEADER_SIZE, 0);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_SCAN, cons_scan);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_SKIP, cons_skip);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_FWD, cons_fwd);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_ISFWD, cons_isfwd);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_PAD, cons_pad);
    res = mps_fmt_create_k (&gc->cons_fmt, gc->arena, args);
  }
  MPS_ARGS_END (args);
  IGC_CHECK_RES (res);

#ifdef IGC_DEBUG_POOL
  mps_class_t ams_pool_class = mps_class_ams_debug ();
#else
  mps_class_t ams_pool_class = mps_class_ams ();
#endif

  /* In a debug pool, fill fencepost and freed objects with a
     byte pattern. This is ignored in non-debug pools.

     (lldb) memory read cons_ptr
     0x17735fe68: 66 72 65 65 66 72 65 65 66 72 65 65 66 72 65 65  freefreefreefree
     0x17735fe78: 66 72 65 65 66 72 65 65 66 72 65 65 66 72 65 65  freefreefreefree
  */
  mps_pool_debug_option_s debug_options = {
    "fence", 5,
    "free", 4,
  };

  // Pool for conses. Since conses have no type field which would let
  // us recognize them when mixed with other objects, use a dedicated
  // pool.
  MPS_ARGS_BEGIN (args)
    {
      MPS_ARGS_ADD(args, MPS_KEY_POOL_DEBUG_OPTIONS, &debug_options);
      MPS_ARGS_ADD (args, MPS_KEY_FORMAT, gc->cons_fmt);
      MPS_ARGS_ADD (args, MPS_KEY_CHAIN, gc->chain);
      MPS_ARGS_ADD (args, MPS_KEY_INTERIOR, 0);
      res = mps_pool_create_k (&gc->cons_pool, gc->arena,
			       ams_pool_class, args);
    }
  MPS_ARGS_END (args);
  IGC_CHECK_RES (res);

  // Object format for conses.
  MPS_ARGS_BEGIN (args)
  {
    MPS_ARGS_ADD (args, MPS_KEY_FMT_ALIGN, GCALIGNMENT);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_HEADER_SIZE, 0);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_SCAN, symbol_scan);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_SKIP, symbol_skip);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_FWD, symbol_fwd);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_ISFWD, symbol_isfwd);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_PAD, symbol_pad);
    res = mps_fmt_create_k (&gc->symbol_fmt, gc->arena, args);
  }
  MPS_ARGS_END (args);
  IGC_CHECK_RES (res);

  // Pool for conses. Since conses have no type field which would let
  // us recognize them when mixed with other objects, use a dedicated
  // pool.
  MPS_ARGS_BEGIN (args)
    {
      MPS_ARGS_ADD(args, MPS_KEY_POOL_DEBUG_OPTIONS, &debug_options);
      MPS_ARGS_ADD (args, MPS_KEY_FORMAT, gc->symbol_fmt);
      MPS_ARGS_ADD (args, MPS_KEY_CHAIN, gc->chain);
      MPS_ARGS_ADD (args, MPS_KEY_INTERIOR, 0);
      res = mps_pool_create_k (&gc->symbol_pool, gc->arena,
			       ams_pool_class, args);
    }
  MPS_ARGS_END (args);
  IGC_CHECK_RES (res);

  add_static_roots (gc);
  enable_finalization (gc, true);

  return gc;
}

static void
free_igc (struct igc *gc)
{
  free_all_threads (gc);
  mps_pool_destroy (gc->cons_pool);
  mps_fmt_destroy (gc->cons_fmt);
  mps_pool_destroy (gc->symbol_pool);
  mps_fmt_destroy (gc->symbol_fmt);
  remove_all_roots (gc);
  mps_chain_destroy (gc->chain);
  mps_arena_destroy (gc->arena);
  xfree (gc);
}

static void
free_global_igc (void)
{
  free_igc (global_igc);
}

void
syms_of_igc (void)
{
}

void
init_igc (void)
{
  global_igc = make_igc ();
  atexit (free_global_igc);
  add_main_thread ();
}

#endif // HAVE_MPS
