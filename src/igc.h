// clang-format off

#ifndef EMACS_IGC_H
#define EMACS_IGC_H

# ifdef HAVE_MPS

extern void init_igc (void);
extern void syms_of_igc (void);

extern void *igc_on_mem_insert (void *start, void *end);
extern void igc_on_mem_delete (void *info);

extern void *igc_thread_add (const void *cold);
extern void igc_thread_remove (void *info);

extern void igc_on_alloc_main_thread_specpdl (void);
extern void igc_on_grow_specpdl (void);
extern void igc_on_specbinding_unused (union specbinding *b);
extern void igc_on_idle (void);
extern void igc_on_old_gc (void);
extern void igc_on_pdump_loaded (void);
extern void igc_on_make_face_cache (void *face_cache);
extern void igc_on_free_face_cache (void *face_cache);
extern void igc_on_face_cache_change (void *face_cache);
extern void igc_on_adjust_glyph_matrix (void *matrix);
extern void igc_on_free_glyph_matrix (void *matrix);
extern void igc_handle_messages (void);

#define IGC_MANAGE_CONS 1

extern Lisp_Object igc_make_cons (Lisp_Object car, Lisp_Object cdr);

# endif // HAVE_MPS

#endif // EMACS_IGC_H
