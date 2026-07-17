(define-module (cind vim)
  #:use-module (cind command)
  #:use-module (cind host)
  #:use-module (cind input)
  #:export (install-vim-input-states!
            vim-command-definitions
            install-vim-keymaps!))

(define vim-normal-keymap 'vim.normal)
(define vim-insert-keymap 'vim.insert)
(define vim-visual-keymap 'vim.visual)
(define vim-operator-state 'vim-operator)

;; Operator entries are
;; #(host view original-selection operator-count motion-count phase finished?).
(define operator-sessions '())

(define (session-matches? entry host view)
  (and (eq? (vector-ref entry 0) host)
       (equal? (vector-ref entry 1) view)))

(define (find-session sessions host view)
  (let loop ((remaining sessions))
    (and (pair? remaining)
         (let ((entry (car remaining)))
           (if (session-matches? entry host view)
               entry
               (loop (cdr remaining)))))))

(define (remove-matching sessions host view)
  (let loop ((remaining sessions)
             (kept '()))
    (cond ((null? remaining) (reverse kept))
          ((session-matches? (car remaining) host view)
           (append (reverse kept) (cdr remaining)))
          (else (loop (cdr remaining) (cons (car remaining) kept))))))

(define (find-operator-session host view)
  (find-session operator-sessions host view))

(define (remove-operator-session! host view)
  (set! operator-sessions (remove-matching operator-sessions host view)))

(define (start-operator-session! host view original count)
  (remove-operator-session! host view)
  (set! operator-sessions
        (cons (vector host view original (or count 1) #f #f #f)
              operator-sessions)))

(define (digit-value key)
  (and (= (string-length key) 1)
       (let ((character (string-ref key 0)))
         (and (char>=? character #\0)
              (char<=? character #\9)
              (- (char->integer character) (char->integer #\0))))))

(define (digit-command-name prefix digit)
  (string-append prefix (number->string digit)))

(define (motion-for-key key)
  (cond ((string=? key "h") 'cind.backward-character)
        ((string=? key "l") 'cind.forward-character)
        ((string=? key "w") 'cind.forward-word)
        ((string=? key "b") 'cind.backward-word)
        (else #f)))

(define (thing-for-key key)
  (cond ((string=? key "a") 'angle)
        ((string=? key "w") 'word)
        ((string=? key "s") 'string)
        ((string=? key "f") 'defun)
        (else #f)))

(define operator-hints
  (vector (vector "h" "backward character" #f)
          (vector "l" "forward character" #f)
          (vector "w" "forward word" #f)
          (vector "b" "backward word" #f)
          (vector "i" "inner thing" #t)
          (vector "a" "around thing" #t)
          (vector "0" "count (0-9)" #t)
          (vector "\"" "register" #t)))

(define thing-hints
  (vector (vector "a" "angle" #f)
          (vector "w" "word" #f)
          (vector "s" "string" #f)
          (vector "f" "function" #f)))

(define (operator-count session)
  (* (vector-ref session 3) (or (vector-ref session 4) 1)))

(define (finish-operator! host view session selected)
  (set-selection! host view selected)
  (vector-set! session 6 #t)
  (pop-input-state! host view)
  (vector 'dispatch "edit.kill-region"))

(define (operator-motion host view session motion)
  (motion-selection host view (vector-ref session 2)
                    motion (operator-count session) #t))

(define (operator-thing host view session thing extent)
  (thing-selection host view (vector-ref session 2) thing extent))

(define (start-register-capture! host view sequence)
  (read-key-then!
   host view
   (lambda (key)
     (if (= (string-length key) 1)
         (command-dispatch "vim.register-commit" key)
         (begin
           (set-message! host (string-append "invalid register: " key))
           'consume)))
   #:sequence sequence))

(define (operator-handle-key host context key)
  (let* ((view (context-view context))
         (session (find-operator-session host view))
         (digit (digit-value key)))
    (cond ((not session)
           (pop-input-state! host view)
           (set-message! host "operator session is missing")
           'consume)
          (digit
           (let ((count (vector-ref session 4)))
             (vector-set! session 4 (+ (* (or count 0) 10) digit))
             (vector 'dispatch (digit-command-name "vim.operator-digit-" digit))))
          ((string=? key "\"")
           (start-register-capture! host view "d \"")
           'consume)
          ((motion-for-key key)
           (finish-operator! host view session
                             (operator-motion host view session (motion-for-key key))))
          (else
           (pop-input-state! host view)
           (set-message! host (string-append "undefined operator key: " key))
           'consume))))

(define (vim-operator-handler host context key)
  (let* ((view (context-view context))
         (session (find-operator-session host view))
         (phase (and session (vector-ref session 5))))
    (cond ((not session) (operator-handle-key host context key))
          (phase
           (let ((thing (thing-for-key key)))
             (vector-set! session 5 #f)
             (if thing
                 (let ((selected (operator-thing host view session thing phase)))
                   (if selected
                       (finish-operator! host view session selected)
                       (begin
                         (pop-input-state! host view)
                         (set-message! host "thing is not present at point")
                         'consume)))
                 (begin
                   (pop-input-state! host view)
                   (set-message! host (string-append "undefined thing: " key))
                   'consume))))
          ((or (string=? key "i") (string=? key "a"))
           (vector-set! session 5 (if (string=? key "i") 'inner 'bounds))
           (vector 'pending (string-append "d " key) thing-hints))
          (else (operator-handle-key host context key)))))

(define (select-vim-strategy host)
  (lambda (context invocation)
    (set-view-input-strategy! host (context-view context) 'vim)
    (command-completed/collapse)))

(define (select-vim-state host state selection-maker)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (selected (and selection-maker (selection-maker view))))
      (set-base-input-state! host view state)
      (if selected
          (command-completed/selection selected)
          (command-completed/collapse)))))

(define (operator-start-command host)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (original (view-selection host view)))
      (start-operator-session! host view original
                               (invocation-repeat-count invocation))
      (let ((preview (motion-selection host view original
                                       'cind.forward-character 1 #t)))
        (set-selection! host view preview))
      (push-input-state! host view vim-operator-state)
      (set-input-feedback! host view "d" operator-hints)
      (command-prefix (invocation-repeat-count invocation)
                      (invocation-register invocation)
                      (invocation-prefix-extra invocation)))))

(define (register-start-command host)
  (lambda (context invocation)
    (let ((view (context-view context)))
      (start-register-capture! host view "\"")
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

(define (prefix-digit-command digit)
  (lambda (context invocation)
    (let ((count (invocation-repeat-count invocation)))
      (if (and (= digit 0) (not count))
          (command-dispatch "cursor.line-start")
          (command-prefix (+ (* (or count 0) 10) digit)
                          (invocation-register invocation)
                          (invocation-prefix-extra invocation))))))

(define (operator-digit-command host digit)
  (lambda (context invocation)
    (let ((session (find-operator-session host (context-view context))))
      (if session
          (command-prefix (operator-count session)
                          (invocation-register invocation)
                          (invocation-prefix-extra invocation))
          (command-error "operator count session is missing")))))

(define (extend-motion-command host motion)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (count (or (invocation-repeat-count invocation) 1))
           (selected (motion-selection host view (view-selection host view)
                                       motion count #t)))
      (request-redraw! host)
      (command-completed/selection selected))))

(define (visual-delete-command host)
  (lambda (context invocation)
    (set-base-input-state! host (context-view context) 'vim-normal)
    (command-dispatch "edit.kill-region")))

(define (operator-state-exit host event)
  (let* ((view (vector-ref event 1))
         (session (find-operator-session host view)))
    (when session
      (unless (vector-ref session 6)
        (set-selection! host view (vector-ref session 2)))
      (remove-operator-session! host view))))

(define (digit-command-definitions host)
  (let loop ((digit 0)
             (definitions '()))
    (if (> digit 9)
        (reverse definitions)
        (loop (+ digit 1)
              (cons (list (digit-command-name "vim.operator-digit-" digit)
                          (operator-digit-command host digit) #f)
                    (cons (list (digit-command-name "vim.prefix-digit-" digit)
                                (prefix-digit-command digit) #f)
                          definitions))))))

(define (vim-command-definitions host)
  (append
   (list (list "vim.enter-normal" (select-vim-strategy host) #f)
         (list "vim.enter-insert"
               (select-vim-state host 'vim-insert #f) #f)
         (list "vim.enter-visual"
               (select-vim-state
                host 'vim-visual
                (lambda (view)
                  (motion-selection host view (view-selection host view)
                                    'cind.forward-character 1 #t)))
               #f)
         (list "vim.operator-delete" (operator-start-command host) #f)
         (list "vim.register-start" (register-start-command host) #f)
         (list "vim.register-commit" (register-commit-command host) #f)
         (list "vim.visual-delete" (visual-delete-command host) #f)
         (list "vim.extend-backward-character"
               (extend-motion-command host 'cind.backward-character) #f)
         (list "vim.extend-forward-character"
               (extend-motion-command host 'cind.forward-character) #f)
         (list "vim.extend-backward-word"
               (extend-motion-command host 'cind.backward-word) #f)
         (list "vim.extend-forward-word"
               (extend-motion-command host 'cind.forward-word) #f))
   (digit-command-definitions host)))

(define (install-vim-input-states! host)
  (define-keymap! host vim-normal-keymap #f)
  (define-keymap! host vim-insert-keymap #f)
  (define-keymap! host vim-visual-keymap #f)
  (define-input-state! host 'vim-normal
    #:keymaps (vector vim-normal-keymap)
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "N")
  (define-input-state! host 'vim-insert
    #:keymaps (vector vim-insert-keymap)
    #:indicator "I")
  (define-input-state! host 'vim-visual
    #:keymaps (vector vim-visual-keymap)
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "V")
  (define-input-state! host vim-operator-state
    #:text-input 'ignore
    #:cursor 'block
    #:indicator "O"
    #:handler (lambda (context key) (vim-operator-handler host context key))
    #:on-exit (lambda (event) (operator-state-exit host event)))
  (define-input-strategy! host 'vim 'vim-normal 'vim-normal 'collapse)
  4)

(define digit-bindings
  '(("0" . "vim.prefix-digit-0")
    ("1" . "vim.prefix-digit-1")
    ("2" . "vim.prefix-digit-2")
    ("3" . "vim.prefix-digit-3")
    ("4" . "vim.prefix-digit-4")
    ("5" . "vim.prefix-digit-5")
    ("6" . "vim.prefix-digit-6")
    ("7" . "vim.prefix-digit-7")
    ("8" . "vim.prefix-digit-8")
    ("9" . "vim.prefix-digit-9")))

(define normal-bindings
  (append digit-bindings
          '(("\"" . "vim.register-start")
            ("h" . "cursor.backward-character")
            ("j" . "cursor.next-line")
            ("k" . "cursor.previous-line")
            ("l" . "cursor.forward-character")
            ("w" . "cursor.forward-word")
            ("b" . "cursor.backward-word")
            ("$" . "cursor.line-end")
            ("i" . "vim.enter-insert")
            ("v" . "vim.enter-visual")
            ("d" . "vim.operator-delete")
            ("p" . "edit.yank")
            ("u" . "edit.undo")
            ("C-r" . "edit.redo"))))

(define insert-bindings
  '(("ESC" . "vim.enter-normal")))

(define visual-bindings
  '(("h" . "vim.extend-backward-character")
    ("l" . "vim.extend-forward-character")
    ("w" . "vim.extend-forward-word")
    ("b" . "vim.extend-backward-word")
    ("d" . "vim.visual-delete")
    ("ESC" . "vim.enter-normal")))

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

(define (install-vim-keymaps! host)
  (define-keymap! host vim-normal-keymap #f)
  (define-keymap! host vim-insert-keymap #f)
  (define-keymap! host vim-visual-keymap #f)
  (+ (bind-all! host vim-normal-keymap normal-bindings)
     (bind-all! host vim-insert-keymap insert-bindings)
     (bind-all! host vim-visual-keymap visual-bindings)))
