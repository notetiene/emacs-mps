/* Android initialization for GNU Emacs.

Copyright (C) 2023 Free Software Foundation, Inc.

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
along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.  */

/* On Android, Emacs is built as a shared library loaded from Java
   using the Java Native Interface.  Emacs's `main' function is
   renamed `android_emacs_init', and runs with some modifications
   inside a separate thread, communicating with the Java code through
   a table of function pointers.  */

#ifndef _ANDROID_H_
#ifndef ANDROID_STUBIFY
#include <jni.h>
#include <pwd.h>

#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>

#include <android/bitmap.h>

#include "androidgui.h"
#include "lisp.h"
#endif

/* This must be used in every symbol declaration to export it to the
   JNI Emacs wrapper.  */
#define ANDROID_EXPORT __attribute__ ((visibility ("default")))

extern bool ANDROID_EXPORT android_init_gui;
extern int ANDROID_EXPORT android_emacs_init (int, char **);

#ifndef ANDROID_STUBIFY

extern int android_select (int, fd_set *, fd_set *, fd_set *,
			   struct timespec *, const sigset_t *);

extern bool android_file_access_p (const char *, int);
extern int android_open (const char *, int, int);
extern char *android_user_full_name (struct passwd *);
extern int android_fstat (int, struct stat *);
extern int android_fstatat (int, const char *restrict,
			    struct stat *restrict, int);
extern int android_close (int);
extern int android_fclose (FILE *);
extern const char *android_get_home_directory (void);

extern double android_pixel_density_x, android_pixel_density_y;

enum android_handle_type
  {
    ANDROID_HANDLE_WINDOW,
    ANDROID_HANDLE_GCONTEXT,
    ANDROID_HANDLE_PIXMAP,
  };

extern jobject android_resolve_handle (android_handle,
				       enum android_handle_type);
extern unsigned char *android_lock_bitmap (android_window,
					   AndroidBitmapInfo *,
					   jobject *);
extern void android_damage_window (android_window,
				   struct android_rectangle *);
extern int android_get_screen_width (void);
extern int android_get_screen_height (void);
extern int android_get_mm_width (void);
extern int android_get_mm_height (void);
extern bool android_detect_mouse (void);

extern void android_set_dont_focus_on_map (android_window, bool);
extern void android_set_dont_accept_focus (android_window, bool);

extern jstring android_build_string (Lisp_Object);
extern void android_exception_check (void);

extern void android_get_keysym_name (int, char *, size_t);



/* Directory listing emulation.  */

struct android_dir;

extern struct android_dir *android_opendir (const char *);
extern struct dirent *android_readdir (struct android_dir *);
extern void android_closedir (struct android_dir *);



#endif

/* JNI functions should not be built when Emacs is stubbed out for the
   build.  These should be documented in EmacsNative.java.  */

#ifndef ANDROID_STUBIFY
#include <jni.h>

extern JNIEnv *android_java_env;

#define ANDROID_DELETE_LOCAL_REF(ref)				\
  ((*android_java_env)->DeleteLocalRef (android_java_env,	\
					(ref)))

#define NATIVE_NAME(name) Java_org_gnu_emacs_EmacsNative_##name

#endif
#endif /* _ANDROID_H_ */