(define-module (cind lifecycle)
  #:export (configure-startup-policy!
            resolve-startup-plan
            configure-fallback-buffer-policy!
            create-fallback-buffer!
            startup-placeholder
            set-startup-placeholder!))

(define startup-policies (make-weak-key-hash-table))
(define fallback-buffer-policies (make-weak-key-hash-table))
(define startup-placeholders (make-weak-key-hash-table))

(define (configure-startup-policy! host procedure)
  (unless (procedure? procedure)
    (error "startup policy must be a procedure" procedure))
  (hashq-set! startup-policies host procedure)
  procedure)

(define (resolve-startup-plan host facts)
  (let ((procedure (hashq-ref startup-policies host)))
    (unless procedure
      (error "startup policy is not configured"))
    (procedure host facts)))

(define (configure-fallback-buffer-policy! host procedure)
  (unless (procedure? procedure)
    (error "fallback buffer policy must be a procedure" procedure))
  (hashq-set! fallback-buffer-policies host procedure)
  procedure)

(define (create-fallback-buffer! host)
  (let ((procedure (hashq-ref fallback-buffer-policies host)))
    (unless procedure
      (error "fallback buffer policy is not configured"))
    (procedure host)))

(define (startup-placeholder host)
  (hashq-ref startup-placeholders host))

(define (set-startup-placeholder! host buffer)
  (if buffer
      (hashq-set! startup-placeholders host buffer)
      (hashq-remove! startup-placeholders host))
  buffer)
