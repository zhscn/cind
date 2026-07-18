(define-module (cind pointer)
  #:export (configure-pointer-policy!
            handle-pointer!
            configure-scroll-policy!
            handle-scroll!))

(define pointer-policies (make-weak-key-hash-table))
(define scroll-policies (make-weak-key-hash-table))

(define (configure-pointer-policy! host procedure)
  (unless (procedure? procedure)
    (error "pointer policy must be a procedure" procedure))
  (hashq-set! pointer-policies host procedure)
  procedure)

(define (handle-pointer! host context event)
  (let ((procedure (hashq-ref pointer-policies host)))
    (unless procedure
      (error "pointer policy is not configured"))
    (procedure host context event)))

(define (configure-scroll-policy! host procedure)
  (unless (procedure? procedure)
    (error "scroll policy must be a procedure" procedure))
  (hashq-set! scroll-policies host procedure)
  procedure)

(define (handle-scroll! host context input)
  (let ((procedure (hashq-ref scroll-policies host)))
    (unless procedure
      (error "scroll policy is not configured"))
    (procedure host context input)))
