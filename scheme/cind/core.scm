(define-module (cind core)
  #:use-module (ice-9 optargs)
  #:use-module (cind command)
  #:use-module (cind development)
  #:use-module (cind emacs)
  #:use-module (cind helix)
  #:use-module (cind host)
  #:use-module (cind input)
  #:use-module (cind introspect)
  #:use-module (cind meow)
  #:use-module (cind structural)
  #:use-module (cind toy-modal)
  #:use-module (cind vim)
  #:export (install-core-commands!
            install-core-providers!
            install-input-states!
            install-core-modes!
            define-major-mode!
            define-minor-mode!
            install-default-keymaps!))

(define (last-string-argument invocation)
  (let ((arguments (invocation-arguments invocation)))
    (and (> (vector-length arguments) 0)
         (let ((argument (vector-ref arguments (- (vector-length arguments) 1))))
           (and (string? argument) argument)))))

(define (string-prefix?* prefix text)
  (and (>= (string-length text) (string-length prefix))
       (string=? prefix (substring text 0 (string-length prefix)))))

(define (string-suffix?* suffix text)
  (and (>= (string-length text) (string-length suffix))
       (string=? suffix
                 (substring text
                            (- (string-length text) (string-length suffix))
                            (string-length text)))))

(define (internal-command? name)
  (or (string-suffix?* ".accept" name)
      (string-prefix?* "interaction." name)))

(define (commands-provider host context query)
  (let ((names (enabled-command-names host context)))
    (let loop ((index 0)
               (candidates '()))
      (if (= index (vector-length names))
          (list->vector (reverse candidates))
          (let ((name (vector-ref names index)))
            (loop (+ index 1)
                  (if (internal-command? name)
                      candidates
                      (cons (interaction-candidate name name "command" name)
                            candidates))))))))

(define (buffers-provider host context query)
  (let ((summaries (open-buffer-summaries host)))
    (let loop ((index 0)
               (candidates '()))
      (if (= index (vector-length summaries))
          (list->vector (reverse candidates))
          (let* ((summary (vector-ref summaries index))
                 (name (vector-ref summary 0))
                 (resource (vector-ref summary 1))
                 (modified? (vector-ref summary 2))
                 (detail (cond ((and resource modified?)
                                (string-append resource " · modified"))
                               (resource resource)
                               (modified? "modified")
                               (else "")))
                 (filter-text (if resource
                                  (string-append name " " resource)
                                  name)))
            (loop (+ index 1)
                  (cons (interaction-candidate name name detail filter-text)
                        candidates)))))))

(define (project-files-provider host context query)
  (let ((project (context-project context)))
    (if (not project)
        (vector)
        (let ((root (project-root host project))
              (files (project-files host project)))
          (if (not root)
              (vector)
              (let loop ((index 0)
                         (candidates '()))
                (if (= index (vector-length files))
                    (list->vector (reverse candidates))
                    (let* ((file (vector-ref files index))
                           (relative-value (path-relative host file root))
                           (relative (if (= (string-length relative-value) 0)
                                         (path-filename host file)
                                         relative-value)))
                      (loop (+ index 1)
                            (cons (interaction-candidate
                                   file relative (path-parent host relative)
                                   (string-append relative " " file))
                                  candidates))))))))))

(define (key-bindings-provider host context query)
  (let ((bindings (active-key-bindings host)))
    (let loop ((index 0)
               (candidates '()))
      (if (= index (vector-length bindings))
          (list->vector (reverse candidates))
          (let* ((binding (vector-ref bindings index))
                 (keys (vector-ref binding 0))
                 (command (vector-ref binding 1)))
            (loop (+ index 1)
                  (cons (interaction-candidate
                         (string-append keys "  " command)
                         keys command (string-append keys " " command))
                        candidates)))))))

(define (command-palette-accept context invocation)
  (let ((name (last-string-argument invocation)))
    (if name
        (command-dispatch name)
        (command-error "command palette requires a command name"))))

(define (command-palette context invocation)
  (interaction 'picker "Command: " "" "commands" "commands" #f
               "command.palette.accept"))

(define (file-open-interaction initial-input)
  (interaction 'picker "Open file: " initial-input "files" "files" #t
               "file.open.accept"))

(define (file-open host context invocation)
  (let* ((resource (buffer-resource host (context-buffer context)))
         (directory (if resource (path-parent host resource) ".")))
    (file-open-interaction (path-as-directory host directory))))

(define (file-open-accept host context invocation)
  (let ((path (last-string-argument invocation)))
    (cond ((not path)
           (command-error "open file requires a path"))
          ((= (string-length path) 0)
           (command-error "file path is empty"))
          ((directory-path? host path)
           (file-open-interaction (path-as-directory host path)))
          (else
           (open-file! host (context-window context) path)
           (command-completed)))))

(define (file-save host context invocation)
  (save-buffer! host (context-buffer context))
  (command-completed))

(define (file-save-as host context invocation)
  (let ((resource (buffer-resource host (context-buffer context))))
    (interaction 'text "Write file: " (if resource resource "") "files" "" #t
                 "file.save-as.accept")))

(define (file-save-as-accept host context invocation)
  (let ((path (last-string-argument invocation)))
    (cond ((not path)
           (command-error "write file requires a path"))
          ((= (string-length path) 0)
           (command-error "file path is empty"))
          (else
           (set-buffer-resource! host (context-buffer context) path)
           (command-dispatch "file.save")))))

(define (buffer-switch context invocation)
  (interaction 'picker "Switch buffer: " "" "buffers" "buffers" #f
               "buffer.switch.accept"))

(define (buffer-switch-accept host context invocation)
  (let ((name (last-string-argument invocation)))
    (if (not name)
        (command-error "switch buffer requires a buffer name")
        (let ((buffer (buffer-id-by-name host name)))
          (if buffer
              (begin
                (display-buffer! host (context-window context) buffer)
                (command-completed))
              (command-error (string-append "unknown buffer '" name "'")))))))

(define (vector-index-of values target)
  (let loop ((index 0))
    (cond ((= index (vector-length values)) #f)
          ((equal? (vector-ref values index) target) index)
          (else (loop (+ index 1))))))

(define (buffer-switch-relative host context delta)
  (let* ((buffers (open-buffer-ids host))
         (count (vector-length buffers)))
    (if (< count 2)
        (command-completed)
        (let ((current (vector-index-of buffers (context-buffer context))))
          (if (not current)
              (command-error "current buffer is not open")
              (begin
                (display-buffer! host (context-window context)
                                 (vector-ref buffers (modulo (+ current delta) count)))
                (command-completed)))))))

(define (buffer-kill host context force?)
  (let ((error (kill-buffer! host (context-buffer context) force?)))
    (if error
        (command-error error)
        (command-completed))))

(define (completed-or-error error)
  (if error (command-error error) (command-completed)))

(define (application-quit host force?)
  (request-quit! host force?)
  (command-completed))

(define (window-split host context axis)
  (completed-or-error (split-window! host (context-window context) axis)))

(define (window-delete host context)
  (completed-or-error (delete-window! host (context-window context))))

(define (window-delete-others host context)
  (delete-other-windows! host (context-window context))
  (command-completed))

(define (window-other host context)
  (completed-or-error
   (select-other-window! host (context-window context) 1)))

(define (editor-redraw host)
  (request-redraw! host)
  (command-completed))

(define (goto-line context invocation)
  (interaction 'text "Go to line: " "" "line-numbers" "" #t
               "cursor.goto-line.accept"))

(define max-uint32 4294967295)

(define (decimal-uint32 text)
  (and (> (string-length text) 0)
       (let loop ((index 0))
         (if (= index (string-length text))
             (let ((value (string->number text)))
               (and value (<= value max-uint32) value))
             (let ((character (string-ref text index)))
               (and (char>=? character #\0)
                    (char<=? character #\9)
                    (loop (+ index 1))))))))

(define (position-separator text)
  (let loop ((index 0))
    (if (= index (string-length text))
        #f
        (let ((character (string-ref text index)))
          (if (or (char=? character #\:)
                  (char=? character #\,))
              index
              (loop (+ index 1)))))))

(define (goto-line-accept host context invocation)
  (let ((input (last-string-argument invocation)))
    (if (or (not input) (= (string-length input) 0))
        (command-error "line number is empty")
        (let* ((separator (position-separator input))
               (line-text (if separator (substring input 0 separator) input))
               (column-text (if separator
                                (substring input (+ separator 1) (string-length input))
                                ""))
               (line (decimal-uint32 line-text))
               (column (if (= (string-length column-text) 0)
                           1
                           (decimal-uint32 column-text))))
          (cond ((or (not line) (= line 0))
                 (command-error "invalid line number"))
                ((or (not column) (= column 0))
                 (command-error "invalid column number"))
                (else
                 (move-caret-to-line! host (context-view context)
                                      (- line 1) (- column 1))
                 (command-completed)))))))

(define (project-search context invocation)
  (interaction 'text "Project search: " "" "project-search" "" #t
               "project.search.accept"))

(define (project-search-accept host context invocation)
  (let ((query (last-string-argument invocation))
        (project (context-project context)))
    (cond ((or (not query) (= (string-length query) 0))
           (command-error "project search query is empty"))
          ((not project)
           (command-error "current buffer has no project"))
          (else
           (start-project-search! host project (context-window context) query)
           (command-completed)))))

(define (project-find-file host context invocation)
  (let ((project (context-project context)))
    (if (not project)
        (command-error "current buffer has no project")
        (begin
          (ensure-project-index! host project)
          (interaction 'picker "Project file: " "" "project-files"
                       "project-files" #f "project.find-file.accept")))))

(define (project-find-file-accept host context invocation)
  (let ((path (last-string-argument invocation)))
    (cond ((not path)
           (command-error "project file picker requires a path"))
          ((= (string-length path) 0)
           (command-error "file path is empty"))
          (else
           (open-file! host (context-window context) path)
           (command-completed)))))

(define (help-keys context invocation)
  (interaction 'picker "Key bindings: " "" "key-bindings" "key-bindings" #f
               "help.keys.accept"))

(define (help-keys-accept host context invocation)
  (let ((binding (last-string-argument invocation)))
    (if binding
        (set-message! host binding))
    (command-completed)))

(define (context-has-project? context)
  (and (context-project context) #t))

(define kill-ring-limit 60)

(define (take-at-most values count)
  (if (or (= count 0) (null? values))
      '()
      (cons (car values) (take-at-most (cdr values) (- count 1)))))

(define (range-start range)
  (min (vector-ref range 0) (vector-ref range 1)))

(define (range-end range)
  (max (vector-ref range 0) (vector-ref range 1)))

(define (make-region-commands host)
  (let ((kill-ring '())
        (registers '()))
    (define (register-ref name)
      (let ((entry (assoc name registers)))
        (and entry (cdr entry))))

    (define (register-set! name entry)
      (set! registers
            (cons (cons name entry)
                  (let loop ((entries registers)
                             (kept '()))
                    (cond ((null? entries) (reverse kept))
                          ((string=? (caar entries) name)
                           (append (reverse kept) (cdr entries)))
                          (else (loop (cdr entries) (cons (car entries) kept))))))))

    (define (entry-clipboard-text entry)
      (let loop ((index 0)
                 (text ""))
        (if (= index (vector-length entry))
            text
            (loop (+ index 1)
                  (string-append text
                                 (if (or (= index 0) (string-suffix?* "\n" text)) "" "\n")
                                 (vector-ref entry index))))))

    (define (remember-kill! entry invocation)
      (set! kill-ring
            (cons entry (take-at-most kill-ring (- kill-ring-limit 1))))
      (let ((register (invocation-register invocation)))
        (when register (register-set! register entry)))
      (write-clipboard! host (entry-clipboard-text entry)))

    (define (latest-kill invocation)
      (let ((register (invocation-register invocation)))
        (if register
            (let ((entry (register-ref register)))
              (if entry
                  (vector entry #f)
                  (vector #f (string-append "register " register " is empty"))))
            (if (pair? kill-ring)
                (vector (car kill-ring) #f)
                (let* ((result (read-clipboard host))
                       (text (vector-ref result 0))
                       (error (vector-ref result 1)))
                  (if (and text (> (string-length text) 0))
                      (begin
                        (let ((entry (vector text)))
                          (set! kill-ring (list entry))
                          (vector entry #f)))
                      (vector #f error)))))))

    (define (toggle-mark context invocation)
      (let* ((view (context-view context))
             (caret (view-caret host view))
             (mark (view-mark host view)))
        (if (and mark (= mark caret))
            (begin
              (set-message! host "mark cleared")
              (command-completed/collapse))
            (begin
              (set-message! host "mark set")
              (command-completed/selection
               (selection (list (selection-range caret caret 'char))))))))

    (define (kill-range! context range invocation)
      (let* ((buffer (context-buffer context))
             (view (context-view context))
             (text (buffer-substring host buffer
                                     (range-start range) (range-end range))))
        (erase-range! host view (range-start range) (range-end range))
        (let ((clipboard-error (remember-kill! (vector text) invocation)))
          (if clipboard-error
              (set-message! host
                            (string-append "killed internally; clipboard: "
                                           clipboard-error))))
        (command-completed)))

    (define (kill-region context invocation)
      (let ((view (context-view context)))
        (if (view-mark host view)
            (let* ((selected (view-selection host view))
                   (texts (selection-texts host view selected)))
              (replace-selection! host view selected "")
              (request-redraw! host)
              (let ((clipboard-error (remember-kill! texts invocation)))
                (if clipboard-error
                    (set-message! host
                                  (string-append "killed internally; clipboard: "
                                                 clipboard-error))))
              (command-completed))
            (begin
              (set-message! host "no active region")
              (command-completed)))))

    (define (kill-line context invocation)
      (let ((range (soft-kill-range host (context-view context))))
        (if range
            (kill-range! context range invocation)
            (command-completed))))

    (define (copy-region context invocation)
      (let ((view (context-view context)))
        (if (not (view-mark host view))
            (begin
              (set-message! host "no active region")
              (command-completed))
            (let* ((selected (view-selection host view))
                   (texts (selection-texts host view selected))
                   (clipboard-error (remember-kill! texts invocation)))
              (set-message! host
                            (if clipboard-error
                                (string-append "copied internally; clipboard: "
                                               clipboard-error)
                                "copied"))
              (command-completed/collapse)))))

    (define (yank context invocation)
      (let* ((result (latest-kill invocation))
             (entry (vector-ref result 0))
             (clipboard-error (vector-ref result 1)))
        (cond (entry
               (let* ((view (context-view context))
                      (range-count
                       (vector-length (vector-ref (view-selection host view) 3)))
                      (entry-count (vector-length entry)))
                 (insert-text! host view
                               (cond ((= entry-count 1) (vector-ref entry 0))
                                     ((= entry-count range-count) entry)
                                     (else (entry-clipboard-text entry))))))
              (clipboard-error
               (set-message! host
                             (if (invocation-register invocation)
                                 clipboard-error
                                 (string-append "kill ring is empty; clipboard: "
                                                clipboard-error))))
              (else
               (set-message! host "kill ring and clipboard are empty")))
        (command-completed)))

    (define (delete-selection context invocation)
      (let* ((view (context-view context))
             (result (replace-selection! host view (view-selection host view) "")))
        (request-redraw! host)
        (command-completed/selection result)))

    (list (list "selection.toggle-mark" toggle-mark #f)
          (list "edit.delete-selection" delete-selection #f)
          (list "edit.kill-region" kill-region #f)
          (list "edit.kill-line" kill-line #f)
          (list "edit.copy-region" copy-region #f)
          (list "edit.yank" yank #f))))

(define (make-motion-command host motion extend?)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (count (or (invocation-repeat-count invocation) 1))
           (selection (motion-selection host view (view-selection host view)
                                       motion count extend?)))
      (reset-preferred-column! host view)
      (request-redraw! host)
      (command-completed/selection selection))))

(define (core-commands host)
  (append
   (list (list "command.palette.accept" command-palette-accept #f)
        (list "command.palette" command-palette #f)
        (list "file.open.accept"
              (lambda (context invocation)
                (file-open-accept host context invocation))
              #f)
        (list "file.open"
              (lambda (context invocation)
                (file-open host context invocation))
              #f)
        (list "file.save"
              (lambda (context invocation)
                (file-save host context invocation))
              #f)
        (list "file.save-as.accept"
              (lambda (context invocation)
                (file-save-as-accept host context invocation))
              #f)
        (list "file.save-as"
              (lambda (context invocation)
                (file-save-as host context invocation))
              #f)
        (list "buffer.switch.accept"
              (lambda (context invocation)
                (buffer-switch-accept host context invocation))
              #f)
        (list "buffer.switch" buffer-switch #f)
        (list "buffer.next"
              (lambda (context invocation)
                (buffer-switch-relative host context 1))
              #f)
        (list "buffer.previous"
              (lambda (context invocation)
                (buffer-switch-relative host context -1))
              #f)
        (list "buffer.kill"
              (lambda (context invocation)
                (buffer-kill host context #f))
              #f)
        (list "buffer.force-kill"
              (lambda (context invocation)
                (buffer-kill host context #t))
              #f)
        (list "application.quit"
              (lambda (context invocation)
                (application-quit host #f))
              #f)
        (list "application.force-quit"
              (lambda (context invocation)
                (application-quit host #t))
              #f)
        (list "window.split-below"
              (lambda (context invocation)
                (window-split host context 'rows))
              #f)
        (list "window.split-right"
              (lambda (context invocation)
                (window-split host context 'columns))
              #f)
        (list "window.delete"
              (lambda (context invocation)
                (window-delete host context))
              #f)
        (list "window.delete-others"
              (lambda (context invocation)
                (window-delete-others host context))
              #f)
        (list "window.other"
              (lambda (context invocation)
                (window-other host context))
              #f)
        (list "editor.redraw"
              (lambda (context invocation)
                (editor-redraw host))
              #f)
        (list "cursor.forward-expression"
              (make-motion-command host 'cind.forward-expression #f)
              #f)
        (list "cursor.backward-expression"
              (make-motion-command host 'cind.backward-expression #f)
              #f)
        (list "cursor.up-list"
              (make-motion-command host 'cind.up-list #f)
              #f)
        (list "cursor.forward-word"
              (make-motion-command host 'cind.forward-word #f)
              #f)
        (list "cursor.backward-word"
              (make-motion-command host 'cind.backward-word #f)
              #f)
        (list "selection.extend-forward-word"
              (make-motion-command host 'cind.forward-word #t)
              #f)
        (list "cursor.goto-line.accept"
              (lambda (context invocation)
                (goto-line-accept host context invocation))
              #f)
        (list "cursor.goto-line" goto-line #f)
        (list "project.find-file.accept"
              (lambda (context invocation)
                (project-find-file-accept host context invocation))
              #f)
        (list "project.find-file"
              (lambda (context invocation)
                (project-find-file host context invocation))
              context-has-project?)
        (list "project.search.accept"
              (lambda (context invocation)
                (project-search-accept host context invocation))
              #f)
        (list "project.search" project-search context-has-project?)
        (list "help.keys.accept"
              (lambda (context invocation)
                (help-keys-accept host context invocation))
              #f)
        (list "help.keys" help-keys #f))
   (introspection-command-definitions host)
   (development-command-definitions host)
   (make-region-commands host)
   (emacs-command-definitions host)
   (helix-command-definitions host)
   (meow-command-definitions host)
   (structural-command-definitions host)
   (vim-command-definitions host)
   (toy-modal-command-definitions host)))

(define (install-core-commands! host)
  (let ((commands (core-commands host)))
    (for-each (lambda (definition)
                (define-command! host
                                 (list-ref definition 0)
                                 (list-ref definition 1)
                                 (list-ref definition 2)))
              commands)
    (install-introspection-documentation! host)
    (install-development-documentation! host)
    (length commands)))

(define (core-providers host)
  (append
   (list (cons "commands"
               (lambda (context query)
                 (commands-provider host context query)))
         (cons "buffers"
               (lambda (context query)
                 (buffers-provider host context query)))
         (cons "project-files"
               (lambda (context query)
                 (project-files-provider host context query)))
         (cons "key-bindings"
               (lambda (context query)
                 (key-bindings-provider host context query))))
   (introspection-providers host)))

(define (install-core-providers! host)
  (let ((providers (core-providers host)))
    (for-each (lambda (provider)
                (define-interaction-provider! host (car provider) (cdr provider)))
              providers)
    (length providers)))

(define (install-input-states! host)
  (+ (install-read-key-input-state! host)
     (install-emacs-input-state! host)
     (install-helix-input-states! host)
     (install-meow-input-states! host)
     (install-structural-input-state! host)
     (install-vim-input-states! host)
     (install-toy-modal-input-state! host)))

(define* (define-major-mode! host name
                            #:key
                            (parent #f)
                            (keymap #f)
                            (interaction-class #f)
                            (initial-state #f)
                            (things '()))
  (%define-mode! host name 'major parent keymap interaction-class initial-state things))

(define* (define-minor-mode! host name
                            #:key
                            (parent #f)
                            (keymap #f)
                            (interaction-class #f)
                            (initial-state #f)
                            (things '()))
  (%define-mode! host name 'minor parent keymap interaction-class initial-state things))

(define (install-core-modes! host)
  (define-thing! host 'cind.angle '(pair "<" ">"))
  (define-thing! host 'cind.word '(char-class word))
  (define-thing! host 'cind.symbol '(char-class symbol))
  (define-thing! host 'cind.defun '(cst-node function-definition))
  (define-thing! host 'cind.string '(cst-node string-literal))
  (define-motion! host 'cind.forward-character 'forward-character)
  (define-motion! host 'cind.backward-character 'backward-character)
  (define-motion! host 'cind.forward-word 'forward-word)
  (define-motion! host 'cind.backward-word 'backward-word)
  (define-motion! host 'cind.forward-symbol 'forward-symbol)
  (define-motion! host 'cind.backward-symbol 'backward-symbol)
  (define-motion! host 'cind.forward-expression 'forward-expression)
  (define-motion! host 'cind.backward-expression 'backward-expression)
  (define-motion! host 'cind.up-list 'up-list)
  (define-major-mode! host 'fundamental-mode
    #:interaction-class 'editing)
  (define-major-mode! host 'prog-mode
    #:parent 'fundamental-mode
    #:interaction-class 'editing
    #:things '((angle . cind.angle)
               (defun . cind.defun)
               (word . cind.word)
               (symbol . cind.symbol)))
  (define-major-mode! host 'special-mode
    #:parent 'fundamental-mode
    #:interaction-class 'interface)
  3)

(define control-x-bindings
  '(("C-s" . "file.save")
    ("C-w" . "file.save-as")
    ("C-f" . "file.open")
    ("p f" . "project.find-file")
    ("p g" . "project.search")
    ("b" . "buffer.switch")
    ("k" . "buffer.kill")
    ("Right" . "buffer.next")
    ("Left" . "buffer.previous")
    ("2" . "window.split-below")
    ("3" . "window.split-right")
    ("0" . "window.delete")
    ("1" . "window.delete-others")
    ("o" . "window.other")
    ("u" . "edit.undo")
    ("`" . "location.next-error")
    ("=" . "editor.position")))

(define editor-bindings
  '(("M-x" . "command.palette")
    ("M-:" . "scheme.eval-expression")
    ("C-g" . "keyboard.quit")
    ("C-s" . "search.prompt")
    ("C-r" . "search.backward-prompt")
    ("C-a" . "cursor.line-start")
    ("C-e" . "cursor.line-end")
    ("C-n" . "cursor.next-line")
    ("C-p" . "cursor.previous-line")
    ("C-f" . "cursor.forward-character")
    ("C-b" . "cursor.backward-character")
    ("C-v" . "cursor.page-down")
    ("M-v" . "cursor.page-up")
    ("C-/" . "edit.undo")
    ("C-_" . "edit.undo")
    ("C-M-/" . "edit.redo")
    ("C-SPC" . "selection.toggle-mark")
    ("C-w" . "edit.kill-region")
    ("C-k" . "edit.kill-line")
    ("M-w" . "edit.copy-region")
    ("C-y" . "edit.yank")
    ("C-d" . "edit.delete-forward")
    ("C-u" . "emacs.universal-argument")
    ("C-l" . "editor.redraw")
    ("C-M-f" . "cursor.forward-expression")
    ("C-M-b" . "cursor.backward-expression")
    ("C-M-u" . "cursor.up-list")
    ("C-c e" . "structural.enter")
    ("C-c s" . "selection.contract")
    ("C-c n" . "toy-modal.enter-normal")
    ("C-c h" . "helix.enter-normal")
    ("C-c m" . "meow.normal-mode")
    ("C-c 0" . "meow.prefix-digit-0")
    ("C-c 1" . "meow.prefix-digit-1")
    ("C-c 2" . "meow.prefix-digit-2")
    ("C-c 3" . "meow.prefix-digit-3")
    ("C-c 4" . "meow.prefix-digit-4")
    ("C-c 5" . "meow.prefix-digit-5")
    ("C-c 6" . "meow.prefix-digit-6")
    ("C-c 7" . "meow.prefix-digit-7")
    ("C-c 8" . "meow.prefix-digit-8")
    ("C-c 9" . "meow.prefix-digit-9")
    ("C-c v" . "vim.enter-normal")
    ("M-g g" . "cursor.goto-line")
    ("M-g n" . "location.next-error")
    ("M-g p" . "location.previous-error")
    ("M-%" . "search.replace")
    ("C-h b" . "help.describe-bindings")
    ("C-h k" . "help.describe-key")
    ("C-h x" . "help.describe-command")
    ("C-h f" . "help.describe-function")
    ("C-h v" . "help.describe-variable")
    ("C-h m" . "help.describe-mode")
    ("RET" . "edit.newline")
    ("TAB" . "edit.indent")
    ("Backspace" . "edit.delete-backward")
    ("Delete" . "edit.delete-forward")
    ("Left" . "cursor.backward-character")
    ("Right" . "cursor.forward-character")
    ("Up" . "cursor.previous-line")
    ("Down" . "cursor.next-line")
    ("PgUp" . "cursor.page-up")
    ("PgDn" . "cursor.page-down")
    ("Home" . "cursor.line-start")
    ("End" . "cursor.line-end")))

(define application-bindings
  '(("C-x C-c" . "application.quit")))

(define (bind-all! host keymap bindings)
  (let loop ((remaining bindings)
             (count 0))
    (if (null? remaining)
        count
        (let ((binding (car remaining)))
          (loop (cdr remaining)
                (if (bind-key-if-command! host
                                          keymap
                                          (car binding)
                                          (cdr binding))
                    (+ count 1)
                    count))))))

(define (install-default-keymaps! host)
  (define-keymap! host 'editor.control-x #f)
  (define-keymap! host 'editor.default #f)
  (define-keymap! host 'application.global #f)
  (bind-key! host 'editor.default "C-x" '(prefix editor.control-x "C-x"))
  (+ 1
     (bind-all! host 'editor.control-x control-x-bindings)
     (bind-all! host 'editor.default editor-bindings)
     (bind-all! host 'application.global application-bindings)
     (install-helix-keymaps! host)
     (install-meow-keymaps! host)
     (install-structural-keymap! host)
     (install-vim-keymaps! host)
     (install-toy-modal-keymap! host)))
