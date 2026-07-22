(define-module (cind lifecycle)
  #:use-module (cind host)
  #:use-module (cind lsp)
  #:use-module (cind state)
  #:use-module (cind workbench)
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

;; Bootstrap and buffer-lifecycle policies are mandatory: the application has
;; no built-in fallback for them (design/09-guile-first.md §4).
(define-policy-slot! 'startup)
(define-policy-slot! 'session)
(define-policy-slot! 'fallback-buffer)
(define-policy-slot! 'close)

(define-state-slot! 'startup-placeholder (lambda () #f))
(define-state-slot! 'buffer-saves (lambda () '()))
(define-state-slot! 'buffer-edit-observers (lambda () '()))
(define-state-slot! 'buffer-style-origins (lambda () '()))

(define (host-buffer-style-origins host)
  (state-ref host 'buffer-style-origins))

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
  (state-set! host 'buffer-style-origins
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
    (state-clear! host 'startup-placeholder))
  (state-set! host 'buffer-style-origins
              (without-buffer-style-origin (host-buffer-style-origins host) buffer))
  (lsp-buffer-released! host buffer)
  (workbench-forget-buffer! host buffer))

(define (observe-buffer-edits! host procedure)
  (unless (procedure? procedure)
    (error "buffer edit observer must be a procedure" procedure))
  (let ((observers (state-ref host 'buffer-edit-observers)))
    (state-set! host 'buffer-edit-observers
                (append observers (list procedure)))
    (length observers)))

(define (buffer-edited! host buffer view revision)
  (for-each (lambda (procedure)
              (procedure host buffer view revision))
            (state-ref host 'buffer-edit-observers)))

(define (host-buffer-saves host)
  (state-ref host 'buffer-saves))

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
  (state-set! host 'buffer-saves entries))

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
  (policy-set! host 'startup procedure))

(define (resolve-startup-plan host facts)
  ((policy-ref host 'startup) host facts))

(define (configure-session-policy! host procedure)
  (policy-set! host 'session procedure))

(define (resolve-session-plan host facts)
  ((policy-ref host 'session) host facts))

(define (configure-fallback-buffer-policy! host procedure)
  (policy-set! host 'fallback-buffer procedure))

(define (create-fallback-buffer! host)
  ((policy-ref host 'fallback-buffer) host))

(define (configure-close-policy! host procedure)
  (policy-set! host 'close procedure))

(define (resolve-close-command host context force?)
  ((policy-ref host 'close) host context force?))

(define (startup-placeholder host)
  (state-ref host 'startup-placeholder))

(define (set-startup-placeholder! host buffer)
  (if buffer
      (state-set! host 'startup-placeholder buffer)
      (state-clear! host 'startup-placeholder))
  buffer)
