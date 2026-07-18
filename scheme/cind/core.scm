(define-module (cind core)
  #:use-module (ice-9 optargs)
  #:use-module (cind ares)
  #:use-module (cind command)
  #:use-module (cind development)
  #:use-module (cind emacs)
  #:use-module (cind helix)
  #:use-module (cind host)
  #:use-module (cind input)
  #:use-module (cind introspect)
  #:use-module (cind meow)
  #:use-module (cind minibuffer)
  #:use-module (cind structural)
  #:use-module (cind toy-modal)
  #:use-module (cind vim)
  #:export (install-core-commands!
            install-core-providers!
            install-input-states!
            install-core-modes!
            install-core-resource-policies!
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
  (completing-read "Command: " "commands" "command.palette.accept"
                   #:history "commands"))

(define (file-open-interaction initial-input)
  (completing-read "Open file: " "files" "file.open.accept"
                   #:initial-input initial-input
                   #:history "files"
                   #:allow-custom-input? #t))

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
    (read-from-minibuffer "Write file: " "file.save-as.accept"
                          #:initial-input (if resource resource "")
                          #:history "files")))

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
  (completing-read "Switch buffer: " "buffers" "buffer.switch.accept"
                   #:history "buffers"))

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
  (read-from-minibuffer "Go to line: " "cursor.goto-line.accept"
                        #:history "line-numbers"))

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
  (read-from-minibuffer "Project search: " "project.search.accept"
                        #:history "project-search"))

(define (search-prompt forward?)
  (read-from-minibuffer (if forward? "search: " "search backward: ")
                        "search.accept"
                        #:history "search"
                        #:arguments (list forward?)))

(define (search-match host buffer query caret forward?)
  (let* ((size (buffer-byte-size host buffer))
         (start (if forward?
                    (min (+ caret 1) size)
                    (and (> caret 0) (- caret 1))))
         (first (and start
                     (find-buffer-text host buffer query start
                                       (if forward? 'forward 'backward)))))
    (if first
        (vector first #f)
        (let ((wrapped (find-buffer-text host buffer query
                                         (if forward? 0 size)
                                         (if forward? 'forward 'backward))))
          (vector wrapped (and wrapped #t))))))

(define (make-search-commands host)
  (let ((query ""))
    (define (move context forward?)
      (if (= (string-length query) 0)
          (begin
            (set-message! host "search query is empty")
            (command-completed/preserve))
          (let* ((view (context-view context))
                 (result (search-match host (context-buffer context) query
                                       (view-caret host view) forward?))
                 (match (vector-ref result 0))
                 (wrapped? (vector-ref result 1)))
            (if (not match)
                (begin
                  (set-message! host (string-append "\"" query "\" not found"))
                  (command-completed/preserve))
                (begin
                  (reset-preferred-column! host view)
                  (set-view-caret! host view (vector-ref match 0))
                  (set-message! host (if wrapped? "search wrapped" ""))
                  (request-redraw! host)
                  (command-completed/preserve))))))

    (define (accept context invocation)
      (let ((arguments (invocation-arguments invocation)))
        (if (or (< (vector-length arguments) 2)
                (not (boolean? (vector-ref arguments 0)))
                (not (string? (vector-ref arguments
                                          (- (vector-length arguments) 1)))))
            (command-error "search accepts a direction and query")
            (let ((input (vector-ref arguments (- (vector-length arguments) 1))))
              (unless (= (string-length input) 0)
                (set! query input))
              (move context (vector-ref arguments 0))))))

    (list (list "search.prompt"
                (lambda (context invocation) (search-prompt #t))
                #f)
          (list "search.backward-prompt"
                (lambda (context invocation) (search-prompt #f))
                #f)
          (list "search.accept" accept #f)
          (list "search.next"
                (lambda (context invocation) (move context #t))
                #f)
          (list "search.previous"
                (lambda (context invocation) (move context #f))
                #f))))

(define (query-replace-count-message count)
  (string-append "replaced " (number->string count) " occurrence"
                 (if (= count 1) "" "s")))

(define (query-replace-finish! host view count)
  (clear-selection! host view)
  (request-redraw! host)
  (set-message! host (query-replace-count-message count)))

(define (query-replace-selection range)
  (selection
   (list (selection-range (vector-ref range 0) (vector-ref range 1) 'char))))

(define (query-replace-result-caret selected)
  (let* ((ranges (selection-ranges selected))
         (primary (selection-primary selected)))
    (selection-range-head (vector-ref ranges primary))))

(define (query-replace-one! host view match replacement)
  (query-replace-result-caret
   (replace-selection! host view (query-replace-selection match) replacement)))

(define (query-replace-matches host buffer query start)
  (let loop ((position start)
             (matches '()))
    (let ((match (find-buffer-text host buffer query position 'forward)))
      (if match
          (loop (vector-ref match 1) (cons match matches))
          (reverse matches)))))

(define (query-replace-all! host buffer view query replacement start count)
  (let ((matches (query-replace-matches host buffer query start)))
    (unless (null? matches)
      (replace-selection!
       host view
       (selection
        (map (lambda (match)
               (selection-range (vector-ref match 0) (vector-ref match 1) 'char))
             matches))
       replacement))
    (query-replace-finish! host view (+ count (length matches)))))

(define query-replace-hints
  (vector (vector "y" "replace" #f)
          (vector "n" "skip" #f)
          (vector "!" "replace remaining" #f)
          (vector "q" "quit" #f)))

(define (query-replace-read! host buffer view query replacement start count)
  (let ((match (find-buffer-text host buffer query start 'forward)))
    (if (not match)
        (query-replace-finish! host view count)
        (begin
          (set-selection! host view (query-replace-selection match))
          (request-redraw! host)
          (set-message! host
                        (string-append "Replace " query " with " replacement
                                       "? (y/n/!/q)"))
          (read-key-then!
           host view
           (lambda (key)
             (cond ((or (string=? key "y") (string=? key "SPC"))
                    (query-replace-read!
                     host buffer view query replacement
                     (query-replace-one! host view match replacement)
                     (+ count 1)))
                   ((or (string=? key "n") (string=? key "Delete"))
                    (clear-selection! host view)
                    (set-view-caret! host view (vector-ref match 1))
                    (query-replace-read! host buffer view query replacement
                                         (vector-ref match 1) count))
                   ((or (string=? key "!") (string=? key "a"))
                    (query-replace-all! host buffer view query replacement
                                        (vector-ref match 0) count))
                   ((or (string=? key "q") (string=? key "RET"))
                    (query-replace-finish! host view count))
                   (else
                    (query-replace-read! host buffer view query replacement
                                         (vector-ref match 0) count)))
             'consume)
           #:sequence "query-replace"
           #:hints query-replace-hints)))))

(define (query-replace context invocation)
  (read-from-minibuffer "Replace: " "search.replace.from.accept"
                        #:history "search"))

(define (query-replace-from-accept context invocation)
  (let ((query (last-string-argument invocation)))
    (if (or (not query) (= (string-length query) 0))
        (command-error "replacement query is empty")
        (read-from-minibuffer
         (string-append "Replace " query " with: ")
         "search.replace.to.accept"
         #:history "replace"
         #:arguments (list query)))))

(define (query-replace-to-accept host context invocation)
  (let* ((arguments (invocation-arguments invocation))
         (query (and (= (vector-length arguments) 2) (vector-ref arguments 0)))
         (replacement (last-string-argument invocation)))
    (if (not (and (string? query) (string? replacement)))
        (command-error "query replace arguments are malformed")
        (begin
          (query-replace-read! host (context-buffer context) (context-view context)
                               query replacement
                               (view-caret host (context-view context)) 0)
          (command-completed/preserve)))))

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
          (completing-read "Project file: " "project-files"
                           "project.find-file.accept"
                           #:history "project-files")))))

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
  (completing-read "Key bindings: " "key-bindings" "help.keys.accept"
                   #:history "key-bindings"))

(define (help-keys-accept host context invocation)
  (let ((binding (last-string-argument invocation)))
    (if binding
        (set-message! host binding))
    (command-completed)))

(define (context-has-project? context)
  (and (context-project context) #t))

(define (interaction-active? host context)
  (vector-ref (interaction-status host) 0))

(define (interaction-picker-active? host context)
  (vector-ref (interaction-status host) 1))

(define (interaction-history-active? host context)
  (vector-ref (interaction-status host) 2))

(define (keyboard-quit host context invocation)
  (reset-input-states! host (context-view context))
  (cancel-pending-input! host)
  (unless (cancel-interaction! host)
    (clear-selection! host (context-view context)))
  (set-message! host "cancelled")
  (command-completed/preserve))

(define (interaction-move-candidate host delta)
  (lambda (context invocation)
    (move-interaction-candidate! host delta)
    (command-completed/preserve)))

(define (interaction-move-history host delta)
  (lambda (context invocation)
    (move-interaction-history! host delta)
    (command-completed/preserve)))

(define (interaction-submit host context invocation)
  (let* ((submission (submit-interaction! host))
         (arguments (vector-ref submission 1))
         (target (vector-ref submission 2)))
    (apply command-dispatch-to
           (vector-ref submission 0)
           (vector-ref target 0)
           (vector-ref target 1)
           (vector-ref target 2)
           (vector->list arguments))))

(define (editor-position host context invocation)
  (let ((position (view-position host (context-view context))))
    (set-message!
     host
     (string-append
      "line " (number->string (+ (vector-ref position 0) 1))
      "/" (number->string (vector-ref position 1))
      ", column " (number->string (+ (vector-ref position 2) 1))
      ", byte " (number->string (vector-ref position 3))
      "/" (number->string (vector-ref position 4))))
    (command-completed/preserve)))

(define kill-ring-limit 60)

(define (take-at-most values count)
  (if (or (= count 0) (null? values))
      '()
      (cons (car values) (take-at-most (cdr values) (- count 1)))))

(define (range-start range)
  (min (vector-ref range 0) (vector-ref range 1)))

(define (range-end range)
  (max (vector-ref range 0) (vector-ref range 1)))

(define signed-64-min -9223372036854775808)
(define signed-64-max 9223372036854775807)

(define (bounded-signed-64 value)
  (max signed-64-min (min signed-64-max value)))

(define (repeat-text text count)
  (let loop ((remaining count)
             (chunk text)
             (result ""))
    (cond ((= remaining 0) result)
          ((= remaining 1) (string-append result chunk))
          ((odd? remaining)
           (loop (quotient remaining 2)
                 (string-append chunk chunk)
                 (string-append result chunk)))
          (else
           (loop (quotient remaining 2)
                 (string-append chunk chunk)
                 result)))))

(define (buffer-writable? host context)
  (not (buffer-read-only? host (context-buffer context))))

(define (structural-typed-character? text)
  (and (= (string-length text) 1)
       (let ((codepoint (char->integer (string-ref text 0))))
         (and (>= codepoint 32) (< codepoint 128)))))

(define (self-insert host context invocation)
  (let* ((text (last-string-argument invocation))
         (count (or (invocation-repeat-count invocation) 1)))
    (cond ((not text)
           (command-error "self insert requires text"))
          ((< count 0)
           (command-error "negative repeat count is invalid for text input"))
          ((= count 0)
           (set-message! host "")
           (command-completed/preserve))
          ((not (buffer-writable? host context))
           (command-error "buffer is read-only"))
          (else
           (let ((view (context-view context)))
             (reset-preferred-column! host view)
             (let ((committed (repeat-text text count)))
               (if (structural-typed-character? text)
                   (type-text! host view committed)
                   (insert-text! host view committed)))
             (request-redraw! host)
             (command-completed))))))

(define (history-edit host context redo?)
  (if (not (buffer-writable? host context))
      (command-error "buffer is read-only")
      (let ((view (context-view context)))
        (reset-preferred-column! host view)
        (let ((changed? (if redo? (redo! host view) (undo! host view))))
          (set-message! host
                        (cond (changed? (if redo? "redo" "undo"))
                              (redo? "nothing to redo")
                              (else "nothing to undo")))
          (command-completed)))))

(define (line-boundary-command host boundary)
  (lambda (context invocation)
    (let ((view (context-view context)))
      (reset-preferred-column! host view)
      (move-caret-line-boundary! host view boundary)
      (request-redraw! host)
      (command-completed/preserve))))

(define (vertical-command host direction page?)
  (lambda (context invocation)
    (let* ((count (or (invocation-repeat-count invocation) 1))
           (scale (if page? (page-rows host) 1))
           (delta (bounded-signed-64 (* direction scale count))))
      (move-caret-lines! host (context-view context) delta)
      (request-redraw! host)
      (command-completed/preserve))))

(define (delete-command host direction mode)
  (lambda (context invocation)
    (if (not (buffer-writable? host context))
        (command-error "buffer is read-only")
        (let ((view (context-view context)))
          (reset-preferred-column! host view)
          (let ((outcome (delete-grapheme! host view direction mode)))
            (cond ((eq? outcome 'moved-over-pair)
                   (set-message! host "soft delete: pair not empty (moved over)"))
                  ((eq? outcome 'moved-over-literal)
                   (set-message! host "soft delete: literal not empty (moved over)"))))
          (command-completed)))))

(define (newline-command host context invocation)
  (if (not (buffer-writable? host context))
      (command-error "buffer is read-only")
      (let ((view (context-view context)))
        (reset-preferred-column! host view)
        (newline! host view)
        (command-completed))))

(define (indent-command host context invocation)
  (if (not (buffer-writable? host context))
      (command-error "buffer is read-only")
      (let ((view (context-view context)))
        (reset-preferred-column! host view)
        (let ((role (indent! host view)))
          (set-message! host
                        (if role
                            (string-append "indent: " role)
                            "indentation unavailable for this mode")))
        (command-completed))))

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
           (selected (motion-selection host view (view-selection host view)
                                       motion count extend?)))
      (reset-preferred-column! host view)
      (request-redraw! host)
      (command-completed/selection selected))))

(define (make-caret-motion-command host motion)
  (lambda (context invocation)
    (let* ((view (context-view context))
           (count (or (invocation-repeat-count invocation) 1))
           (selected (motion-selection host view (view-selection host view)
                                       motion count #f))
           (ranges (selection-ranges selected))
           (primary (selection-primary selected)))
      (reset-preferred-column! host view)
      (set-view-caret! host view
                       (selection-range-head (vector-ref ranges primary)))
      (request-redraw! host)
      (command-completed/preserve))))

(define (core-commands host)
  (append
   (list (list "edit.self-insert"
               (lambda (context invocation)
                 (self-insert host context invocation))
               #f)
         (list "edit.undo"
               (lambda (context invocation)
                 (history-edit host context #f))
               #f)
         (list "edit.redo"
               (lambda (context invocation)
                 (history-edit host context #t))
               #f)
         (list "cursor.line-start" (line-boundary-command host 'start) #f)
         (list "cursor.line-end" (line-boundary-command host 'end) #f)
         (list "cursor.next-line" (vertical-command host 1 #f) #f)
         (list "cursor.previous-line" (vertical-command host -1 #f) #f)
         (list "cursor.page-down" (vertical-command host 1 #t) #f)
         (list "cursor.page-up" (vertical-command host -1 #t) #f)
         (list "cursor.forward-character"
               (make-caret-motion-command host 'cind.forward-character)
               #f)
         (list "cursor.backward-character"
               (make-caret-motion-command host 'cind.backward-character)
               #f)
         (list "edit.delete-backward"
               (delete-command host 'backward 'structural)
               #f)
         (list "edit.delete-forward"
               (delete-command host 'forward 'structural)
               #f)
         (list "edit.delete-backward-raw"
               (delete-command host 'backward 'raw)
               #f)
         (list "edit.delete-forward-raw"
               (delete-command host 'forward 'raw)
               #f)
         (list "edit.newline"
               (lambda (context invocation)
                 (newline-command host context invocation))
               #f)
         (list "edit.indent"
               (lambda (context invocation)
                 (indent-command host context invocation))
               #f)
         (list "keyboard.quit"
               (lambda (context invocation)
                 (keyboard-quit host context invocation))
               #f)
         (list "interaction.submit"
               (lambda (context invocation)
                 (interaction-submit host context invocation))
               (lambda (context) (interaction-active? host context)))
         (list "interaction.next-candidate"
               (interaction-move-candidate host 1)
               (lambda (context) (interaction-picker-active? host context)))
         (list "interaction.previous-candidate"
               (interaction-move-candidate host -1)
               (lambda (context) (interaction-picker-active? host context)))
         (list "interaction.previous-history"
               (interaction-move-history host -1)
               (lambda (context) (interaction-history-active? host context)))
         (list "interaction.next-history"
               (interaction-move-history host 1)
               (lambda (context) (interaction-history-active? host context)))
         (list "editor.position"
               (lambda (context invocation)
                 (editor-position host context invocation))
               #f)
         (list "command.palette.accept" command-palette-accept #f)
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
              (make-motion-command host 'cind.forward-word-end #f)
              #f)
        (list "cursor.backward-word"
              (make-motion-command host 'cind.backward-word #f)
              #f)
        (list "selection.extend-forward-word"
              (make-motion-command host 'cind.forward-word-end #t)
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
        (list "search.replace" query-replace #f)
        (list "search.replace.from.accept" query-replace-from-accept #f)
        (list "search.replace.to.accept"
              (lambda (context invocation)
                (query-replace-to-accept host context invocation))
              #f)
        (list "help.keys.accept"
              (lambda (context invocation)
                (help-keys-accept host context invocation))
              #f)
        (list "help.keys" help-keys #f))
   (make-search-commands host)
   (introspection-command-definitions host)
   (development-command-definitions host)
   (ares-command-definitions host)
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
    (install-ares-documentation! host)
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
  (define-motion! host 'cind.forward-word-end 'forward-word-end)
  (define-motion! host 'cind.backward-word 'backward-word)
  (define-motion! host 'cind.forward-symbol 'forward-symbol)
  (define-motion! host 'cind.backward-symbol 'backward-symbol)
  (define-motion! host 'cind.forward-expression 'forward-expression)
  (define-motion! host 'cind.backward-expression 'backward-expression)
  (define-motion! host 'cind.up-list 'up-list)
  (define-keymap! host 'scheme-mode-map #f)
  (define-major-mode! host 'fundamental-mode
    #:interaction-class 'editing)
  (define-major-mode! host 'prog-mode
    #:parent 'fundamental-mode
    #:interaction-class 'editing
    #:things '((word . cind.word)
               (symbol . cind.symbol)))
  (define-major-mode! host 'special-mode
    #:parent 'fundamental-mode
    #:interaction-class 'interface)
  (define-major-mode! host 'scheme-mode
    #:parent 'prog-mode
    #:keymap 'scheme-mode-map
    #:interaction-class 'editing)
  4)

(define (install-core-resource-policies! host)
  (define-file-mode-rule!
    host 'cind.c-family 'cind.cpp
    '(".c" ".h" ".cc" ".cpp" ".cxx" ".hh" ".hpp" ".hxx" ".inc" ".ipp" ".tpp")
    '())
  (define-file-mode-rule!
    host 'cind.scheme 'scheme-mode
    '(".scm" ".ss" ".sls" ".sld")
    '())
  (define-project-provider! host 'cind.vcs '(".git" ".hg" ".svn"))
  (define-project-provider! host 'cind.cmk '("cmk.yaml"))
  (define-project-provider! host 'cind.compilation-database '("compile_commands.json"))
  5)

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

(define text-input-bindings
  '(("C-a" . "cursor.line-start")
    ("C-e" . "cursor.line-end")
    ("C-f" . "cursor.forward-character")
    ("C-b" . "cursor.backward-character")
    ("M-f" . "cursor.forward-word")
    ("M-b" . "cursor.backward-word")
    ("C-k" . "edit.kill-line")
    ("C-y" . "edit.yank")
    ("C-/" . "edit.undo")
    ("C-_" . "edit.undo")
    ("C-M-/" . "edit.redo")
    ("C-SPC" . "selection.toggle-mark")
    ("C-w" . "edit.kill-region")
    ("M-w" . "edit.copy-region")
    ("C-u" . "emacs.universal-argument")
    ("C-d" . "edit.delete-forward")
    ("Backspace" . "edit.delete-backward")
    ("Delete" . "edit.delete-forward")
    ("Left" . "cursor.backward-character")
    ("Right" . "cursor.forward-character")
    ("Home" . "cursor.line-start")
    ("End" . "cursor.line-end")))

(define editor-bindings
  '(("M-x" . "command.palette")
    ("M-:" . "scheme.eval-expression")
    ("C-g" . "keyboard.quit")
    ("C-s" . "search.prompt")
    ("C-r" . "search.backward-prompt")
    ("C-n" . "cursor.next-line")
    ("C-p" . "cursor.previous-line")
    ("C-v" . "cursor.page-down")
    ("M-v" . "cursor.page-up")
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
    ("Up" . "cursor.previous-line")
    ("Down" . "cursor.next-line")
    ("PgUp" . "cursor.page-up")
    ("PgDn" . "cursor.page-down")))

(define application-bindings
  '(("C-x C-c" . "application.quit")))

(define system-bindings
  '(("C-g" . "keyboard.quit")))

(define interaction-text-bindings
  '(("RET" . "interaction.submit")
    ("ESC" . "keyboard.quit")
    ("M-p" . "interaction.previous-history")
    ("M-n" . "interaction.next-history")))

(define interaction-picker-bindings
  '(("C-n" . "interaction.next-candidate")
    ("Down" . "interaction.next-candidate")
    ("TAB" . "interaction.next-candidate")
    ("C-p" . "interaction.previous-candidate")
    ("Up" . "interaction.previous-candidate")))

(define scheme-mode-bindings
  '(("C-c C-e" . "scheme.eval-expression")
    ("C-c C-r" . "scheme.eval-region")
    ("C-c C-b" . "scheme.eval-buffer")))

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
  (define-keymap! host 'editor.text-input #f)
  (define-keymap! host 'editor.default 'editor.text-input)
  (define-keymap! host 'application.global #f)
  (define-keymap! host 'editor.system #f)
  (define-keymap! host 'interaction.text 'editor.text-input)
  (define-keymap! host 'interaction.picker 'interaction.text)
  (define-keymap! host 'scheme-mode-map #f)
  (bind-key! host 'editor.default "C-x" '(prefix editor.control-x "C-x"))
  (+ 1
     (bind-all! host 'editor.control-x control-x-bindings)
     (bind-all! host 'editor.text-input text-input-bindings)
     (bind-all! host 'editor.default editor-bindings)
     (bind-all! host 'application.global application-bindings)
     (bind-all! host 'editor.system system-bindings)
     (bind-all! host 'interaction.text interaction-text-bindings)
     (bind-all! host 'interaction.picker interaction-picker-bindings)
     (bind-all! host 'scheme-mode-map scheme-mode-bindings)
     (install-helix-keymaps! host)
     (install-meow-keymaps! host)
     (install-structural-keymap! host)
     (install-vim-keymaps! host)
     (install-toy-modal-keymap! host)))
