(define-module (cind pointer)
  #:use-module (cind state)
  #:export (configure-pointer-policy!
            handle-pointer!
            configure-scroll-policy!
            handle-scroll!))

;; Both are mandatory policies: the frontend has no meaningful behavior for a
;; pointer or scroll event until one is configured (design/09-guile-first.md §4).
(define-policy-slot! 'pointer)
(define-policy-slot! 'scroll)

(define (configure-pointer-policy! host procedure)
  (policy-set! host 'pointer procedure))

(define (handle-pointer! host context event)
  ((policy-ref host 'pointer) host context event))

(define (configure-scroll-policy! host procedure)
  (policy-set! host 'scroll procedure))

(define (handle-scroll! host context input)
  ((policy-ref host 'scroll) host context input))
