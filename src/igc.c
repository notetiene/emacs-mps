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
   - use MPS_RM_PROT ?
   - staticpro roots
   - buffer-locals roots
   - thread roots (control stack)
   - thread-local allocation points
   - emacs_abort -> something nicer
   - Use mps_arena_step during idle time. This lets MPS take a
   specified maximum amount of time (default 10ms) for its work.
*/

// clang-format on

#include <config.h>

#ifdef HAVE_MPS

# include <mps.h>
# include <mpsavm.h>
# include <mpscamc.h>
# include <mpscams.h>
# include <stdlib.h>
# include "lisp.h"
# include "igc.h"

/* In MPS scan functions it is not easy to call C functions (see the
   MPS documentation).  Rather than taking the risk of using functions
   from lisp.h, which may may not be inlined, I'm therfore using some
   macros, and assume that Lisp_Objects are EMACS_INTs, and we are
   using the 3 lowest bits for tags.  Good enough for me, ATM.  */

# define IGC_TAG(obj) ((EMACS_INT) (obj) & 0x7)
# define IGC_UNTAGGED(obj) ((EMACS_INT) (obj) & ~0x7)

# define IGC_MAKE_LISP_OBJ(untagged, tag) \
   ((Lisp_Object) ((EMACS_INT) (untagged) | (tag)))

# define IGC_FIXNUMP(obj) \
   (IGC_TAG (obj) == Lisp_Int0 || IGC_TAG (obj) == Lisp_Int1)

static mps_res_t igc_scan_mem_area (mps_ss_t ss, void *start, void *end,
				    void *closure);
static mps_res_t igc_scan_staticvec (mps_ss_t ss, void *start, void *end,
				     void *closure);

/* Boolkeeping of what we need to know about the MPS GC.  Avoid tons of
   global variables.  */
struct igc
{
  /* The MPS arena.  */
  mps_arena_t arena;

  /* Generations in the arena.  */
  mps_chain_t chain;

  /* MPS pool for conses.  This is a non-moving pool as long as not all
     Lisp object types are managed by MPS.  */
  mps_pool_t cons_pool;
  mps_fmt_t cons_fmt;

  /* One MPS root in the root registry.  */
  struct igc_root
  {
    struct igc_root *next, *prev;
    struct igc *gc;
    mps_root_t root;
  } *roots;
};

static struct igc *global_igc = NULL;

/***********************************************************************
				Roots
 ***********************************************************************/

/* Add ROOT to the root registry of GC.  Value is a pointer to a new
   igc_root struct for the root.  */

static struct igc_root *
igc_register_root (struct igc *gc, mps_root_t root)
{
  struct igc_root *r = xzalloc (sizeof *r);
  r->gc = gc;
  r->root = root;
  r->next = gc->roots;
  r->prev = NULL;

  if (r->next)
    r->next->prev = r;
  gc->roots = r;
  return r;
}

/* Remove root description R from its root registry, and free it.  Value
   is the MPS root that was registered.  */

static mps_root_t
igc_deregister_root (struct igc_root *r)
{
  if (r->next)
    r->next->prev = r->prev;
  if (r->prev)
    r->prev->next = r->next;
  else
    r->gc->roots = r->next;
  mps_root_t root = r->root;
  xfree (r);
  return root;
}

/* Destroy the MPS root in R, and deregister it.  This is called from
   mem_delete.  */

static void
igc_remove_root (struct igc_root *r)
{
  mps_root_destroy (igc_deregister_root (r));
}

/* Destroy all registered roots of GC.  */

static void
igc_remove_all_roots (struct igc *gc)
{
  while (gc->roots)
    igc_remove_root (gc->roots);
}

/* Create an MPS root for the memory area between START and END, and
   remember it in the root registry of global_igc.  This is called from
   mem_insert.  */

struct igc_root *
igc_mem_add_root (void *start, void *end)
{
  mps_root_t root;
  mps_res_t res
    = mps_root_create_area (&root, global_igc->arena, mps_rank_ambig (), 0, start,
			    end, igc_scan_mem_area, NULL);
  if (res != MPS_RES_OK)
    emacs_abort ();
  return igc_register_root (global_igc, root);
}

void
igc_mem_remove_root (struct igc_root *r)
{
  igc_remove_root (r);
}

/* Add a root for staticvec.  */

static void
igc_add_staticvec_root (struct igc *gc)
{
  mps_root_t root;
  mps_res_t res
    = mps_root_create_area (&root, gc->arena, mps_rank_ambig (), 0,
			    staticvec,
			    staticvec + ARRAYELTS (staticvec),
			    igc_scan_staticvec, NULL);
  if (res != MPS_RES_OK)
    emacs_abort ();
  igc_register_root (gc, root);
}

static void
igc_add_static_roots (struct igc *gc)
{
  igc_add_staticvec_root (gc);
}

/***********************************************************************
				Scanning
 ***********************************************************************/

/* Fix a Lisp_Object at *P.  SS ist the MPS scan state.  */

# define IGC_FIX_LISP_OBJ(ss, p)                     \
   if (!IGC_FIXNUMP (*(p)))                          \
     {                                               \
       EMACS_INT untagged_ = IGC_UNTAGGED (*(p));    \
       mps_addr_t addr_ = (mps_addr_t) untagged_;    \
       if (MPS_FIX1 ((ss), addr_))                   \
	 {                                           \
	   mps_res_t res_ = MPS_FIX2 ((ss), &addr_); \
	   if (res_ != MPS_RES_OK)                   \
	     return res_;                            \
	   EMACS_INT tag_ = IGC_TAG (*(p));          \
	   *(p) = IGC_MAKE_LISP_OBJ (addr_, tag_);   \
	 }                                           \
     }                                               \
   else

/* Scan a memory area at [START, END). SS is the MPS scan state.
   CLOSURE is ignored.  */

static mps_res_t
igc_scan_mem_area (mps_ss_t ss, void *start, void *end, void *closure)
{
  MPS_SCAN_BEGIN (ss)
  {
    for (Lisp_Object *p = start; p < (Lisp_Object *) end; ++p)
      IGC_FIX_LISP_OBJ (ss, p);
  }
  MPS_SCAN_END (ss);
  return MPS_RES_OK;
}

/* Scan staticvec in the interval [START, END). SS is the MPS scan
   state.  CLOSURE is ignored.  */

static mps_res_t
igc_scan_staticvec (mps_ss_t ss, void *start, void *end, void *closure)
{
  MPS_SCAN_BEGIN (ss)
  {
    for (Lisp_Object **p = start; p < (Lisp_Object **) end; ++p)
      IGC_FIX_LISP_OBJ (ss, *p);
  }
  MPS_SCAN_END (ss);
  return MPS_RES_OK;
}

/* Scan a Lisp_Cons.  */

static mps_res_t
igc_cons_scan (mps_ss_t ss, mps_addr_t base, mps_addr_t limit)
{
  MPS_SCAN_BEGIN (ss)
  {
    struct Lisp_Cons *cons = (struct Lisp_Cons *) base;
    IGC_FIX_LISP_OBJ (ss, &cons->u.s.car);
    IGC_FIX_LISP_OBJ (ss, &cons->u.s.u.cdr);
  }
  MPS_SCAN_END (ss);
  return MPS_RES_OK;
}

static mps_addr_t
igc_cons_skip (mps_addr_t addr)
{
  const struct Lisp_Cons *cons = addr;
  return (mps_addr_t) (cons + 1);
}

static void
igc_cons_fwd (mps_addr_t old, mps_addr_t new)
{
  // unclear
}

static mps_addr_t
igc_cons_isfwd (mps_addr_t addr)
{
  return NULL;
}

static void
igc_cons_pad (mps_addr_t addr, size_t size)
{
}

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
  if (res != MPS_RES_OK)
    emacs_abort ();

  // Generations
  mps_gen_param_s gen_params[]
    = { { 32000, 0.8 }, { 5 * 32009, 0.4 } };
  res = mps_chain_create (&gc->chain, gc->arena, ARRAYELTS (gen_params),
			  gen_params);
  if (res != MPS_RES_OK)
    emacs_abort ();

  // Object format for conses.
  MPS_ARGS_BEGIN (args)
  {
    MPS_ARGS_ADD (args, MPS_KEY_FMT_ALIGN, 8);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_HEADER_SIZE, 0);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_SCAN, igc_cons_scan);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_SKIP, igc_cons_skip);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_FWD, igc_cons_fwd);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_ISFWD, igc_cons_isfwd);
    MPS_ARGS_ADD (args, MPS_KEY_FMT_PAD, igc_cons_pad);
    res = mps_fmt_create_k (&gc->cons_fmt, gc->arena, args);
  }
  MPS_ARGS_END (args);
  if (res != MPS_RES_OK)
    emacs_abort ();

  // Pool for conses. Since conses have no type field which would let
  // us recognize them when mixed with other objects, use a dedicated
  // pool.
  MPS_ARGS_BEGIN (args)
  {
    MPS_ARGS_ADD (args, MPS_KEY_FORMAT, gc->cons_fmt);
    MPS_ARGS_ADD (args, MPS_KEY_CHAIN, gc->chain);
    MPS_ARGS_ADD (args, MPS_KEY_INTERIOR, 0);
    res = mps_pool_create_k (&gc->cons_pool, gc->arena,
			     mps_class_ams (), args);
  }
  MPS_ARGS_END (args);
  if (res != MPS_RES_OK)
    emacs_abort ();

  // Add staticpro roots. For now, as ambigous references.
  igc_add_static_roots (gc);

  return gc;
}

static void
free_igc (struct igc *gc)
{
  mps_pool_destroy (gc->cons_pool);
  mps_fmt_destroy (gc->cons_fmt);
  igc_remove_all_roots (gc);
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
}

#endif // HAVE_MPS
