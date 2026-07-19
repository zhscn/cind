;;; guile-ares-rs --- Asynchronous Reliable Extensible Sleek RPC Server
;;;
;;; Copyright © 2023, 2024 Andrew Tropin <andrew@trop.in>
;;; Copyright © 2026 cind contributors
;;;
;;; This file is part of guile-ares-rs.
;;;
;;; guile-ares-rs is free software; you can redistribute it and/or modify it
;;; under the terms of the GNU General Public License as published by
;;; the Free Software Foundation; either version 3 of the License, or (at
;;; your option) any later version.

(define-module (ares completion)
  #:use-module (ares reflection metadata)
  #:use-module (ice-9 regex)
  #:use-module (ice-9 session)
  #:use-module (srfi srfi-9)
  #:export (completion-candidate?
            completion-candidate-name
            completion-candidate-type
            completion-candidate-namespace
            completion-candidate-arglists
            completion-candidate-documentation
            completion-candidates))

(define-record-type <completion-candidate>
  (make-completion-candidate name type namespace arglists documentation)
  completion-candidate?
  (name completion-candidate-name)
  (type completion-candidate-type)
  (namespace completion-candidate-namespace)
  (arglists completion-candidate-arglists)
  (documentation completion-candidate-documentation))

(define (variable-type variable)
  (cond ((macro? variable) "macro")
        ((procedure? variable) "function")
        (else "variable")))

(define (completion-candidates prefix module)
  "Return completion metadata for names beginning with PREFIX in MODULE."
  (unless (string? prefix)
    (error "completion prefix must be a string" prefix))
  (unless (module? module)
    (error "completion environment must be a module" module))
  (let ((candidates
         (apropos-fold
          (lambda (candidate-module name variable result)
            (cons (make-completion-candidate
                   (symbol->string name)
                   (variable-type variable)
                   (object->string (module-name candidate-module))
                   (get-arglists variable)
                   (get-docstring variable))
                  result))
          '()
          (string-append "^" (regexp-quote prefix))
          ((@@ (ice-9 session) make-fold-modules)
           (lambda () (list module))
           (compose reverse module-uses)
           identity))))
    (list->vector
     (sort! candidates
            (lambda (left right)
              (string<? (completion-candidate-name left)
                        (completion-candidate-name right)))))))
