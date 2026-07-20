(define-module (cind helix)
  #:use-module (cind application)
  #:use-module (cind command)
  #:use-module (cind host)
  #:use-module (cind input)
  #:export (install-helix-input-states!
            helix-command-definitions
            install-helix-keymaps!))

(define helix-normal-keymap 'helix.normal)
(define helix-select-keymap 'helix.select)
(define helix-insert-keymap 'helix.insert)

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
           (selected (motion-selection host view (view-selection host view)
                                       motion count extend?)))
      (reset-preferred-column! host view)
      (request-redraw! host)
      (command-completed/selection selected))))

(define (thing-start-command host extent display)
  (lambda (context invocation)
    (let ((view (context-view context)))
      (read-key-then!
       host view
       (lambda (key)
         (let ((thing (thing-for-key key)))
           (if thing
               (command-dispatch "helix.thing-commit"
                                 (symbol->string extent)
                                 (symbol->string thing))
               (begin
                 (set-message! host (string-append "undefined thing key: " key))
                 'consume))))
       #:sequence display
       #:hints thing-hints)
      (command-completed/preserve))))

(define (thing-commit-command host)
  (lambda (context invocation)
    (let ((arguments (invocation-arguments invocation)))
      (if (not (= (vector-length arguments) 2))
          (command-error "thing capture requires an extent and thing")
          (let* ((extent (string->symbol (vector-ref arguments 0)))
                 (thing (vector-ref arguments 1))
                 (view (context-view context))
                 (selected (thing-selection host view (view-selection host view)
                                            thing extent)))
            (if selected
                (command-completed/selection selected)
                (command-error (string-append "no " thing
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
    #:keymaps (vector helix-normal-keymap)
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "NOR")
  (define-input-state! host 'hx-select
    #:keymaps (vector helix-select-keymap)
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "SEL")
  (define-input-state! host 'hx-insert
    #:keymaps (vector helix-insert-keymap)
    #:indicator "INS")
  (define-input-strategy! host 'helix 'hx-normal 'hx-normal 'preserve)
  3)

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
