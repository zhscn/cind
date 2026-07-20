(define-module (cind lifecycle)
  #:use-module (cind host)
  #:use-module (cind lsp)
  #:export (configure-startup-policy!
            resolve-startup-plan
            configure-session-policy!
            resolve-session-plan
            configure-fallback-buffer-policy!
            create-fallback-buffer!
            configure-close-policy!
            resolve-close-command
            begin-buffer-save!
            complete-buffer-save!
            abort-buffer-save!
            buffer-saving?
            observe-buffer-edits!
            buffer-edited!
            buffer-created!
            buffer-style-origin
            buffer-released!
            startup-placeholder
            set-startup-placeholder!))

(define startup-policies (make-weak-key-hash-table))
(define session-policies (make-weak-key-hash-table))
(define fallback-buffer-policies (make-weak-key-hash-table))
(define startup-placeholders (make-weak-key-hash-table))
(define close-policies (make-weak-key-hash-table))
(define buffer-save-states (make-weak-key-hash-table))
(define buffer-edit-observers (make-weak-key-hash-table))
(define buffer-style-origins (make-weak-key-hash-table))

(define (host-buffer-style-origins host)
  (or (hashq-ref buffer-style-origins host) '()))

(define (without-buffer-style-origin entries buffer)
  (cond ((null? entries) '())
        ((equal? buffer (vector-ref (car entries) 0))
         (cdr entries))
        (else
         (cons (car entries)
               (without-buffer-style-origin (cdr entries) buffer)))))

(define (buffer-created! host buffer style-origin)
  (unless (string? style-origin)
    (error "buffer style origin must be a string" style-origin))
  (hashq-set! buffer-style-origins host
              (cons (vector buffer style-origin)
                    (without-buffer-style-origin
                     (host-buffer-style-origins host) buffer)))
  buffer)

(define (buffer-style-origin host buffer)
  (let loop ((entries (host-buffer-style-origins host)))
    (cond ((null? entries)
           (error "buffer has no style origin" buffer))
          ((equal? buffer (vector-ref (car entries) 0))
           (vector-ref (car entries) 1))
          (else (loop (cdr entries))))))

(define (buffer-released! host buffer)
  (set-buffer-saves! host
                     (without-buffer-save (host-buffer-saves host) buffer))
  (when (equal? buffer (startup-placeholder host))
    (hashq-remove! startup-placeholders host))
  (let ((remaining
         (without-buffer-style-origin (host-buffer-style-origins host) buffer)))
    (if (null? remaining)
        (hashq-remove! buffer-style-origins host)
        (hashq-set! buffer-style-origins host remaining)))
  (lsp-buffer-released! host buffer))

(define (observe-buffer-edits! host procedure)
  (unless (procedure? procedure)
    (error "buffer edit observer must be a procedure" procedure))
  (let ((observers (or (hashq-ref buffer-edit-observers host) '())))
    (hashq-set! buffer-edit-observers host
                (append observers (list procedure)))
    (length observers)))

(define (buffer-edited! host buffer view revision)
  (for-each (lambda (procedure)
              (procedure host buffer view revision))
            (or (hashq-ref buffer-edit-observers host) '())))

(define (host-buffer-saves host)
  (or (hashq-ref buffer-save-states host) '()))

(define (buffer-save-entry host buffer)
  (let loop ((entries (host-buffer-saves host)))
    (and (pair? entries)
         (if (equal? buffer (vector-ref (car entries) 0))
             (car entries)
             (loop (cdr entries))))))

(define (without-buffer-save entries buffer)
  (cond ((null? entries) '())
        ((equal? buffer (vector-ref (car entries) 0))
         (cdr entries))
        (else
         (cons (car entries)
               (without-buffer-save (cdr entries) buffer)))))

(define (set-buffer-saves! host entries)
  (if (null? entries)
      (hashq-remove! buffer-save-states host)
      (hashq-set! buffer-save-states host entries)))

(define (buffer-saving? host buffer)
  (and (buffer-save-entry host buffer) #t))

(define (begin-buffer-save! host buffer)
  (when (buffer-saving? host buffer)
    (error "save already in progress"))
  (let ((contents (buffer-save-snapshot host buffer)))
    (set-buffer-saves! host
                       (cons (vector buffer contents)
                             (host-buffer-saves host)))
    contents))

(define (complete-buffer-save! host buffer)
  (let ((entry (buffer-save-entry host buffer)))
    (unless entry
      (error "buffer has no save in progress"))
    (let ((newer-edits?
           (mark-buffer-saved! host buffer (vector-ref entry 1))))
      (set-buffer-saves! host
                         (without-buffer-save (host-buffer-saves host) buffer))
      newer-edits?)))

(define (abort-buffer-save! host buffer)
  (set-buffer-saves! host
                     (without-buffer-save (host-buffer-saves host) buffer)))

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
