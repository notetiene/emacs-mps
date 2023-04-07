/* Communication module for Android terminals.  -*- c-file-style: "GNU" -*-

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

package org.gnu.emacs;

import java.lang.Thread;
import java.util.Arrays;

import android.os.Build;
import android.util.Log;

public class EmacsThread extends Thread
{
  private static final String TAG = "EmacsThread";

  /* Whether or not Emacs should be started -Q.  */
  private boolean startDashQ;

  /* Runnable run to initialize Emacs.  */
  private Runnable paramsClosure;

  /* Whether or not to open a file after starting Emacs.  */
  private String fileToOpen;

  public
  EmacsThread (EmacsService service, Runnable paramsClosure,
	       boolean startDashQ, String fileToOpen)
  {
    super ("Emacs main thread");
    this.startDashQ = startDashQ;
    this.paramsClosure = paramsClosure;
    this.fileToOpen = fileToOpen;
  }

  @Override
  public void
  run ()
  {
    String args[];

    if (fileToOpen == null)
      {
	if (!startDashQ)
	  args = new String[] { "libandroid-emacs.so", };
	else
	  args = new String[] { "libandroid-emacs.so", "-Q", };
      }
    else
      {
	if (!startDashQ)
	  args = new String[] { "libandroid-emacs.so",
				fileToOpen, };
	else
	  args = new String[] { "libandroid-emacs.so", "-Q",
				fileToOpen, };
      }

    paramsClosure.run ();

    /* Run the native code now.  */
    Log.d (TAG, "run: " + Arrays.toString (args));
    EmacsNative.initEmacs (args, EmacsApplication.dumpFileName,
			   Build.VERSION.SDK_INT);
  }
};