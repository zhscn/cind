(define-module (cind emacs)
  #:use-module (cind command)
  #:use-module (cind host)
  #:use-module (cind input)
  #:export (install-emacs-input-state!
            emacs-command-definitions))

(define universal-state 'emacs-universal)
(define universal-sessions '())

(define (session-matches? entry host view)
  (and (eq? (vector-ref entry 0) host)
       (equal? (vector-ref entry 1) view)))

(define (find-universal-session host view)
  (let loop ((remaining universal-sessions))
    (and (pair? remaining)
         (let ((entry (car remaining)))
           (if (session-matches? entry host view)
               entry
               (loop (cdr remaining)))))))

(define (remove-universal-session! host view)
  (set! universal-sessions
        (let loop ((remaining universal-sessions)
                   (kept '()))
          (cond ((null? remaining) (reverse kept))
                ((session-matches? (car remaining) host view)
                 (append (reverse kept) (cdr remaining)))
                (else (loop (cdr remaining) (cons (car remaining) kept)))))))

(define (start-universal-session! host view)
  (remove-universal-session! host view)
  (let ((session (vector host view 'initial)))
    (set! universal-sessions (cons session universal-sessions))
    session))

(define (universal-prefix invocation count)
  (command-prefix count
                  (invocation-register invocation)
                  (invocation-prefix-extra invocation)))

(define (universal-handler host context key)
  (let ((view (context-view context)))
    (cond ((string=? key "C-u")
           (command-dispatch "emacs.universal-more"))
          ((and (= (string-length key) 1)
                (char-numeric? (string-ref key 0)))
           (command-dispatch "emacs.universal-digit" key))
          ((string=? key "-")
           (command-dispatch "emacs.negative-argument"))
          ((string=? key "Backspace")
           (pop-input-state! host view)
           (command-dispatch "edit.delete-backward-raw"))
          ((string=? key "Delete")
           (pop-input-state! host view)
           (command-dispatch "edit.delete-forward-raw"))
          (else
           (pop-input-state! host view)
           'pass))))

(define (universal-start-command host)
  (lambda (context invocation)
    (let ((view (context-view context)))
      (if (find-universal-session host view)
          (command-error "universal argument session is already active")
          (begin
            (push-input-state! host view universal-state)
            (universal-prefix invocation
                              (* (or (invocation-repeat-count invocation) 1) 4)))))))

(define (universal-more-command host)
  (lambda (context invocation)
    (if (find-universal-session host (context-view context))
        (universal-prefix invocation
                          (* (or (invocation-repeat-count invocation) 1) 4))
        (command-error "universal argument session is not active"))))

(define (universal-digit-command host)
  (lambda (context invocation)
    (let* ((session (find-universal-session host (context-view context)))
           (arguments (invocation-arguments invocation))
           (value (and (= (vector-length arguments) 1)
                       (vector-ref arguments 0))))
      (if (not session)
          (command-error "universal argument session is not active")
          (if (not (and (string? value)
                        (= (string-length value) 1)
                        (char-numeric? (string-ref value 0))))
              (command-error "universal digit requires one decimal key")
              (let* ((digit (- (char->integer (string-ref value 0))
                               (char->integer #\0)))
                     (phase (vector-ref session 2))
                     (current (or (invocation-repeat-count invocation) 0))
                     (count (cond ((eq? phase 'initial) digit)
                                  ((eq? phase 'negative) (- digit))
                                  ((< current 0) (- (* current 10) digit))
                                  (else (+ (* current 10) digit)))))
                (vector-set! session 2 'digits)
                (universal-prefix invocation count)))))))

(define (negative-argument-command host)
  (lambda (context invocation)
    (let ((session (find-universal-session host (context-view context)))
          (current (or (invocation-repeat-count invocation) 1)))
      (if (not session)
          (command-error "universal argument session is not active")
          (let ((phase (vector-ref session 2)))
            (cond ((eq? phase 'initial)
                   (vector-set! session 2 'negative)
                   (universal-prefix invocation -1))
                  ((eq? phase 'negative)
                   (vector-set! session 2 'initial)
                   (universal-prefix invocation (- current)))
                  (else
                   (universal-prefix invocation (- current)))))))))

(define (emacs-command-definitions host)
  (list (list "emacs.universal-argument" (universal-start-command host) #f)
        (list "emacs.universal-more" (universal-more-command host) #f)
        (list "emacs.universal-digit" (universal-digit-command host) #f)
        (list "emacs.negative-argument" (negative-argument-command host) #f)))

(define (install-emacs-input-state! host)
  (define-input-state! host 'emacs)
  (define-input-state! host universal-state
    #:text-input 'ignore
    #:indicator "ARG"
    #:handler (lambda (context key) (universal-handler host context key))
    #:on-enter
    (lambda (event)
      (start-universal-session! host (vector-ref event 1)))
    #:on-exit
    (lambda (event)
      (remove-universal-session! host (vector-ref event 1))))
  (define-input-strategy! host 'emacs 'emacs 'emacs 'collapse)
  (set-default-input-strategy! host 'emacs)
  2)
