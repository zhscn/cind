(define-module (cind application)
  #:use-module (cind state)
  #:export (application-state
            exit-editor!
            request-redraw!
            caret-reveal?
            set-caret-reveal!
            page-rows
            set-page-rows!))

;; #(exit-requested? caret-reveal? page-rows).
(define-state-slot! 'application (lambda () (vector #f #t 1)))

(define (host-application-state host)
  (state-ref host 'application))

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
