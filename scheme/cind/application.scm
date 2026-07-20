(define-module (cind application)
  #:export (application-state
            exit-editor!
            request-redraw!
            caret-reveal?
            set-caret-reveal!
            page-rows
            set-page-rows!))

;; #(exit-requested? caret-reveal? page-rows). Application instances are
;; represented by weak host capabilities, so state disappears with the owning
;; editor.
(define application-states (make-weak-key-hash-table))

(define (host-application-state host)
  (or (hashq-ref application-states host)
      (let ((state (vector #f #t 1)))
        (hashq-set! application-states host state)
        state)))

(define (application-state host)
  (let ((state (host-application-state host)))
    (vector (vector-ref state 0) (vector-ref state 1) (vector-ref state 2))))

(define (exit-editor! host)
  (vector-set! (host-application-state host) 0 #t))

(define (caret-reveal? host)
  (vector-ref (host-application-state host) 1))

(define (set-caret-reveal! host reveal?)
  (unless (boolean? reveal?)
    (error "caret reveal state must be a boolean" reveal?))
  (vector-set! (host-application-state host) 1 reveal?))

(define (request-redraw! host)
  (set-caret-reveal! host #t))

(define (page-rows host)
  (vector-ref (host-application-state host) 2))

(define (set-page-rows! host rows)
  (unless (and (integer? rows) (positive? rows))
    (error "page rows must be a positive integer" rows))
  (vector-set! (host-application-state host) 2 rows))
