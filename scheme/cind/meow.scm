(define-module (cind meow)
  #:use-module (srfi srfi-13)
  #:use-module (cind application)
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
(define meow-numeric-state 'meow-numeric)

;; Keypad entries are #(host view translated-keys raw-keys pending-modifier),
;; while numeric entries are #(host view sign magnitude). The host capability
;; is part of each key because Guile modules are process global while editor
;; runtimes are independent.
(define keypad-sessions '())
(define numeric-sessions '())
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

(define (find-numeric-session host view)
  (let loop ((sessions numeric-sessions))
    (and (pair? sessions)
         (let ((entry (car sessions)))
           (if (session-matches? entry host view)
               entry
               (loop (cdr sessions)))))))

(define (remove-numeric-session! host view)
  (set! numeric-sessions
        (let loop ((sessions numeric-sessions)
                   (kept '()))
          (cond ((null? sessions) (reverse kept))
                ((session-matches? (car sessions) host view)
                 (append (reverse kept) (cdr sessions)))
                (else (loop (cdr sessions) (cons (car sessions) kept)))))))

(define (store-numeric-session! host view sign magnitude)
  (remove-numeric-session! host view)
  (let ((session (vector host view sign magnitude)))
    (set! numeric-sessions (cons session numeric-sessions))
    session))

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

(define (metadata-value metadata key)
  (and (list? metadata)
       (let ((entry (assq key metadata)))
         (and entry (cdr entry)))))

(define (meow-metadata type . motions)
  (append (list (cons 'strategy 'meow)
                (cons 'type type))
          (if (pair? motions)
              (list (cons 'expand (car motions)))
              '())))

(define (meow-selection-type selected)
  (let ((metadata (selection-metadata selected)))
    (and (eq? (metadata-value metadata 'strategy) 'meow)
         (metadata-value metadata 'type))))

(define (collapse-selection selected)
  (selection-with-ranges
   selected
   (map (lambda (range)
          (let ((head (selection-range-head range)))
            (selection-range head head (selection-range-granularity range))))
        (vector->list (selection-ranges selected)))))

(define (orient-selection selected backward?)
  (selection-with-ranges
   selected
   (map (lambda (range)
          (let ((anchor (selection-range-anchor range))
                (head (selection-range-head range))
                (granularity (selection-range-granularity range)))
            (if (or (= anchor head)
                    (eq? backward? (< head anchor)))
                range
                (selection-range head anchor granularity))))
        (vector->list (selection-ranges selected)))))

(define (with-meow-metadata selected type . motions)
  (selection-with-metadata
   selected
   (apply meow-metadata type motions)))

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

(define (finish-numeric! host view)
  (remove-numeric-session! host view)
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
                  (let ((view (context-view context)))
                    (thing-selection host view (view-selection host view)
                                     thing extent))))
            (if selected
                (command-completed/selection
                 (with-meow-metadata selected
                                     (cons 'select (string->symbol thing))))
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

(define (numeric-count session)
  (* (vector-ref session 2) (or (vector-ref session 3) 1)))

(define (ensure-numeric-session! host view sign magnitude)
  (or (find-numeric-session host view)
      (begin
        (push-input-state! host view meow-numeric-state)
        (store-numeric-session! host view sign magnitude))))

(define (prefix-digit-command host digit)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (session (ensure-numeric-session! host view 1 #f))
           (magnitude (vector-ref session 3)))
      (vector-set! session 3 (+ (* (or magnitude 0) 10) digit))
      (command-prefix (numeric-count session)
                      (invocation-register invocation)
                      (invocation-prefix-extra invocation)))))

(define (negative-prefix-command host)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (existing (find-numeric-session host view))
           (session (or existing
                        (begin
                          (push-input-state! host view meow-numeric-state)
                          (store-numeric-session! host view -1 #f)))))
      (when existing
        (vector-set! session 2 (- (vector-ref session 2))))
      (command-prefix (numeric-count session)
                      (invocation-register invocation)
                      (invocation-prefix-extra invocation)))))

(define (digit-value key)
  (and (= (string-length key) 1)
       (char-numeric? (string-ref key 0))
       (- (char->integer (string-ref key 0)) (char->integer #\0))))

(define (numeric-handle-key host context key)
  (let ((digit (digit-value key))
        (view (context-view context)))
    (cond (digit
           (command-dispatch
            (string-append "meow.prefix-digit-" (number->string digit))))
          ((string=? key "-")
           (command-dispatch "meow.negative-argument"))
          (else
           (finish-numeric! host view)
           'pass))))

(define (thing-motions type)
  (cond ((eq? type 'word)
         (cons 'cind.backward-word 'cind.forward-word))
        ((eq? type 'symbol)
         (cons 'cind.backward-symbol 'cind.forward-symbol))
        (else #f)))

(define (primary-backward? selected)
  (let* ((ranges (selection-ranges selected))
         (range (vector-ref ranges (selection-primary selected))))
    (< (selection-range-head range) (selection-range-anchor range))))

(define (primary-head selected)
  (let ((ranges (selection-ranges selected)))
    (selection-range-head
     (vector-ref ranges (selection-primary selected)))))

(define (expand-hint-label amount)
  (number->string (if (= amount 10) 0 amount)))

(define (expand-position-hints host context)
  (let* ((view (context-view context))
         (current (view-selection host view))
         (type (meow-selection-type current))
         (motions (metadata-value (selection-metadata current) 'expand)))
    (if (not (and (pair? type) (pair? motions)))
        (vector)
        (let ((motion (if (primary-backward? current)
                          (car motions)
                          (cdr motions))))
          (let loop ((amount 1)
                     (previous (primary-head current))
                     (hints '()))
            (if (> amount 10)
                (list->vector (reverse hints))
                (let* ((expanded
                        (motion-selection host view current motion amount #t))
                       (position (primary-head expanded)))
                  (if (= position previous)
                      (list->vector (reverse hints))
                      (loop (+ amount 1)
                            position
                            (cons (vector position (expand-hint-label amount))
                                  hints))))))))))

(define (next-thing-command host type backward?)
  (let ((motions (thing-motions type)))
    (lambda (context invocation)
      (let* ((view (context-view context))
             (signed-count (or (invocation-repeat-count invocation) 1))
             (effective-backward? (if (< signed-count 0)
                                      (not backward?)
                                      backward?))
             (count (abs signed-count))
             (current (view-selection host view))
             (expand? (equal? (meow-selection-type current)
                              (cons 'expand type)))
             (source (orient-selection
                      (if expand? current (collapse-selection current))
                      effective-backward?))
             (motion (if effective-backward? (car motions) (cdr motions)))
             (selected (motion-selection host view source motion count #t)))
        (reset-preferred-column! host view)
        (request-redraw! host)
        (command-completed/selection
         (with-meow-metadata selected
                             (cons (if expand? 'expand 'select) type)
                             motions))))))

(define (mark-thing-command host type)
  (let ((motions (thing-motions type)))
    (lambda (context invocation)
      (let* ((view (context-view context))
             (current (collapse-selection (view-selection host view)))
             (selected (thing-selection host view current type 'inner)))
        (if selected
            (begin
              (request-redraw! host)
              (command-completed/selection
               (with-meow-metadata selected (cons 'expand type) motions)))
            (command-error
             (string-append "no " (symbol->string type)
                            " thing at every selection")))))))

(define (expand-command host amount)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (current (view-selection host view))
           (type (meow-selection-type current))
           (motions (metadata-value (selection-metadata current) 'expand)))
      (if (and (pair? type) (pair? motions))
          (let* ((backward? (primary-backward? current))
                 (motion (if backward? (car motions) (cdr motions)))
                 (count (if (= amount 0) 10 amount))
                 (selected (motion-selection host view current motion count #t)))
            (request-redraw! host)
            (command-completed/selection
             (with-meow-metadata selected
                                 (cons 'expand (cdr type))
                                 motions)))
          (command-error "no expandable Meow selection")))))

(define (expand-command-definitions host)
  (let loop ((digit 0)
             (definitions '()))
    (if (> digit 9)
        (reverse definitions)
        (loop (+ digit 1)
              (cons (list (string-append "meow.expand-"
                                         (number->string digit))
                          (expand-command host digit)
                          #f)
                    definitions)))))

(define (digit-command-definitions host)
  (let loop ((digit 0)
             (definitions '()))
    (if (> digit 9)
        (reverse definitions)
        (loop (+ digit 1)
              (cons (list (string-append "meow.prefix-digit-"
                                         (number->string digit))
                          (prefix-digit-command host digit)
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
         (list "meow.thing-commit" (thing-commit-command host) #f)
         (list "meow.negative-argument" (negative-prefix-command host) #f)
         (list "meow.next-word" (next-thing-command host 'word #f) #f)
         (list "meow.back-word" (next-thing-command host 'word #t) #f)
         (list "meow.next-symbol" (next-thing-command host 'symbol #f) #f)
         (list "meow.back-symbol" (next-thing-command host 'symbol #t) #f)
         (list "meow.mark-word" (mark-thing-command host 'word) #f)
         (list "meow.mark-symbol" (mark-thing-command host 'symbol) #f))
   (digit-command-definitions host)
   (expand-command-definitions host)))

(define (install-meow-input-states! host)
  (define-keymap! host meow-normal-keymap #f)
  (define-keymap! host meow-motion-keymap #f)
  (define-keymap! host meow-insert-keymap #f)
  (define-input-state! host 'meow-normal
    #:keymaps (vector meow-normal-keymap)
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "N"
    #:position-hints (lambda (context) (expand-position-hints host context)))
  (define-input-state! host 'meow-motion
    #:keymaps (vector meow-motion-keymap)
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "M")
  (define-input-state! host 'meow-insert
    #:keymaps (vector meow-insert-keymap)
    #:indicator "I")
  (define-input-state! host meow-keypad-state
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "K"
    #:handler (lambda (context key) (keypad-handle-key host context key))
    #:on-exit (lambda (event) (remove-session! host (vector-ref event 1))))
  (define-input-state! host meow-numeric-state
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "N"
    #:handler (lambda (context key) (numeric-handle-key host context key))
    #:on-exit (lambda (event) (remove-numeric-session! host (vector-ref event 1))))
  (define-input-strategy! host 'meow 'meow-normal 'meow-motion 'collapse)
  5)

(define keypad-bindings
  '(("SPC" . "meow.keypad-leader")
    ("x" . "meow.keypad-x")
    ("c" . "meow.keypad-c")
    ("g" . "meow.keypad-g")
    ("m" . "meow.keypad-m")))

(define digit-bindings
  '(("0" . "meow.expand-0")
    ("1" . "meow.expand-1")
    ("2" . "meow.expand-2")
    ("3" . "meow.expand-3")
    ("4" . "meow.expand-4")
    ("5" . "meow.expand-5")
    ("6" . "meow.expand-6")
    ("7" . "meow.expand-7")
    ("8" . "meow.expand-8")
    ("9" . "meow.expand-9")))

(define normal-bindings
  (append keypad-bindings digit-bindings
          '(("\"" . "meow.register-start")
            ("-" . "meow.negative-argument")
            ("," . "meow.thing-inner")
            ("." . "meow.thing-bounds")
            ("[" . "meow.mark-word")
            ("]" . "meow.mark-symbol")
            ("h" . "cursor.backward-character")
            ("j" . "cursor.next-line")
            ("k" . "cursor.previous-line")
            ("l" . "cursor.forward-character")
            ("w" . "meow.next-word")
            ("W" . "meow.next-symbol")
            ("b" . "meow.back-word")
            ("B" . "meow.back-symbol")
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
