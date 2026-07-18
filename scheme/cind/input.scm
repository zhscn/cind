(define-module (cind input)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (define-input-state!
            install-read-key-input-state!
            read-key-then!))

(define* (define-input-state! host name
                              #:key
                              (keymaps (vector))
                              (text-input 'accept)
                              (text-command "edit.self-insert")
                              (cursor 'beam)
                              (indicator "")
                              (handler #f)
                              (on-enter #f)
                              (on-exit #f)
                              (position-hints #f))
  (let ((state (%define-input-state! host name keymaps text-input text-command
                                     cursor indicator handler)))
    (set-input-state-lifecycle! host name on-enter on-exit)
    (set-input-state-position-hints! host name position-hints)
    state))

(define read-key-state 'input.read-key)

;; Entries are #(host view procedure). The module owns only transient policy;
;; the View InputState stack remains the authoritative lifetime.
(define sessions '())

(define (session-matches? entry host view)
  (and (eq? (vector-ref entry 0) host)
       (equal? (vector-ref entry 1) view)))

(define (find-session host view)
  (let loop ((remaining sessions))
    (and (pair? remaining)
         (let ((entry (car remaining)))
           (if (session-matches? entry host view)
               entry
               (loop (cdr remaining)))))))

(define (remove-session! host view)
  (set! sessions
        (let loop ((remaining sessions)
                   (kept '()))
          (cond ((null? remaining) (reverse kept))
                ((session-matches? (car remaining) host view)
                 (append (reverse kept) (cdr remaining)))
                (else (loop (cdr remaining) (cons (car remaining) kept)))))))

(define (handle-key host context key)
  (let* ((view (context-view context))
         (session (find-session host view)))
    (if (not session)
        (begin
          (pop-input-state! host view)
          (set-message! host "single-key capture session is missing")
          'consume)
        (let ((procedure (vector-ref session 2)))
          (remove-session! host view)
          (pop-input-state! host view)
          (procedure key)))))

(define* (read-key-then! host view procedure
                         #:key
                         (sequence "")
                         (hints (vector)))
  (if (not (procedure? procedure))
      (error "read-key-then! requires a procedure" procedure))
  (if (not (string? sequence))
      (error "read-key-then! sequence must be a string" sequence))
  (if (not (vector? hints))
      (error "read-key-then! hints must be a vector" hints))
  (if (find-session host view)
      (error "single-key capture is already active for this view" view))
  (let ((pushed? #f))
    (catch #t
      (lambda ()
        (set! sessions (cons (vector host view procedure) sessions))
        (push-input-state! host view read-key-state)
        (set! pushed? #t)
        (set-input-feedback! host view sequence hints))
      (lambda (key . arguments)
        (if pushed?
            (pop-input-state! host view)
            (remove-session! host view))
        (apply throw key arguments)))))

(define (install-read-key-input-state! host)
  (define-input-state! host read-key-state
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "KEY"
    #:handler (lambda (context key) (handle-key host context key))
    #:on-exit (lambda (event) (remove-session! host (vector-ref event 1))))
  1)
