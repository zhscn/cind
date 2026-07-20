(define-module (cind structural)
  #:use-module (cind application)
  #:use-module (cind command)
  #:use-module (cind host)
  #:use-module (cind input)
  #:export (install-structural-input-state!
            structural-command-definitions
            install-structural-keymap!))

(define structural-keymap 'structural.node)
(define structural-state 'structural-node)

;; Each entry is #(host view). Selection history itself is an anchored View
;; mechanism so it stays valid across document transactions.
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

(define (start-session! host view)
  (remove-session! host view)
  (clear-selection-history! host view)
  (let ((session (vector host view)))
    (set! sessions (cons session sessions))
    session))

(define (expand-result host view)
  (let* ((current (view-selection host view))
         (expanded (expand-node-selection host view current)))
    (if expanded
        (begin
          (push-selection-history! host view current)
          (request-redraw! host)
          (command-completed/selection expanded))
        (command-error "no enclosing syntax node at every selection"))))

(define (enter-command host)
  (lambda (context invocation)
    (let ((view (context-view context)))
      (start-session! host view)
      (push-input-state! host view structural-state)
      (let ((result (expand-result host view)))
        (when (eq? (vector-ref result 0) 'error)
          (pop-input-state! host view))
        result))))

(define (expand-command host)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (session (find-session host view)))
      (if session
          (expand-result host view)
          (command-error "structural selection session is not active")))))

(define (contract-command host)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (session (find-session host view))
           (previous (and session (pop-selection-history! host view))))
      (if previous
          (begin
            (request-redraw! host)
            (command-completed/selection previous))
          (command-error "structural selection is at its initial range")))))

(define (exit-command host)
  (lambda (context invocation)
    (pop-input-state! host (context-view context))
    (command-completed/preserve)))

(define (delete-command host)
  (lambda (context invocation)
    (pop-input-state! host (context-view context))
    (command-dispatch "edit.delete-selection")))

(define (structural-command-definitions host)
  (list (list "structural.enter" (enter-command host) #f)
        (list "structural.exit" (exit-command host) #f)
        (list "structural.delete" (delete-command host) #f)
        (list "selection.expand" (expand-command host) #f)
        (list "selection.contract" (contract-command host) #f)))

(define (install-structural-input-state! host)
  (define-keymap! host structural-keymap #f)
  (define-input-state! host structural-state
    #:keymaps (vector structural-keymap)
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "NODE"
    #:on-exit
    (lambda (event)
      (let ((view (vector-ref event 1)))
        (clear-selection-history! host view)
        (remove-session! host view))))
  1)

(define bindings
  '(("e" . "selection.expand")
    ("l" . "selection.expand")
    ("]" . "selection.expand")
    ("s" . "selection.contract")
    ("h" . "selection.contract")
    ("[" . "selection.contract")
    ("d" . "structural.delete")
    ("q" . "structural.exit")
    ("ESC" . "structural.exit")))

(define (install-structural-keymap! host)
  (define-keymap! host structural-keymap #f)
  (let loop ((remaining bindings)
             (count 0))
    (if (null? remaining)
        count
        (let ((binding (car remaining)))
          (loop (cdr remaining)
                (if (bind-key-if-command! host structural-keymap
                                          (car binding) (cdr binding))
                    (+ count 1)
                    count))))))
