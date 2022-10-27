;;; pkg.el --- Lisp packages -*- lexical-binding: t -*-

;; Copyright (C) 2022 Free Software Foundation, Inc.

;; Author: Gerd Möllmann <gerd@gnu.org>
;; Keywords: lisp, tools, maint
;; Version: 1.0

;; This file is part of GNU Emacs.

;; GNU Emacs is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; GNU Emacs is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with GNU Emacs.  If not, see <https://www.gnu.org/licenses/>.

;;; Commentary:

;; This file is part of the implementation of Lisp packages for Emacs.
;; Code is partly adapted from CMUCL, which is in the public domain.

;; The implementation strives to do as much as possible in Lisp, not
;; C.  C functions with names like 'package-%...' are defined which
;; allow low-level access to the guts of Lisp_Package objects.
;; Several variables are exposed from C that allow manipulating
;; internal state.

;; All that is dangerous :-).

;;; Code:

(require 'cl-lib)
(require 'cl-macs)
(require 'gv)

;;; Define setters for internal package details.
(gv-define-simple-setter package-%name package-%set-name)
(gv-define-simple-setter package-%nicknames package-%set-nicknames)
(gv-define-simple-setter package-%use-list package-%set-use-list)
(gv-define-simple-setter package-%shadowing-symbols
                         package-%set-shadowing-symbols)


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;                               Helpers
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

(defun pkg-check-disjoint (&rest args)
  "Check whether all given arguments specify disjoint sets of symbols.
Each argument is of the form (:key . set)."
  (cl-loop for (current-arg . rest-args) on args
           do
	   (cl-loop with (key1 . set1) = current-arg
	            for (key2 . set2) in rest-args
	            for common = (cl-delete-duplicates
	                          (cl-intersection set1 set2 :test #'string=))
	            unless (null common)
	            do
	            (error "Parameters %s and %s must be disjoint \
but have common elements %s" key1 key2 common))))

(defun pkg--stringify-name (name kind)
  "Return a string for string designator NAME.
If NAME is a string, return that.
If NAME is a symbol, return its symbol name.
If NAME is a character, return what `char-to-string' returns.
KIND is the kind of name we are processing, for error messages."
  (cl-typecase name
    (string name)
    (symbol (cl-symbol-name name))
    (base-char (char-to-string name))
    (t (error "Bogus %s: %s" kind name))))

(defun pkg--stringify-names (names kind)
  "Transform a list of string designators to a list of strings.
Duplicates are removed from the result list."
  (cl-remove-duplicates
   (mapcar #'(lambda (name) (pkg--stringify-name name kind)) names)
   :test #'equal))

(defun pkg-package-namify (n)
  "Return N as a package name."
  (pkg--stringify-name n "package"))

(defun pkg-find-package (name)
  "Return the package with NAME in the package registry.
Value is nil if no package is found."
  (gethash name *package-registry* nil))

(defun pkg--symbol-listify (thing)
  "Return a list of symbols for THING.
If THING is a list, check that all elements of the list are
symbols, and return THING.
If THING is a symbol, return a list that contains THING only.
Otherwise, signal an error."
  (cond ((listp thing)
	 (dolist (s thing)
	   (unless (symbolp s)
             (error "%s is not a symbol" s)))
	 thing)
	((symbolp thing)
         (list thing))
	(t
	 (error "%s is neither a symbol nor a list of symbols" thing))))

(cl-defun pkg--find-or-make-package (name)
  "Find or make a package named NAME.
If NAME is a package object, return that.  Otherwise, if NAME can
be found with `find-package' return that.  Otherwise, make a new
package with name NAME."
  (cond ((packagep name)
         (unless (package-%name name)
           (error "Can't do anything with deleted package: %s" name))
         name)
        (t
         (let* ((name (pkg--stringify-name name "package name")))
           (or (pkg-find-package name)
	       (make-package name))))))

(defun pkg--packages-from-names (names)
  "Return a list of packages object for NAMES.
NAMES must be a list of package objects or valid package names."
  (mapcar #'(lambda (name) (pkg--find-or-make-package name))
          names))

(defun pkg--listify-packages (packages)
  "Return a list of packages for PACKAGES.
If PACKAGES is not a list, make it a list.  Then, find or make
packages for packages named in the list and return the result."
  (let ((packages (if (listp packages) packages (list packages))))
    (cl-remove-duplicates (mapcar #'pkg--find-or-make-package
                                  packages))))

(defun pkg--package-or-lose (name)
  "Return the package denoted by NAME.
If NAME is a package, return that.
Otherwise, NAME must be the name of a registered package."
  (if (packagep name)
      name
    (let ((pkg-name (pkg--stringify-name name "package")))
      (or (find-package pkg-name)
          (error "No package %s found" name)))))

(cl-defun pkg--remove-from-registry (package)
  "Remove PACKAGE from the package registry."
  ;; Note that an unregistered package might have the same name or
  ;; nickname as a registered package.  Prevent deleting such a
  ;; package from unregistering some other package.
  (let ((names ()))
    (maphash (lambda (n p)
               (when (eq p package)
                 (push n names)))
             *package-registry*)
    (dolist (n names)
      (remhash n *package-registry*))))

(defun pkg--package-or-default (package)
  "Return the package object denoted by PACKAGE.
If PACKAGE is a package object, return that.
If PACKAGE is nil, return the current package.
Otherwise assume that "
  (cond ((packagep package) package)
        ((null package) *package*)
        (t (pkg--package-or-lose package))))


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;                                  Macros
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;;###autoload
(cl-defmacro do-symbols ((var &optional (package '*package*) result-form)
			 &body body)
  "Loop over symbols in a package.

Evaluate BODY with VAR bound to each symbol accessible in the given
PACKAGE, or the current package if PACKAGE is not specified.

Return what RESULT-FORM evaluates to, if specified, and the loop ends
normally, or else if an explcit return occurs the value it transfers."
  (declare (indent 1))
  (cl-with-gensyms (flet-name)
    `(cl-block nil
       (cl-flet ((,flet-name (,var)
		   (cl-tagbody ,@body)))
	 (let* ((package (pkg--package-or-lose ,package)))
	   (maphash (lambda (k _v) (,flet-name k))
		    (package-%symbols package))
	   (dolist (p (package-%use-list package))
	     (maphash (lambda (k v)
			(when (eq v :external)
			  (,flet-name k)))
		      (package-%symbols p)))))
       (let ((,var nil))
         ,var
	 ,result-form))))

;;;###autoload
(cl-defmacro do-external-symbols ((var &optional (package '*package*) result-form)
			          &body body)
  "Loop over external symbols in a package.

Evaluate BODY with VAR bound to each symbol accessible in the given
PACKAGE, or the current package if PACKAGE is not specified.

Return what RESULT-FORM evaluates to, if specified, and the loop ends
normally, or else if an explcit return occurs the value it transfers."
  (cl-with-gensyms (flet-name)
    `(cl-block nil
       (cl-flet ((,flet-name (,var)
		   (cl-tagbody ,@body)))
	 (let* ((package (pkg--package-or-lose ,package))
		(shadows (package-%shadowing-symbols package)))
	   (maphash (lambda (k v)
		      (when (eq v :external)
			(,flet-name k)))
		    (package-%symbols package))))
       (let ((,var nil))
	 ,result-form))))

;;;###autoload
(cl-defmacro do-all-symbols ((var &optional result-form) &body body)
  "Loop over all symbols in all registered packages.

Evaluate BODY with VAR bound to each symbol accessible in the given
PACKAGE, or the current package if PACKAGE is not specified.

Return what RESULT-FORM evaluates to, if specified, and the loop ends
normally, or else if an explcit return occurs the value it transfers."
  (cl-with-gensyms (flet-name)
    `(cl-block nil
       (cl-flet ((,flet-name (,var)
		   (cl-tagbody ,@body)))
         (dolist (package (list-all-packages))
	   (maphash (lambda (k _v)
		      (,flet-name k))
		    (package-%symbols package))))
       (let ((,var nil))
	 ,result-form))))


;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;                        Basic stuff
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;;;###autoload
(cl-defun make-package (name &key nicknames use (size 10)
                             (register nil))
  "Create and return a new package with name NAME.

NAME must be a string designator, that is a string, a symbol, or
a character.  If it is a symbol, the symbol's name will be used
as package name.  If a character, the character's string
representation will be used (`char-to-string').

NICKNAMES specifies a list of string designators for additional
names which may be used to refer to the package.  Default is nil.

USE specifies zero or more packages the external symbols of which
are to be inherited by the package.  See also function
`use-package'.  All packages in the use-list must be either
package objects or they are looked up in the package registry
with `find-package'.  If they are not found, a new package with
the given name is created.

SIZE gives the size to use for the symbol table of the new
package.  Default is 10.

REGISTER if true means register the package in the package
registry.

Please note that the newly created package is not automaticall
registered in the package registry, that is it will not be found
under its names by `find-package'.  Use `register-package' to
register the package.  This deviates from the CLHS specification,
but is what Common Lisp implementations usually do."
  (cl-check-type size natnum)
  (let* ((name (pkg--stringify-name name "package name"))
         (nicknames (pkg--stringify-names nicknames "package nickname"))
         (use (pkg--packages-from-names use))
         (package (make-%package name size)))
    (setf (package-%nicknames package) nicknames
          (package-%use-list package) use)
    (when register
      (register-package package))
    package))

(defun register-package (package)
  "Register PACKAGE in the package registry.
Signal an error if the name or one of the nicknames of PACKAGE
conflicts with a name already present in the registry.
Value is PACKAGE."
  (let ((package (pkg--package-or-lose package)))
    (cl-flet ((check (name)
                (when (gethash name *package-registry*)
                  (error "%s conflicts with existing package" name))))
      (check (package-%name package))
      (mapc #'check (package-%nicknames package))
      (puthash (package-%name package) package *package-registry*)
      (mapc (lambda (name) (puthash name package *package-registry*))
            (package-%nicknames package))
      package)))

;;;###autoload
(defun list-all-packages ()
  "Return a fresh list of all registered packages."
  (let ((all ()))
    (maphash (lambda (_ p) (push p all)) *package-registry*)
    (cl-remove-duplicates all)))

;;;###autoload
(defun package-name (package)
  "Return the name of PACKAGE.
If PACKAGE is not a package object already, it must the name of a
registered package."
  (package-%name (pkg--package-or-lose package)))

;;;###autoload
(defun package-nicknames (package)
  "Return the list of nickname strings of PACKAGE.
If PACKAGE is not a package object already, it must the name of a
registered package."
  (package-%nicknames (pkg--package-or-lose package)))

;;;###autoload
(defun package-shadowing-symbols (package)
  "Return the list of shadowing symbols of PACKAGE.
If PACKAGE is not a package object already, it must the name of a
registered package."
  (package-%shadowing-symbols (pkg--package-or-lose package)))

;;;###autoload
(defun package-use-list (package)
  (package-%use-list (pkg--package-or-lose package)))

;;;###autoload
(defun package-used-by-list (package)
  "Return a list of packages using PACKAGE."
  (let ((package (pkg--package-or-lose package))
        (used-by ()))
    (dolist (p (list-all-packages))
      (when (memq package (package-%use-list p))
        (cl-pushnew p used-by)))
    used-by))

;;;###autoload
(defun find-package (package)
  "Find and return the package for PACKAGE.
If PACKAGE is a package object, return that.

Otherwise, PACKAGE must be a package name, and that name
is lookup up in the package registry and the result is
returned if found.

Value is nil if no package with the given name is found. "
  (if (packagep package)
      package
    (let ((name (pkg--stringify-name package "package name")))
      (gethash name *package-registry*))))

;;;###autoload
(defun delete-package (package)
  "Delete PACKAGE.

If PACKAGE is an already deleted package, return nil.

If PACKAGE is a package that is not already deleted, or PACKAGE
is a package name that is registered, delete that package by
removing it from the package registry, and return t.

After this operation completes, the home package of any symbol
whose home package had previously been package is set to nil.
That is, these symbols are now considered uninterned symbols.

An attempt to delete one of the standard packages results in an
error."
  (if (and (packagep package)
           (null (package-%name package)))
      nil
    (let ((package (pkg--package-or-lose package)))
      (when (or (eq package *emacs-package*)
                (eq package *keyword-package*))
        (error "Cannot delete a standard package"))
      (pkg--remove-from-registry package)
      (setf (package-%name package) nil)
      (do-symbols (sym package)
        (when (eq (symbol-package sym) package)
          (package-%set-symbol-package sym nil)))
      t)))

;;;###autoload
(defun rename-package (package new-name &optional new-nicknames)
  "Replace name and nicknames of PACKAGE with NEW-NAME and NEW-NICKNAMES.

PACKAGE must be a package object, or name a registered package.
Deleted packages cannot be renamed.

NEW-NAME must be a valid package name, a string, symbol, or
character.

Optional NEW-NICKSNAMES must be a list of valid package names.

Value is the renamed package object."
  (let ((package (pkg--package-or-lose package))
        (new-name (pkg--stringify-name new-name "package name"))
        (new-nicknames (pkg--stringify-names new-nicknames
                                             "package nickname")))
    (unless (package-%name package)
      (error "Package is deleted"))
    (pkg--remove-from-registry package)
    (setf (package-%nicknames package) new-nicknames)
    (setf (package-%name package) new-name)
    (register-package package)
    package))

;;;###autoload
(defun export (symbols &optional package)
  "tbd"
  (let ((symbols (pkg--symbol-listify symbols))
        (package (pkg--package-or-default package))
        (syms ()))

    ;; Ignore any symbols that are already external.
    (dolist (sym symbols)
      (cl-multiple-value-bind (_s status)
	  (find-symbol (cl-symbol-name sym) package)
        (unless (or (eq :external status)
                    (memq sym syms))
          (push sym syms))))

    ;; Find symbols and packages with conflicts.
    (let ((used-by (package-used-by-list package))
	  (cpackages ())
	  (cset ()))
      (dolist (sym syms)
	(let ((name (cl-symbol-name sym)))
	  (dolist (p used-by)
	    (cl-multiple-value-bind (s w)
                (find-symbol name p)
	      (when (and w (not (eq s sym))
			 (not (member s (package-%shadowing-symbols p))))
		(cl-pushnew sym cset)
		(cl-pushnew p cpackages))))))

      (when cset
	(error "Exporting these symbols from the %s package: %s
 results in name conflicts with these packages: %s"
	       (package-name package)
               cset
	       (mapcar #'package-name cpackages))))

    ;; Check that all symbols are accessible.
    (let ((missing ())
	  (imports ()))
      (dolist (sym syms)
	(cl-multiple-value-bind (s w)
            (find-symbol (cl-symbol-name sym) package)
	  (cond ((not (and w (eq s sym)))
                 (push sym missing))
		((eq w :inherited)
                 (push sym imports)))))
      (when missing
	(error "These symbols are not accessible in the %s package: %s"
               (package-%name package)
               missing))

      ;; Import
      (import imports package))

    ;; And now, three pages later, we export the suckers.
    (dolist (sym syms)
      (package-%set-status sym package :external))
    t))


;;;###autoload
(defun unexport (_symbols &optional package)
  (setq package (pkg--package-or-default package))
  (error "not yet implemented"))

;;;###autoload
(defun import (symbols &optional package)
  (let ((package (pkg--package-or-default package))
        (symbols (pkg--symbol-listify symbols)))
    (list package symbols)))

;;;###autoload
(defun shadow (symbols &optional package)
  "Make an internal symbol in PACKAGE with the same name as each of the
  specified SYMBOLS, adding the new symbols to the Package-Shadowing-Symbols.
  If a symbol with the given name is already present in PACKAGE, then
  the existing symbol is placed in the shadowing symbols list if it is
  not already present."
  (let* ((package (pkg--package-or-lose package)))
    (dolist (name (mapcar #'string
			  (if (listp symbols) symbols (list symbols))))
      (cl-multiple-value-bind (sym status) (find-symbol name package)
	(when (or (not status) (eq status :inherited))
	  (setq sym (make-symbol name))
	  (package-%set-symbol-package sym package)
          (puthash sym :internal (package-%symbols package)))
	(cl-pushnew sym (package-%shadowing-symbols package)))))
  t)


;;;###autoload
(defun shadowing-import (_symbols &optional package)
  (setq package (pkg--package-or-default package))
  (error "not yet implemented"))

;;;###autoload
(defun use-package (use &optional package)
  (let* ((package (pkg--package-or-default package))
         (use (pkg--listify-packages use)))
    (setf (package-%use-list package)
          (cl-union (package-%use-list package)
                    use))
    t))

;;;###autoload
(defun unuse-package (unuse &optional package)
  (let* ((package (pkg--package-or-default package))
         (unuse (pkg--listify-packages unuse)))
    (setf (package-%use-list package)
          (cl-intersection (package-%use-list package)
                           unuse))
    t))

;;;###autoload
(defun in-package* (package)
  "Switch to PACKAGE with completion."
  (interactive (list (completing-read "Package to switch to: "
                                      *package-registry*
                                      nil t)))
  (let ((package (pkg--package-or-lose package)))
    (setf *package* package)))

;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;                            defpackage
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

;; (defun pkg--enter-new-nicknames (package nicknames)
;;   (cl-check-type nicknames list)
;;   (dolist (n nicknames)
;;     (let* ((n (pkg-package-namify n))
;; 	   (found (pkg-name-to-package n)))
;;       (cond ((not found)
;; 	     (setf (gethash n *package-registry*) package)
;; 	     (push n (package-%nicknames package)))
;; 	    ((eq found package))
;; 	    ((string= (package-name found) n)
;; 	     (error "%s is a package name, so it cannot be a nickname for %s."
;; 	            n (package-name package)))
;; 	    (t
;; 	     (error "%s is already a nickname for %s"
;;                     n (package-name found)))))))

;; (defun pkg-defpackage (name nicknames size shadows shadowing-imports
;; 			    use imports interns exports doc-string)
;;   (let ((package (find-package name)))
;;     (unless package
;;       (setq package (make-package name :use nil :size (or size 10))))
;;     (unless (string= (package-name package) name)
;;       (error "%s is a nickname for the package %s"
;;              name (package-name package)))

;;     Nicknames
;;     (pkg--enter-new-nicknames package nicknames)

;;     Shadows and Shadowing-imports.
;;     (let ((old-shadows (package-%shadowing-symbols package)))
;;       (shadow shadows package)
;;       (dolist (sym-name shadows)
;; 	(setf old-shadows (remove (find-symbol sym-name package) old-shadows)))
;;       (dolist (simports-from shadowing-imports)
;; 	(let ((other-package (package-or-lose (car simports-from))))
;; 	  (dolist (sym-name (cdr simports-from))
;; 	    (let ((sym (find-or-make-symbol sym-name other-package)))
;; 	      (shadowing-import sym package)
;; 	      (setf old-shadows (remove sym old-shadows))))))
;;       (when old-shadows
;; 	(warn "%s also shadows the following symbols: %s"
;; 	      name old-shadows)))

;;     Use
;;     (let ((old-use-list (package-use-list package))
;; 	  (new-use-list (mapcar #'package-or-lose use)))
;;       (use-package (cl-set-difference new-use-list old-use-list) package)
;;       (let ((laterize (cl-set-difference old-use-list new-use-list)))
;; 	(when laterize
;; 	  (unuse-package laterize package)
;; 	    (warn "%s previously used the following packages: %s"
;; 		  name laterize))))

;;     Import and Intern.
;;     (dolist (sym-name interns)
;;       (intern sym-name package))
;;     (dolist (imports-from imports)
;;       (let ((other-package (package-or-lose (car imports-from))))
;; 	(dolist (sym-name (cdr imports-from))
;; 	  (import (list (find-or-make-symbol sym-name other-package))
;; 		  package))))

;;     Exports.
;;     (let ((old-exports nil)
;; 	  (exports (mapcar (lambda (sym-name) (intern sym-name package)) exports)))
;;       (do-external-symbols (sym package)
;; 	 (push sym old-exports))
;;       (export exports package)
;;       (let ((diff (cl-set-difference old-exports exports)))
;; 	(when diff
;; 	  (warn "%s also exports the following symbols: %s" name diff))))

;;     Documentation
;;     (setf (package-doc-string package) doc-string)
;;     package))



;; (defmacro defpackage (package &rest options)
;;   "Defines a new package called PACKAGE.  Each of OPTIONS should be one of the
;;    following:
;;      (:NICKNAMES {package-name}*)
;;      (:SIZE <integer>)
;;      (:SHADOW {symbol-name}*)
;;      (:SHADOWING-IMPORT-FROM <package-name> {symbol-name}*)
;;      (:USE {package-name}*)
;;      (:IMPORT-FROM <package-name> {symbol-name}*)
;;      (:INTERN {symbol-name}*)
;;      (:EXPORT {symbol-name}*)
;;      (:DOCUMENTATION doc-string)
;;    All options except :SIZE and :DOCUMENTATION can be used multiple times."
;;   (let ((nicknames nil)
;; 	(size nil)
;; 	(shadows nil)
;; 	(shadowing-imports nil)
;; 	(use nil)
;; 	(use-p nil)
;; 	(imports nil)
;; 	(interns nil)
;; 	(exports nil)
;; 	(doc nil))
;;     (dolist (option options)
;;       (unless (consp option)
;; 	(error "Bogus DEFPACKAGE option: %s" option))
;;       (cl-case (car option)
;; 	(:nicknames
;; 	 (setf nicknames (pkg--stringify-names (cdr option) "package")))
;; 	(:size
;; 	 (cond (size
;; 		(error "Can't specify :SIZE twice."))
;; 	       ((and (consp (cdr option))
;; 		     (cl-typep (cl-second option) 'natnum))
;; 		(setf size (cl-second option)))
;; 	       (t
;; 		(error "Bogus :SIZE, must be a positive integer: %s"
;;                        (cl-second option)))))
;; 	(:shadow
;; 	 (let ((new (pkg--stringify-names (cdr option) "symbol")))
;; 	   (setf shadows (append shadows new))))
;; 	(:shadowing-import-from
;; 	 (let ((package-name (pkg--stringify-name (cl-second option) "package"))
;; 	       (names (pkg--stringify-names (cddr option) "symbol")))
;; 	   (let ((assoc (cl-assoc package-name shadowing-imports
;; 			          :test #'string=)))
;; 	     (if assoc
;; 		 (setf (cdr assoc) (append (cdr assoc) names))
;; 	       (setf shadowing-imports
;; 		     (cl-acons package-name names shadowing-imports))))))
;; 	(:use
;; 	 (let ((new (pkg--stringify-names (cdr option) "package")))
;; 	   (setf use (cl-delete-duplicates (nconc use new) :test #'string=))
;; 	   (setf use-p t)))
;; 	(:import-from
;; 	 (let ((package-name (pkg--stringify-name (cl-second option) "package"))
;; 	       (names (pkg--stringify-names (cddr option) "symbol")))
;; 	   (let ((assoc (cl-assoc package-name imports
;; 			          :test #'string=)))
;; 	     (if assoc
;; 		 (setf (cdr assoc) (append (cdr assoc) names))
;; 	       (setf imports (cl-acons package-name names imports))))))
;; 	(:intern
;; 	 (let ((new (pkg--stringify-names (cdr option) "symbol")))
;; 	   (setf interns (append interns new))))
;; 	(:export
;; 	 (let ((new (pkg--stringify-names (cdr option) "symbol")))
;; 	   (setf exports (append exports new))))
;; 	(:documentation
;; 	 (when doc
;; 	   (error "Can't specify :DOCUMENTATION twice."))
;; 	 (setf doc (cl-coerce (cl-second option) 'string)))
;; 	(t
;; 	 (error "Bogus DEFPACKAGE option: %s" option))))
;;     (pkg-check-disjoint `(:intern ,@interns) `(:export  ,@exports))
;;     (pkg-check-disjoint `(:intern ,@interns)
;; 		        `(:import-from ,@(apply 'append (mapcar 'cl-rest imports)))
;; 		        `(:shadow ,@shadows)
;; 		        `(:shadowing-import-from
;;                           ,@(apply 'append (mapcar 'cl-rest shadowing-imports))))
;;     `(cl-eval-when (compile load eval)
;;        (pkg-defpackage ,(pkg--stringify-name package "package") ',nicknames ',size
;; 		       ',shadows ',shadowing-imports ',(if use-p use :default)
;; 		       ',imports ',interns ',exports ',doc))))

(provide 'pkg)

;;; pkg.el ends here
