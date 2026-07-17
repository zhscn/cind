(define-module (cind meow)
  #:use-module (srfi srfi-13)
  #:use-module (cind command)
  #:use-module (cind host)
  #:use-module (cind input)
  #:export (install-meow-input-states!
            meow-command-definitions
            install-meow-keymaps!
            char-thing-table-add!))

(define meow-normal-keymap 'meow.normal)
(define meow-motion-keymap 'meow.motion)
(define meow-insert-keymap 'meow.insert)
(define meow-keypad-state 'meow-keypad)

;; Each entry is #(host view translated-keys raw-keys pending-modifier).
;; The host capability is part of the key because Guile modules are process
;; global while editor runtimes are independent.
(define keypad-sessions '())
(define char-thing-tables '())

(define (session-matches? entry host view)
  (and (eq? (vector-ref entry 0) host)
       (equal? (vector-ref entry 1) view)))

(define (find-session host view)
  (let loop ((sessions keypad-sessions))
    (and (pair? sessions)
         (let ((entry (car sessions)))
           (if (session-matches? entry host view)
               entry
               (loop (cdr sessions)))))))

(define (remove-session! host view)
  (set! keypad-sessions
        (let loop ((sessions keypad-sessions)
                   (kept '()))
          (cond ((null? sessions) (reverse kept))
                ((session-matches? (car sessions) host view)
                 (append (reverse kept) (cdr sessions)))
                (else (loop (cdr sessions) (cons (car sessions) kept)))))))

(define (store-session! host view translated raw modifier)
  (remove-session! host view)
  (set! keypad-sessions
        (cons (vector host view translated raw modifier) keypad-sessions)))

(define (normalized-thing-key key)
  (cond ((char? key) (string key))
        ((and (string? key) (= (string-length key) 1)) key)
        (else (error "thing table key must be one character" key))))

(define (char-thing-table-add! table key thing)
  (let ((normalized (normalized-thing-key key)))
    (set! char-thing-tables
          (cons (list table normalized thing)
                (let loop ((entries char-thing-tables)
                           (kept '()))
                  (cond ((null? entries) (reverse kept))
                        ((and (eq? (caar entries) table)
                              (string=? (cadar entries) normalized))
                         (append (reverse kept) (cdr entries)))
                        (else (loop (cdr entries) (cons (car entries) kept)))))))
    thing))

(define (thing-for-key table key)
  (let loop ((entries char-thing-tables))
    (and (pair? entries)
         (let ((entry (car entries)))
           (if (and (eq? (car entry) table) (string=? (cadr entry) key))
               (caddr entry)
               (loop (cdr entries)))))))

(define (key-sequence keys)
  (string-join keys " "))

(define (modified-key? key)
  (or (string-prefix? "C-" key)
      (string-prefix? "M-" key)
      (string-prefix? "S-" key)
      (string-prefix? "s-" key)))

(define (control-key key)
  (if (modified-key? key) key (string-append "C-" key)))

(define (resolution-kind resolution)
  (vector-ref resolution 0))

(define (pending-result host context translated)
  (let* ((sequence (key-sequence translated))
         (layers (base-keymap-layers host context)))
    (vector 'pending sequence
            (key-sequence-completions host layers sequence))))

(define (publish-feedback! host context translated display)
  (let ((layers (base-keymap-layers host context)))
    (set-input-feedback!
     host
     (context-view context)
     display
     (if (null? translated)
         (vector)
         (key-sequence-completions host layers (key-sequence translated))))))

(define (finish-keypad! host view)
  (remove-session! host view)
  (pop-input-state! host view))

(define (resolved-action host context translated raw modifier resolution)
  (let ((view (context-view context)))
    (cond ((eq? (resolution-kind resolution) 'command)
           (finish-keypad! host view)
           (vector 'dispatch (vector-ref resolution 1)))
          ((eq? (resolution-kind resolution) 'prefix)
           (store-session! host view translated raw modifier)
           (pending-result host context translated))
          (else #f))))

(define (keypad-handle-key host context key)
  (let* ((view (context-view context))
         (session (find-session host view)))
    (if (not session)
        (begin
          (pop-input-state! host view)
          (set-message! host "keypad session is missing")
          'consume)
        (let* ((translated (vector-ref session 2))
               (raw (append (vector-ref session 3) (list key)))
               (modifier (vector-ref session 4))
               (translated-key (if modifier
                                   (string-append modifier key)
                                   (control-key key)))
               (candidate (append translated (list translated-key)))
               (layers (base-keymap-layers host context))
               (candidate-resolution
                (resolve-key-sequence host layers (key-sequence candidate)))
               (candidate-action
                (resolved-action host context candidate raw #f candidate-resolution)))
          (if candidate-action
              candidate-action
              (let* ((literal-candidate
                      (and (not modifier)
                           (not (string=? translated-key key))
                           (append translated (list key))))
                     (literal-resolution
                      (and literal-candidate
                           (resolve-key-sequence
                            host layers (key-sequence literal-candidate))))
                     (literal-action
                      (and literal-resolution
                           (resolved-action host context literal-candidate raw #f
                                            literal-resolution))))
                (if literal-action
                    literal-action
                    (let* ((transparent-resolution
                            (resolve-key-sequence host layers (key-sequence raw)))
                           (transparent-action
                            (resolved-action host context raw raw #f
                                             transparent-resolution)))
                      (if transparent-action
                          transparent-action
                          (begin
                            (finish-keypad! host view)
                            (set-message!
                             host
                             (string-append "undefined keypad key: "
                                            (key-sequence raw)))
                            'consume))))))))))

(define (keypad-start! host context start)
  (let* ((view (context-view context))
         (translated
          (cond ((eq? start 'x) '("C-x"))
                ((eq? start 'c) '("C-c"))
                ((eq? start 'leader) '("C-c"))
                (else '())))
         (raw (list (if (eq? start 'leader) "SPC" (symbol->string start))))
         (modifier (cond ((eq? start 'm) "M-")
                         ((eq? start 'g) "C-M-")
                         (else #f)))
         (display (if modifier modifier (key-sequence translated))))
    (store-session! host view translated raw modifier)
    (push-input-state! host view meow-keypad-state)
    (publish-feedback! host context translated display)
    (command-completed)))

(define (register-start-command host)
  (lambda (context invocation)
    (let ((view (context-view context)))
      (read-key-then!
       host view
       (lambda (key)
         (if (= (string-length key) 1)
             (command-dispatch "meow.register-commit" key)
             (begin
               (set-message! host (string-append "invalid register: " key))
               'consume)))
       #:sequence "\"")
      (command-prefix (invocation-repeat-count invocation)
                      (invocation-register invocation)
                      (invocation-prefix-extra invocation)))))

(define (register-commit-command host)
  (lambda (context invocation)
    (let ((arguments (invocation-arguments invocation)))
      (if (not (= (vector-length arguments) 1))
          (command-error "register capture requires one key")
          (command-prefix (invocation-repeat-count invocation)
                          (vector-ref arguments 0)
                          (invocation-prefix-extra invocation))))))

(define (thing-start-command host extent display)
  (lambda (context invocation)
    (let ((view (context-view context)))
      (read-key-then!
       host view
       (lambda (key)
         (let ((thing (and (= (string-length key) 1)
                           (thing-for-key 'meow key))))
           (if thing
               (command-dispatch "meow.thing-commit"
                                 (symbol->string extent)
                                 (symbol->string thing))
               (begin
                 (set-message! host (string-append "undefined thing key: " key))
                 'consume))))
       #:sequence display)
      (command-completed/preserve))))

(define (thing-commit-command host)
  (lambda (context invocation)
    (let ((arguments (invocation-arguments invocation)))
      (if (not (= (vector-length arguments) 2))
          (command-error "thing capture requires an extent and thing")
          (let* ((extent (string->symbol (vector-ref arguments 0)))
                 (thing (vector-ref arguments 1))
                 (selected
                  (thing-selection host (context-view context) thing extent)))
            (if selected
                (command-completed/selection selected)
                (command-error (string-append "no " thing
                                              " thing at point"))))))))

(define (select-meow-state host state)
  (lambda (context invocation)
    (if state
        (set-base-input-state! host (context-view context) state)
        (set-view-input-strategy! host (context-view context) 'meow))
    (command-completed)))

(define (keypad-command host start)
  (lambda (context invocation)
    (keypad-start! host context start)))

(define (prefix-digit-command digit)
  (lambda (context invocation)
    (let ((count (invocation-repeat-count invocation)))
      (if (and (= digit 0) (not count))
          (command-dispatch "cursor.line-start")
          (command-prefix (+ (* (if count count 0) 10) digit)
                          (invocation-register invocation)
                          (invocation-prefix-extra invocation))))))

(define (digit-command-definitions)
  (let loop ((digit 0)
             (definitions '()))
    (if (> digit 9)
        (reverse definitions)
        (loop (+ digit 1)
              (cons (list (string-append "meow.prefix-digit-"
                                         (number->string digit))
                          (prefix-digit-command digit)
                          #f)
                    definitions)))))

(define (meow-command-definitions host)
  (append
   (list (list "meow.normal-mode" (select-meow-state host #f) #f)
         (list "meow.insert-mode" (select-meow-state host 'meow-insert) #f)
         (list "meow.keypad-leader" (keypad-command host 'leader) #f)
         (list "meow.keypad-x" (keypad-command host 'x) #f)
         (list "meow.keypad-c" (keypad-command host 'c) #f)
         (list "meow.keypad-g" (keypad-command host 'g) #f)
         (list "meow.keypad-m" (keypad-command host 'm) #f)
         (list "meow.register-start" (register-start-command host) #f)
         (list "meow.register-commit" (register-commit-command host) #f)
         (list "meow.thing-inner" (thing-start-command host 'inner ",") #f)
         (list "meow.thing-bounds" (thing-start-command host 'bounds ".") #f)
         (list "meow.thing-commit" (thing-commit-command host) #f))
   (digit-command-definitions)))

(define (install-meow-input-states! host)
  (define-keymap! host meow-normal-keymap #f)
  (define-keymap! host meow-motion-keymap #f)
  (define-keymap! host meow-insert-keymap #f)
  (define-input-state! host 'meow-normal
    (vector meow-normal-keymap) 'ignore 'block "N" #f)
  (define-input-state! host 'meow-motion
    (vector meow-motion-keymap) 'ignore 'block "M" #f)
  (define-input-state! host 'meow-insert
    (vector meow-insert-keymap) 'accept 'beam "I" #f)
  (define-input-state! host meow-keypad-state
    (vector) 'ignore 'block "K"
    (lambda (context key) (keypad-handle-key host context key)))
  (define-input-strategy! host 'meow 'meow-normal 'meow-motion 'collapse)
  (observe-input-state-changes!
   host
   (lambda (event)
     (when (eq? (vector-ref event 0) 'pop)
       (let ((view (vector-ref event 1))
             (state (vector-ref event 2)))
         (when (eq? state meow-keypad-state)
           (remove-session! host view))))))
  4)

(define keypad-bindings
  '(("SPC" . "meow.keypad-leader")
    ("x" . "meow.keypad-x")
    ("c" . "meow.keypad-c")
    ("g" . "meow.keypad-g")
    ("m" . "meow.keypad-m")))

(define digit-bindings
  '(("0" . "meow.prefix-digit-0")
    ("1" . "meow.prefix-digit-1")
    ("2" . "meow.prefix-digit-2")
    ("3" . "meow.prefix-digit-3")
    ("4" . "meow.prefix-digit-4")
    ("5" . "meow.prefix-digit-5")
    ("6" . "meow.prefix-digit-6")
    ("7" . "meow.prefix-digit-7")
    ("8" . "meow.prefix-digit-8")
    ("9" . "meow.prefix-digit-9")))

(define normal-bindings
  (append keypad-bindings digit-bindings
          '(("\"" . "meow.register-start")
            ("," . "meow.thing-inner")
            ("." . "meow.thing-bounds")
            ("h" . "cursor.backward-character")
            ("j" . "cursor.next-line")
            ("k" . "cursor.previous-line")
            ("l" . "cursor.forward-character")
            ("w" . "cursor.forward-word")
            ("b" . "cursor.backward-word")
            ("$" . "cursor.line-end")
            ("i" . "meow.insert-mode"))))

(define motion-bindings keypad-bindings)

(define insert-bindings
  '(("ESC" . "meow.normal-mode")))

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

(define (install-meow-keymaps! host)
  (define-keymap! host meow-normal-keymap #f)
  (define-keymap! host meow-motion-keymap #f)
  (define-keymap! host meow-insert-keymap #f)
  (+ (bind-all! host meow-normal-keymap normal-bindings)
     (bind-all! host meow-motion-keymap motion-bindings)
     (bind-all! host meow-insert-keymap insert-bindings)))

(char-thing-table-add! 'meow #\a 'angle)
(char-thing-table-add! 'meow #\w 'word)
(char-thing-table-add! 'meow #\s 'string)
