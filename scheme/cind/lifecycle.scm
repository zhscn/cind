(define-module (cind lifecycle)
  #:export (configure-startup-policy!
            resolve-startup-plan
            configure-session-policy!
            resolve-session-plan
            configure-fallback-buffer-policy!
            create-fallback-buffer!
            configure-close-policy!
            resolve-close-command
            startup-placeholder
            set-startup-placeholder!))

(define startup-policies (make-weak-key-hash-table))
(define session-policies (make-weak-key-hash-table))
(define fallback-buffer-policies (make-weak-key-hash-table))
(define startup-placeholders (make-weak-key-hash-table))
(define close-policies (make-weak-key-hash-table))

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

(define (configure-session-policy! host procedure)
  (unless (procedure? procedure)
    (error "session policy must be a procedure" procedure))
  (hashq-set! session-policies host procedure)
  procedure)

(define (resolve-session-plan host facts)
  (let ((procedure (hashq-ref session-policies host)))
    (unless procedure
      (error "session policy is not configured"))
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

(define (configure-close-policy! host procedure)
  (unless (procedure? procedure)
    (error "close policy must be a procedure" procedure))
  (hashq-set! close-policies host procedure)
  procedure)

(define (resolve-close-command host context force?)
  (let ((procedure (hashq-ref close-policies host)))
    (unless procedure
      (error "close policy is not configured"))
    (procedure host context force?)))

(define (startup-placeholder host)
  (hashq-ref startup-placeholders host))

(define (set-startup-placeholder! host buffer)
  (if buffer
      (hashq-set! startup-placeholders host buffer)
      (hashq-remove! startup-placeholders host))
  buffer)
