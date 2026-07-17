(define-module (cind helix)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (install-helix-input-states!
            helix-command-definitions
            install-helix-keymaps!))

(define helix-normal-keymap 'helix.normal)
(define helix-select-keymap 'helix.select)
(define helix-insert-keymap 'helix.insert)
(define helix-thing-state 'helix-thing)

;; Each entry is #(host view extent captured-thing-or-#f).
(define thing-sessions '())

(define (session-matches? entry host view)
  (and (eq? (vector-ref entry 0) host)
       (equal? (vector-ref entry 1) view)))

(define (find-thing-session host view)
  (let loop ((sessions thing-sessions))
    (and (pair? sessions)
         (let ((entry (car sessions)))
           (if (session-matches? entry host view)
               entry
               (loop (cdr sessions)))))))

(define (remove-thing-session! host view)
  (set! thing-sessions
        (let loop ((sessions thing-sessions)
                   (kept '()))
          (cond ((null? sessions) (reverse kept))
                ((session-matches? (car sessions) host view)
                 (append (reverse kept) (cdr sessions)))
                (else (loop (cdr sessions) (cons (car sessions) kept)))))))

(define (start-thing-session! host view extent)
  (remove-thing-session! host view)
  (set! thing-sessions
        (cons (vector host view extent #f) thing-sessions)))

(define (thing-for-key key)
  (cond ((string=? key "a") 'angle)
        ((string=? key "w") 'word)
        ((string=? key "s") 'string)
        ((string=? key "f") 'defun)
        (else #f)))

(define thing-hints
  (vector (vector "a" "angle" #f)
          (vector "w" "word" #f)
          (vector "s" "string" #f)
          (vector "f" "function" #f)))

(define (thing-handle-key host context key)
  (let* ((view (context-view context))
         (session (find-thing-session host view))
         (thing (thing-for-key key)))
    (cond ((not session)
           (pop-input-state! host view)
           (set-message! host "thing capture session is missing")
           'consume)
          ((not thing)
           (pop-input-state! host view)
           (set-message! host (string-append "undefined thing key: " key))
           'consume)
          (else
           (vector-set! session 3 thing)
           (pop-input-state! host view)
           (vector 'dispatch "helix.thing-commit")))))

(define (select-helix-strategy host)
  (lambda (context invocation)
    (set-view-input-strategy! host (context-view context) 'helix)
    (command-completed/preserve)))

(define (select-helix-state host state)
  (lambda (context invocation)
    (set-base-input-state! host (context-view context) state)
    (command-completed/preserve)))

(define (motion-command host motion extend?)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (count (or (invocation-repeat-count invocation) 1))
           (selected (motion-selection host view motion count extend?)))
      (reset-preferred-column! host view)
      (request-redraw! host)
      (command-completed/selection selected))))

(define (thing-start-command host extent display)
  (lambda (context invocation)
    (let ((view (context-view context)))
      (start-thing-session! host view extent)
      (push-input-state! host view helix-thing-state)
      (set-input-feedback! host view display thing-hints)
      (command-completed/preserve))))

(define (thing-commit-command host)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (session (find-thing-session host view))
           (extent (and session (vector-ref session 2)))
           (thing (and session (vector-ref session 3))))
      (if (not thing)
          (command-error "thing capture session is missing")
          (let ((selected (thing-selection host view thing extent)))
            (remove-thing-session! host view)
            (if selected
                (command-completed/selection selected)
                (command-error (string-append "no " (symbol->string thing)
                                              " thing at every selection"))))))))

(define (delete-selection-command host)
  (lambda (context invocation)
    (set-base-input-state! host (context-view context) 'hx-normal)
    (command-dispatch "edit.delete-selection")))

(define (helix-command-definitions host)
  (list (list "helix.enter-normal" (select-helix-strategy host) #f)
        (list "helix.enter-select" (select-helix-state host 'hx-select) #f)
        (list "helix.enter-insert" (select-helix-state host 'hx-insert) #f)
        (list "helix.move-backward-character"
              (motion-command host 'cind.backward-character #f) #f)
        (list "helix.move-forward-character"
              (motion-command host 'cind.forward-character #f) #f)
        (list "helix.move-backward-word"
              (motion-command host 'cind.backward-word #f) #f)
        (list "helix.move-forward-word"
              (motion-command host 'cind.forward-word #f) #f)
        (list "helix.extend-backward-character"
              (motion-command host 'cind.backward-character #t) #f)
        (list "helix.extend-forward-character"
              (motion-command host 'cind.forward-character #t) #f)
        (list "helix.extend-backward-word"
              (motion-command host 'cind.backward-word #t) #f)
        (list "helix.extend-forward-word"
              (motion-command host 'cind.forward-word #t) #f)
        (list "helix.thing-inner" (thing-start-command host 'inner "m i") #f)
        (list "helix.thing-bounds" (thing-start-command host 'bounds "m a") #f)
        (list "helix.thing-commit" (thing-commit-command host) #f)
        (list "helix.delete-selection" (delete-selection-command host) #f)))

(define (install-helix-input-states! host)
  (define-keymap! host helix-normal-keymap #f)
  (define-keymap! host helix-select-keymap #f)
  (define-keymap! host helix-insert-keymap #f)
  (define-input-state! host 'hx-normal
    (vector helix-normal-keymap) 'ignore 'block "NOR" #f)
  (define-input-state! host 'hx-select
    (vector helix-select-keymap) 'ignore 'block "SEL" #f)
  (define-input-state! host 'hx-insert
    (vector helix-insert-keymap) 'accept 'beam "INS" #f)
  (define-input-state! host helix-thing-state
    (vector) 'ignore 'block "OBJ"
    (lambda (context key) (thing-handle-key host context key)))
  (define-input-strategy! host 'helix 'hx-normal 'hx-normal 'preserve)
  (observe-input-state-changes!
   host
   (lambda (event)
     (when (and (eq? (vector-ref event 0) 'pop)
                (eq? (vector-ref event 2) helix-thing-state))
       (let* ((view (vector-ref event 1))
              (session (find-thing-session host view)))
         (when (and session (not (vector-ref session 3)))
           (remove-thing-session! host view))))))
  4)

(define normal-bindings
  '(("h" . "helix.move-backward-character")
    ("l" . "helix.move-forward-character")
    ("w" . "helix.move-forward-word")
    ("b" . "helix.move-backward-word")
    ("v" . "helix.enter-select")
    ("i" . "helix.enter-insert")
    ("m i" . "helix.thing-inner")
    ("m a" . "helix.thing-bounds")
    ("d" . "helix.delete-selection")
    ("p" . "edit.yank")
    ("u" . "edit.undo")
    ("C-r" . "edit.redo")))

(define select-bindings
  '(("h" . "helix.extend-backward-character")
    ("l" . "helix.extend-forward-character")
    ("w" . "helix.extend-forward-word")
    ("b" . "helix.extend-backward-word")
    ("m i" . "helix.thing-inner")
    ("m a" . "helix.thing-bounds")
    ("d" . "helix.delete-selection")
    ("ESC" . "helix.enter-normal")))

(define insert-bindings
  '(("ESC" . "helix.enter-normal")))

(define (bind-all! host keymap bindings)
  (let loop ((remaining bindings)
             (count 0))
    (if (null? remaining)
        count
        (let ((binding (car remaining)))
          (loop (cdr remaining)
                (if (bind-key-if-command! host keymap (car binding) (cdr binding))
                    (+ count 1)
                    count))))))

(define (install-helix-keymaps! host)
  (define-keymap! host helix-normal-keymap #f)
  (define-keymap! host helix-select-keymap #f)
  (define-keymap! host helix-insert-keymap #f)
  (+ (bind-all! host helix-normal-keymap normal-bindings)
     (bind-all! host helix-select-keymap select-bindings)
     (bind-all! host helix-insert-keymap insert-bindings)))
