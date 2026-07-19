;;; guile-ares-rs --- Asynchronous Reliable Extensible Sleek RPC Server
;;;
;;; Copyright © 2024 Nikita Domnitskii
;;;
;;; This file is part of guile-ares-rs.
;;;
;;; guile-ares-rs is free software; you can redistribute it and/or modify it
;;; under the terms of the GNU General Public License as published by
;;; the Free Software Foundation; either version 3 of the License, or (at
;;; your option) any later version.
;;;
;;; guile-ares-rs is distributed in the hope that it will be useful, but
;;; WITHOUT ANY WARRANTY; without even the implied warranty of
;;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;;; General Public License for more details.
;;;
;;; You should have received a copy of the GNU General Public License
;;; along with guile-ares-rs.  If not, see <http://www.gnu.org/licenses/>.

(define-module (ares-extension nrepl lookup)
  #:use-module (ares guile)
  #:use-module (ares lookup)
  #:use-module (ares reflection modules)
  #:use-module (srfi srfi-2)
  #:export (nrepl.lookup))

(define (get-lookup-information context)
  "Handle lookup operation, provide information necessary for go to
definition, documentation and other purposes."
  (let* ((state (assoc-ref context 'ares/state))
         (message (assoc-ref context 'nrepl/message))
         (reply! (assoc-ref context 'reply!)))
    (with-exception-handler
        (lambda (ex)
          (reply! `(("status" . #("error" "lookup-error" "done")))))
      (lambda ()
        (let ((ns (or (string->resolved-module (assoc-ref message "ns"))
                      (current-module)))
              (sym (and=> (assoc-ref message "sym") string->symbol)))
          (reply! `(("status" . #("done"))
                    ("info" . ,(lookup-symbol ns sym))))))
      #:unwind? #t)))

(define operations
  `(("lookup" . ,get-lookup-information)))

(define-with-meta (nrepl.lookup handler)
  "Handles lookup related functionality."
  `((provides . (nrepl.lookup))
    (requires . (nrepl.session))
    (handles . ,operations))

  (lambda (context)
    (let* ((message (assoc-ref context 'nrepl/message))
           (op (assoc-ref message "op"))
           (operation-function (assoc-ref operations op)))
      (if operation-function
          (operation-function context)
          (handler context)))))
