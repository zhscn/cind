;;; guile-ares-rs --- Asynchronous Reliable Extensible Sleek RPC Server
;;;
;;; Copyright © 2023, 2024 Andrew Tropin <andrew@trop.in>
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

(define-module (ares-extension nrepl completion)
  #:use-module (ares completion)
  #:use-module (ares guile)
  #:use-module (ares reflection modules)
  #:export (nrepl.completion))

(define* (simple-completions prefix module #:optional options)
  (let ((extra-metadata
         (let ((value (assoc-ref options "extra-metadata")))
           (if value (vector->list value) '())))
        (candidates (completion-candidates prefix module)))
    (let loop ((index 0)
               (result '()))
      (if (= index (vector-length candidates))
          (list->vector (reverse result))
          (let* ((candidate (vector-ref candidates index))
                 (metadata
                  `(("candidate" . ,(completion-candidate-name candidate))
                    ("type" . ,(if (string=? (completion-candidate-type candidate)
                                             "variable")
                                    "var"
                                    (completion-candidate-type candidate)))
                    ("ns" . ,(completion-candidate-namespace candidate))))
                 (metadata
                  (if (member "arglists" extra-metadata)
                      (acons "arglists" (completion-candidate-arglists candidate) metadata)
                      metadata))
                 (metadata
                  (if (member "docs" extra-metadata)
                      (acons "docs" (completion-candidate-documentation candidate) metadata)
                      metadata)))
            (loop (+ index 1) (cons metadata result)))))))

(define (get-completions context)
  "Handles completion operation."
  (let* ((state (assoc-ref context 'ares/state))
         (reply! (assoc-ref context 'reply!))
         (message (assoc-ref context 'nrepl/message)))
    (with-exception-handler
        (lambda (ex)
          (reply! `(("status" . #("error" "completion-error" "done")))))
      (lambda ()
        (let* ((module (or (string->resolved-module (assoc-ref message "ns"))
                           (current-module)))
               (prefix (or (assoc-ref message "prefix") ""))
               (options (assoc-ref message "options"))
               (completions (simple-completions prefix module options)))
          (reply! `(("completions" . ,completions)
                    ("status" . #("done"))))))
      #:unwind? #t)))

(define operations
  `(("completions" . ,get-completions)))

(define-with-meta (nrepl.completion handler)
  "Handles completion related functionality."
  `((provides . (nrepl.completion))
    (requires . (nrepl.session))
    (handles . ,operations))

  (lambda (context)
    (let* ((message (assoc-ref context 'nrepl/message))
           (operation-function (assoc-ref operations (assoc-ref message "op"))))
      (if operation-function
          (operation-function context)
          (handler context)))))
