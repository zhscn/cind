(define-module (cind core)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (install-core-commands!
            install-default-keymaps!))

(define (last-string-argument invocation)
  (let ((arguments (invocation-arguments invocation)))
    (and (> (vector-length arguments) 0)
         (let ((argument (vector-ref arguments (- (vector-length arguments) 1))))
           (and (string? argument) argument)))))

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

(define (core-commands host)
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
        (list "help.keys" help-keys #f)))

(define (install-core-commands! host)
  (let ((commands (core-commands host)))
    (for-each (lambda (definition)
                (define-command! host
                                 (list-ref definition 0)
                                 (list-ref definition 1)
                                 (list-ref definition 2)))
              commands)
    (length commands)))

(define editor-bindings
  '(("C-x C-s" . "file.save")
    ("C-x C-w" . "file.save-as")
    ("C-x C-f" . "file.open")
    ("C-x p f" . "project.find-file")
    ("C-x p g" . "project.search")
    ("C-x b" . "buffer.switch")
    ("C-x k" . "buffer.kill")
    ("C-x Right" . "buffer.next")
    ("C-x Left" . "buffer.previous")
    ("C-x 2" . "window.split-below")
    ("C-x 3" . "window.split-right")
    ("C-x 0" . "window.delete")
    ("C-x 1" . "window.delete-others")
    ("C-x o" . "window.other")
    ("M-x" . "command.palette")
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
    ("C-x u" . "edit.undo")
    ("C-M-/" . "edit.redo")
    ("C-SPC" . "selection.toggle-mark")
    ("C-w" . "edit.kill-region")
    ("C-k" . "edit.kill-line")
    ("M-w" . "edit.copy-region")
    ("C-y" . "edit.yank")
    ("C-d" . "edit.delete-forward")
    ("C-l" . "editor.redraw")
    ("C-M-f" . "cursor.forward-expression")
    ("C-M-b" . "cursor.backward-expression")
    ("C-M-u" . "cursor.up-list")
    ("C-c e" . "selection.expand")
    ("C-c s" . "selection.contract")
    ("M-g g" . "cursor.goto-line")
    ("M-g n" . "location.next-error")
    ("M-g p" . "location.previous-error")
    ("C-x `" . "location.next-error")
    ("M-%" . "search.replace")
    ("C-h b" . "help.keys")
    ("C-x =" . "editor.position")
    ("RET" . "edit.newline")
    ("TAB" . "edit.indent")
    ("Backspace" . "edit.delete-backward")
    ("Delete" . "edit.delete-forward")
    ("C-u Backspace" . "edit.delete-backward-raw")
    ("C-u Delete" . "edit.delete-forward-raw")
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
  (+ (bind-all! host "editor.default" editor-bindings)
     (bind-all! host "application.global" application-bindings)))
