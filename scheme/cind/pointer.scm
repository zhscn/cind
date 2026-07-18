(define-module (cind pointer)
  #:export (configure-pointer-policy!
            handle-pointer!))

(define pointer-policies (make-weak-key-hash-table))

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
