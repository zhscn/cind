;;; guile-ares-rs --- Asynchronous Reliable Extensible Sleek RPC Server
;;;
;;; Copyright © 2026 cind contributors
;;;
;;; This file is part of guile-ares-rs.
;;;
;;; guile-ares-rs is free software; you can redistribute it and/or modify it
;;; under the terms of the GNU General Public License as published by
;;; the Free Software Foundation; either version 3 of the License, or (at
;;; your option) any later version.

(define-module (ares repl)
  #:use-module (ares completion)
  #:use-module (ares evaluation eval)
  #:use-module (ares guile exceptions)
  #:use-module (ares lookup)
  #:use-module (srfi srfi-9)
  #:export (make-repl
            repl?
            repl-module
            repl-evaluate
            repl-complete
            repl-complete-summaries
            repl-lookup
            repl-result?
            repl-result-status
            repl-result-values
            repl-result-output
            repl-result-error-output
            repl-result-error
            repl-result-stack))

(define-record-type <repl>
  (%make-repl module)
  repl?
  (module repl-module))

(define-record-type <repl-result>
  (make-repl-result status values output error-output error stack)
  repl-result?
  (status repl-result-status)
  (values repl-result-values)
  (output repl-result-output)
  (error-output repl-result-error-output)
  (error repl-result-error)
  (stack repl-result-stack))

(define (make-repl module)
  (unless (module? module)
    (error "REPL environment must be a module" module))
  (%make-repl module))

(define (validate-repl repl)
  (unless (repl? repl)
    (error "expected an Ares REPL" repl)))

(define (evaluation-values result)
  (let ((kind (assoc-ref result 'result-type)))
    (case kind
      ((value) (list (assoc-ref result 'eval-value)))
      ((multiple-values) (assoc-ref result 'eval-value))
      (else '()))))

(define (repl-evaluate repl source source-name)
  "Evaluate SOURCE in REPL and return a protocol-independent result."
  (validate-repl repl)
  (unless (string? source)
    (error "REPL source must be a string" source))
  (unless (string? source-name)
    (error "REPL source name must be a string" source-name))
  (let ((output-port (open-output-string))
        (error-port (open-output-string)))
    (let ((result
           (save-module-excursion
            (lambda ()
              (set-current-module (repl-module repl))
              (with-output-to-port output-port
                (lambda ()
                  (with-error-to-port error-port
                    (lambda ()
                      ((evaluation-thunk
                        `(("code" . ,source)
                          ("file" . ,source-name))))))))))))
      (let ((kind (assoc-ref result 'result-type))
            (output (get-output-string output-port))
            (error-output (get-output-string error-port)))
        (case kind
          ((value multiple-values)
           (make-repl-result 'ok (evaluation-values result) output error-output #f #f))
          ((exception)
           (make-repl-result
            'error '() output error-output
            (exception->string (assoc-ref result 'exception-value))
            (assoc-ref result 'stack)))
          ((interrupted)
           (make-repl-result 'interrupted '() output error-output
                             "evaluation interrupted" #f))
          (else
           (error "unknown Ares evaluation result" kind)))))))

(define (repl-complete repl prefix)
  "Return structured completion candidates for PREFIX in REPL."
  (validate-repl repl)
  (completion-candidates prefix (repl-module repl)))

(define (repl-complete-summaries repl prefix)
  "Return completion candidates without resolving documentation metadata."
  (validate-repl repl)
  (completion-candidate-summaries prefix (repl-module repl)))

(define (repl-lookup repl symbol)
  "Return source and documentation metadata for SYMBOL in REPL."
  (validate-repl repl)
  (lookup-symbol (repl-module repl) symbol))
