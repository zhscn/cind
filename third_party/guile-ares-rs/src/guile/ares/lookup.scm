;;; guile-ares-rs --- Asynchronous Reliable Extensible Sleek RPC Server
;;;
;;; Copyright © 2024 Nikita Domnitskii
;;; Copyright © 2026 cind contributors
;;;
;;; This file is part of guile-ares-rs.
;;;
;;; guile-ares-rs is free software; you can redistribute it and/or modify it
;;; under the terms of the GNU General Public License as published by
;;; the Free Software Foundation; either version 3 of the License, or (at
;;; your option) any later version.

(define-module (ares lookup)
  #:use-module (ares file)
  #:use-module (ares guile)
  #:use-module (ares reflection metadata)
  #:use-module ((ares reflection modules) #:prefix reflection:)
  #:use-module ((ice-9 regex) #:select (regexp-quote))
  #:use-module ((ice-9 session) #:select (apropos-fold))
  #:use-module (srfi srfi-2)
  #:use-module (srfi srfi-197)
  #:use-module (system vm program)
  #:export (lookup-symbol))

(define (lookup-symbol module symbol)
  "Return source, documentation, and argument metadata for SYMBOL in MODULE."
  (unless (module? module)
    (error "lookup environment must be a module" module))
  (unless (symbol? symbol)
    (error "lookup name must be a symbol" symbol))
  (define (module-location candidate-module)
    `(0 ,(reflection:module-filename candidate-module) 0 . 0))
  (apropos-fold
   (lambda (candidate-module name variable result)
     (let* ((source (or (get-source variable)
                        (and=> candidate-module module-location)))
            (file (chain-and
                   (source:file source)
                   (search-in-load-path _)))
            (line (and=> source source:line-for-user))
            (column (and=> source source:column))
            (arglists (get-arglists variable))
            (documentation (get-docstring variable)))
       (chain-when
        `(("ns" . ,(object->string (module-name candidate-module))))
        (file (acons "file" file _))
        (line (acons "line" line _))
        (column (acons "column" column _))
        (arglists (acons "arglists" arglists _))
        (documentation (acons "docstring" documentation _)))))
   #f
   (string-append "^" (regexp-quote (symbol->string symbol)) "$")
   ((@@ (ice-9 session) make-fold-modules)
    (lambda () (list module))
    (compose reverse module-uses)
    identity)))
