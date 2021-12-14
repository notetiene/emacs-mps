;;; fcr.el --- FunCallableRecords       -*- lexical-binding: t; -*-

;; Copyright (C) 2015, 2021  Stefan Monnier

;; Author: Stefan Monnier <monnier@iro.umontreal.ca>
;; Version: 0

;; This program is free software; you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; This program is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with this program.  If not, see <http://www.gnu.org/licenses/>.

;;; Commentary:

;; A FunCallableRecord is an object that combines the properties of records
;; with those of a function.  More specifically it is a function extended
;; with a notion of type (e.g. for defmethod dispatch) as well as the
;; ability to have some fields that are accessible from the outside.

;; Here are some cases of "callable objects" where FCRs might be useful:
;; - nadvice.el
;; - iterators (generator.el), thunks (thunk.el), streams (stream.el).
;; - kmacros (for cl-print and for `kmacro-extract-lambda')
;; - PEG rules: they're currently just functions, but they should carry
;;   their original (macro-expanded) definition (and should be printed
;;   differently from functions)!
;; - cl-generic: turn `cl--generic-isnot-nnm-p' into a mere type test
;;   (by putting the no-next-methods into their own class).
;; - documented functions: this could be a subtype of normal functions, which
;;   simply has an additional `docstring' slot.
;; - commands: this could be a subtype of documented functions, which simply
;;   has an additional `interactive-form' slot.

;;; Code:

(eval-when-compile (require 'cl-lib))
(eval-when-compile (require 'subr-x))   ;For `named-let'.

(cl-defstruct (fcr--class
               (:constructor nil)
               (:constructor fcr--class-make ( name docstring slots parents
                                               allparents))
               (:include cl--class)
               (:copier nil))
  "Metaclass for FunCallableRecord classes."
  (allparents nil :read-only t :type (list-of symbol)))

(setf (cl--find-class 'fcr-object)
      (fcr--class-make 'fcr-object "The root parent of all FCR classes"
                       nil nil '(fcr-object)))
(defun fcr--object-p (fcr)
  (let ((type (fcr-type fcr)))
    (when type
      (memq 'fcr-object (fcr--class-allparents (cl--find-class type))))))
(cl-deftype fcr-object () '(satisfies fcr--object-p))

(defun fcr--defstruct-make-copiers (copiers slots name)
  (require 'cl-macs)                    ;`cl--arglist-args' is not autoloaded.
  (mapcar
   (lambda (copier)
     (pcase-let*
         ((cname (pop copier))
          (args (or (pop copier) `(&key ,@slots)))
          (doc (or (pop copier)
                   (format "Copier for objects of type `%s'." name)))
          (obj (make-symbol "obj"))
          (absent (make-symbol "absent"))
          (anames (cl--arglist-args args))
          (index -1)
          (argvals
           (mapcar
	    (lambda (slot)
	      (setq index (1+ index))
	      (when (memq slot anames)
		;; FIXME: Skip the `unless' test for mandatory args.
		`(if (eq ',absent ,slot)
		     (fcr-get ,obj ,index)
		   ,slot)))
	    slots)))
       `(cl-defsubst ,cname (&cl-defs (',absent) ,obj ,@args)
          ,doc
          (declare (side-effect-free t))
          (fcr--copy ,obj ,@argvals))))
   copiers))

(defmacro fcr-defstruct (name &optional docstring &rest slots)
  (declare (doc-string 2) (indent 1))
  (unless (stringp docstring)
    (push docstring slots)
    (setq docstring nil))
  (let* ((options (when (consp name)
                    (prog1 (copy-sequence (cdr name))
                      (setq name (car name)))))
         (get-opt (lambda (opt &optional all)
                    (let ((val (assq opt options))
                          tmp)
                      (when val (setq options (delq val options)))
                      (if (not all)
                          (cdr val)
                        (when val
                          (setq val (list (cdr val)))
                          (while (setq tmp (assq opt options))
                            (push (cdr tmp) val)
                            (setq options (delq tmp options)))
                          (nreverse val))))))

         (parent-names (or (or (funcall get-opt :parent)
                               (funcall get-opt :include))
                           '(fcr-object)))
         (copiers (funcall get-opt :copier 'all))

         (parent-slots '())
         (parents
          (mapcar
           (lambda (name)
             (let* ((class (or (cl--find-class name)
                               (error "Unknown parent: %S" name))))
               (setq parent-slots
                     (named-let merge
                         ((slots-a parent-slots)
                          (slots-b (cl--class-slots class)))
                       (cond
                        ((null slots-a) slots-b)
                        ((null slots-b) slots-a)
                        (t
                         (let ((sa (car slots-a))
                               (sb (car slots-b)))
                           (unless (equal sa sb)
                             (error "Slot %s of %s conflicts with slot %s of previous parent"
                                    (cl--slot-descriptor-name sb)
                                    name
                                    (cl--slot-descriptor-name sa)))
                           (cons sa (merge (cdr slots-a) (cdr slots-b))))))))
               class))
           parent-names))
         (slotdescs (append
                     parent-slots
                     ;; FIXME: Catch duplicate slot names.
                     (mapcar (lambda (field)
                               (cl--make-slot-descriptor field nil nil
                                                         '((:read-only . t))))
                             slots)))
         (allparents (apply #'append (mapcar #'cl--class-allparents
                                             parents)))
         (class (fcr--class-make name docstring slotdescs parents
                                 (delete-dups
                                  (cons name allparents)))))
    ;; FIXME: Use an intermediate function like `cl-struct-define'.
    `(progn
       ,(when options (macroexp-warn-and-return
                       (format "Ignored options: %S" options)
                       nil))
       (eval-and-compile
         (fcr--define ',class
                      (lambda (fcr)
                        (let ((type (fcr-type fcr)))
                          (when type
                            (memq ',name (fcr--class-allparents
                                          (cl--find-class type))))))))
       ,@(let ((i -1))
           (mapcar (lambda (desc)
                     (let ((slot (cl--slot-descriptor-name desc)))
                       (cl-incf i)
                       ;; Always use a double hyphen: if the user wants to
                       ;; make it public, it can do so with an alias.
                       `(defun ,(intern (format "%S--%S" name slot)) (fcr)
                        ,(format "Return slot `%S' of FCR, of type `%S'."
                                 slot name)
                        (fcr-get fcr ,i))))
                   slotdescs))
       ,@(fcr--defstruct-make-copiers copiers slots name))))

(defun fcr--define (class pred)
  (let* ((name (cl--class-name class))
         (predname (intern (format "fcr--%s-p" name)))
         (type `(satisfies ,predname)))
    (setf (cl--find-class name) class)
    (defalias predname pred)
    (put name 'cl-deftype-handler (lambda () type))))

(defmacro fcr-make (type fields args &rest body)
  (declare (indent 3) (debug (sexp (&rest (sexp form)) sexp def-body)))
  ;; FIXME: Provide the fields in the order specified by `type'.
  (let* ((class (cl--find-class type))
         (slots (fcr--class-slots class))
         (prebody '())
         (slotbinds (nreverse
                     (mapcar (lambda (slot)
                               (list (cl--slot-descriptor-name slot)))
                             slots)))
         (tempbinds (mapcar
                     (lambda (field)
                       (let* ((name (car field))
                              (bind (assq name slotbinds)))
                         (cond
                          ((not bind)
                           (error "Unknown slots: %S" name))
                          ((cdr bind)
                           (error "Duplicate slots: %S" name))
                          (t
                           (let ((temp (gensym "temp")))
                             (setcdr bind (list temp))
                             (cons temp (cdr field)))))))
                     fields)))
    ;; FIXME: Since we use the docstring internally to store the
    ;; type we can't handle actual docstrings.  We could fix this by adding
    ;; a docstring slot to FCRs.
    (while (memq (car-safe (car-safe body)) '(interactive declare))
      (push (pop body) prebody))
    ;; FIXME: Optimize temps away when they're provided in the right order!
    ;; FIXME: Slots not specified in `fields' tend to emit "Variable FOO left
    ;; uninitialized"!
    `(let ,tempbinds
       (let ,slotbinds
         ;; FIXME: Prevent store-conversion for fields vars!
         ;; FIXME: Set the object's *type*!
         ;; FIXME: Make sure the slotbinds whose value is duplicable aren't
         ;; just value/variable-propagated by the optimizer (tho I think our
         ;; optimizer is too naive to be a problem currently).
         (fcr--fix-type
          (lambda ,args
            (:documentation ',type)
            ,@prebody
            ;; Add dummy code which accesses the field's vars to make sure
            ;; they're captured in the closure.
            (if t nil ,@(mapcar #'car fields))
            ,@body))))))

(defun fcr--fix-type (fcr)
  (if (byte-code-function-p fcr)
      fcr
    ;; For byte-coded functions, we store the type as a symbol in the docstring
    ;; slot.  For interpreted functions, there's no specific docstring slot
    ;; so `Ffunction' turns the symbol into a string.
    ;; We thus have convert it back into a symbol (via `intern') and then
    ;; stuff it into the environment part of the closure with a special
    ;; marker so we can distinguish this entry from actual variables.
    (cl-assert (eq 'closure (car-safe fcr)))
    (let ((typename (documentation fcr 'raw)))
      (push (cons :type (intern typename))
            (cadr fcr))
      fcr)))

(defun fcr--copy (fcr &rest args)
  (if (byte-code-function-p fcr)
      (apply #'make-closure fcr args)
    (cl-assert (eq 'closure (car-safe fcr)))
    (cl-assert (eq :type (caar (cadr fcr))))
    (let ((env (cadr fcr)))
      `(closure
           (,(car env)
            ,@(named-let loop ((env (cdr env)) (args args))
                (when args
                  (cons (cons (caar env) (car args))
                        (loop (cdr env) (cdr args)))))
            ,@(nthcdr (1+ (length args)) env))
           ,@(nthcdr 2 fcr)))))

(defun fcr-get (fcr index)
  (if (byte-code-function-p fcr)
      (let ((csts (aref fcr 2)))
        (aref csts index))
    (cl-assert (eq 'closure (car-safe fcr)))
    (cl-assert (eq :type (caar (cadr fcr))))
    (cdr (nth (1+ index) (cadr fcr)))))

(defun fcr-type (fcr)
  "Return the type of FCR, or nil if the arg is not a FunCallableRecord."
  (if (byte-code-function-p fcr)
      (let ((type (and (> (length fcr) 4) (aref fcr 4))))
        (if (symbolp type) type))
    (and (eq 'closure (car-safe fcr))
         (eq :type (caar (cadr fcr)))
         (cdar (cadr fcr)))))

(provide 'fcr)
;;; fcr.el ends here
