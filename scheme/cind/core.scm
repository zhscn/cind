(define-module (cind core)
  #:use-module (ice-9 format)
  #:use-module (ice-9 optargs)
  #:use-module (srfi srfi-1)
  #:use-module (srfi srfi-9)
  #:use-module (cind ares)
  #:use-module (cind async)
  #:use-module (cind command)
  #:use-module (cind development)
  #:use-module (cind emacs)
  #:use-module (cind helix)
  #:use-module (cind host)
  #:use-module (cind input)
  #:use-module (cind introspect)
  #:use-module (cind lifecycle)
  #:use-module (cind meow)
  #:use-module (cind minibuffer)
  #:use-module (cind pointer)
  #:use-module (cind structural)
  #:use-module (cind toy-modal)
  #:use-module (cind vim)
  #:export (install-core-commands!
            install-core-providers!
            install-input-states!
            install-core-modes!
            install-core-resource-policies!
            install-buffer-lifecycle-policies!
            install-pointer-policies!
            install-presentation-policies!
            install-display-policy!
            define-major-mode!
            define-minor-mode!
            install-default-keymaps!
            idle-echo-text
            modeline-content
            open-resource!
            project-search-running?
            project-index-updated!))

(define (startup-buffer name contents kind resource read-only? mode)
  (vector 'startup-buffer name contents kind resource read-only? mode))

(define (startup-plan buffer style style-origin resource placeholder?)
  (vector 'startup-plan buffer style style-origin resource placeholder?))

(define (default-startup-plan host facts)
  (let ((path (vector-ref facts 1))
        (has-initial-text? (vector-ref facts 2)))
    (cond
     ((zero? (string-length path))
      (startup-plan
       (startup-buffer "*scratch*"
                       (if has-initial-text? 'initial-text 'empty)
                       'scratch #f #f 'fundamental-mode)
       #f "plain text" #f #f))
     (has-initial-text?
      (let* ((resource (normalize-resource-path host path))
             (mode (or (resource-mode host resource) 'fundamental-mode)))
        (startup-plan
         (startup-buffer (path-filename host resource) 'initial-text 'file resource #f mode)
         #f (if (eq? mode 'cind.cpp) "llvm (fallback)" "plain text") #f #f)))
     (else
      (startup-plan
       (startup-buffer "*scratch*" 'empty 'scratch #f #f 'fundamental-mode)
       #f "plain text" (normalize-resource-path host path) #t)))))

(define (default-session-plan host facts)
  (vector 'session-plan
          (startup-buffer "*session*"
                          (if (vector-ref facts 1) 'initial-text 'empty)
                          'scratch #f #f 'cind.cpp)))

(define (default-fallback-buffer host)
  (create-buffer! host "*scratch*" "" 'scratch #f #f 'fundamental-mode #f
                  "plain text"))

(define (install-buffer-lifecycle-policies! host)
  (configure-startup-policy! host default-startup-plan)
  (configure-session-policy! host default-session-plan)
  (configure-fallback-buffer-policy! host default-fallback-buffer)
  (configure-close-policy!
   host
   (lambda (host context force?)
     (if force? 'application.force-quit 'application.quit)))
  4)

(define (default-pointer-policy host context event)
  (let ((kind (vector-ref event 1))
        (window (or (vector-ref event 2) (context-window context)))
        (line (vector-ref event 3))
        (column (vector-ref event 4))
        (pending-key? (vector-ref event 6)))
    (if (or pending-key?
            (interaction-active? host context)
            (not line)
            (not (memq kind '(document-text document-gutter))))
        #f
        (let ((focus-error (focus-window! host window)))
          (if focus-error
              #f
              (begin
                (move-caret-to-line! host (window-view-id host window) line
                                     (if (eq? kind 'document-gutter) 0 (or column 0)))
                #t))))))

(define (scroll-input-lines input)
  (let ((amount (vector-ref input 1)))
    (case (vector-ref input 2)
      ((lines) amount)
      ((steps) (* amount 3.0))
      (else (error "unknown scroll unit" (vector-ref input 2))))))

(define (install-pointer-policies! host)
  (configure-pointer-policy! host default-pointer-policy)
  (configure-scroll-policy!
   host
   (lambda (host context input)
     (let ((lines (scroll-input-lines input)))
       (if (zero? lines)
           #f
           (begin
             (scroll-view-lines! host (context-view context) lines)
             (set-caret-reveal! host #f)
             #t)))))
  2)

(define (display-fact-window facts window)
  (let ((windows (vector-ref facts 4)))
    (let loop ((index 0))
      (and (< index (vector-length windows))
           (let ((candidate (vector-ref windows index)))
             (if (equal? (vector-ref candidate 1) window)
                 candidate
                 (loop (+ index 1))))))))

(define (display-fact-slot facts role)
  (let ((slots (vector-ref facts 5)))
    (let loop ((index 0))
      (and (< index (vector-length slots))
           (let ((slot (vector-ref slots index)))
             (if (eq? (vector-ref slot 1) role)
                 (vector-ref slot 2)
                 (loop (+ index 1))))))))

(define (display-window-pinned? facts window)
  (let ((summary (display-fact-window facts window)))
    (and summary (vector-ref summary 3))))

(define (display-window-role facts window)
  (let ((summary (display-fact-window facts window)))
    (and summary (vector-ref summary 2))))

(define (display-adjacent-window facts window)
  (let* ((windows (vector-ref facts 4))
         (count (vector-length windows)))
    (let loop ((index 0))
      (cond ((= index count) #f)
            ((equal? (vector-ref (vector-ref windows index) 1) window)
             (and (> count 1)
                  (vector-ref (vector-ref windows (modulo (+ index 1) count)) 1)))
            (else (loop (+ index 1)))))))

(define (display-reuse window)
  (vector 'display-reuse window))

(define (display-split window axis ratio role)
  (vector 'display-split window axis ratio role))

(define (default-display-policy host facts)
  (let* ((intent (vector-ref facts 1))
         (origin (vector-ref facts 2))
         (active (vector-ref facts 3))
         (slot (display-fact-slot facts intent)))
    (cond ((eq? intent 'explicit)
           (display-reuse origin))
          ((and slot (not (display-window-pinned? facts slot)))
           (display-reuse slot))
          ((memq intent '(tools doc))
           (display-split active 'rows 0.72 intent))
          ((eq? intent 'pop)
           (display-split active 'columns 0.5 #f))
          ((and (or (eq? intent 'jump) (eq? intent 'list))
                (or (display-window-pinned? facts active)
                    (memq (display-window-role facts active) '(tools doc))))
           (let ((adjacent (display-adjacent-window facts active)))
             (if (and adjacent (not (display-window-pinned? facts adjacent)))
                 (display-reuse adjacent)
                 (display-split active 'columns 0.5 'jump))))
          ((not (display-window-pinned? facts active))
           (display-reuse active))
          (else
           (display-split origin 'columns 0.5 #f)))))

(define (install-display-policy! host)
  (configure-display-policy! host default-display-policy)
  1)

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

(define idle-echo-commands
  '(("file.save" . "save")
    ("file.open" . "open")
    ("buffer.switch" . "buffer")
    ("application.quit" . "quit")
    ("search.prompt" . "search")
    ("command.palette" . "commands")
    ("help.describe-bindings" . "help")))

(define (vector-contains-string? values target)
  (let loop ((index 0))
    (and (< index (vector-length values))
         (or (string=? (vector-ref values index) target)
             (loop (+ index 1))))))

(define (command-echo-hint enabled bindings entry)
  (and (vector-contains-string? enabled (car entry))
       (let loop ((index 0))
         (and (< index (vector-length bindings))
              (let ((binding (vector-ref bindings index)))
                (if (string=? (vector-ref binding 1) (car entry))
                    (string-append (vector-ref binding 0) " " (cdr entry))
                    (loop (+ index 1))))))))

(define (idle-echo-text host context)
  (let ((enabled (enabled-command-names host context))
        (bindings (active-key-bindings host)))
    (let loop ((commands idle-echo-commands)
               (result ""))
      (if (null? commands)
          result
          (let ((hint (command-echo-hint enabled bindings (car commands))))
            (loop (cdr commands)
                  (if hint
                      (string-append result (if (zero? (string-length result)) "" "  ") hint)
                      result)))))))

(define (modeline-segment group tone weight debug? text)
  (vector 'modeline-segment group tone weight debug? text))

(define (active-workbench-name host)
  (let ((workbenches (workbench-list host)))
    (and (> (vector-length workbenches) 1)
         (let ((active (current-workbench host)))
           (let loop ((index 0))
             (and (< index (vector-length workbenches))
                  (let ((summary (vector-ref workbenches index)))
                    (if (equal? (vector-ref summary 0) active)
                        (let ((name (vector-ref summary 1)))
                          (if (zero? (string-length name)) "default" name))
                        (loop (+ index 1))))))))))

(define (modeline-content host context facts)
  (let* ((buffer-name (vector-ref facts 1))
         (resource (vector-ref facts 2))
         (dirty? (vector-ref facts 3))
         (line (vector-ref facts 4))
         (column (vector-ref facts 5))
         (line-count (vector-ref facts 6))
         (revision (vector-ref facts 7))
         (style-origin (vector-ref facts 8))
         (last-key (vector-ref facts 9))
         (input-state (vector-ref facts 10))
         (workbench-name (active-workbench-name host))
         (name (if resource (path-filename host resource) buffer-name))
         (directory (if resource (path-parent host resource) ""))
         (percent (if (zero? line-count)
                      ""
                      (format #f "~a%" (min 100 (quotient (* line 100) line-count)))))
         (segments
          (append
           (list (modeline-segment 'chip
                                   (if dirty? 'critical 'faded)
                                   'strong #f
                                   (if dirty? "**" "RW"))
                 (modeline-segment 'left 'strong 'strong #f name))
           (if workbench-name
               (list (modeline-segment 'left 'salient 'strong #f workbench-name))
               '())
           (if (zero? (string-length directory))
               '()
               (list (modeline-segment 'left 'faded 'regular #f directory)))
           (if (zero? (string-length style-origin))
               '()
               (list (modeline-segment 'right 'faint 'regular #f style-origin)))
           (list (modeline-segment 'right 'faded 'regular #f
                                   (format #f "~a:~a" line column)))
           (if (zero? (string-length percent))
               '()
               (list (modeline-segment 'right 'faint 'regular #f percent)))
           (if (zero? (string-length input-state))
               '()
               (list (modeline-segment 'right 'salient 'strong #f input-state)))
           (if (zero? (string-length last-key))
               '()
               (list (modeline-segment 'right 'salient 'regular #f last-key)))
           (list (modeline-segment 'right 'faint 'regular #t
                                   (format #f "r~a" revision))))))
    (vector 'modeline (list->vector segments))))

(define (chrome-content pending-key echo echo-caret popup-title popup-items popup-capacity
                        popup-selection popup-input popup-input-caret)
  (vector 'chrome pending-key echo echo-caret popup-title popup-items popup-capacity
          popup-selection popup-input popup-input-caret))

(define (default-presentation-theme host)
  (vector 'presentation-theme
          #xff1e1e2e #xff2a2b3c #xff313244 #xff45475a
          #xff11111b #xffcdd6f4 #xffdee4f7 #xff7f849c
          #xff6c7086 #xff89b4fa #xfffab387 #xfff9e2af
          #xfff5e0dc #xffa6e3a1 #xfff9e2af #xfff38ba8))

(define (presentation-style role foreground background weight)
  (vector 'presentation-style role foreground background weight))

(define (default-presentation-styles host theme)
  (define (color index)
    (vector-ref theme index))
  (vector
   'presentation-styles
   #xb0
   #xc8
   (vector
    (presentation-style 'text (color 6) #f 'regular)
    (presentation-style 'keyword (color 10) #f 'regular)
    (presentation-style 'string (color 11) #f 'regular)
    (presentation-style 'number (color 11) #f 'regular)
    (presentation-style 'comment (color 8) #f 'regular)
    (presentation-style 'preprocessor (color 12) #f 'regular)
    (presentation-style 'gutter (color 9) #f 'regular)
    (presentation-style 'sign-added (color 14) #f 'regular)
    (presentation-style 'sign-modified (color 15) #f 'regular)
    (presentation-style 'sign-deleted (color 16) #f 'regular)
    (presentation-style 'status-bar (color 6) (color 3) 'regular)
    (presentation-style 'status-key (color 7) (color 3) 'strong)
    (presentation-style 'message (color 6) #f 'regular)
    (presentation-style 'popup (color 6) (color 3) 'regular)
    (presentation-style 'position-hint (color 1) (color 10) 'strong)
    (presentation-style 'popup-prompt (color 10) (color 3) 'regular)
    (presentation-style 'popup-count (color 9) (color 3) 'regular)
    (presentation-style 'popup-label (color 8) (color 3) 'strong)
    (presentation-style 'popup-input (color 7) (color 3) 'regular)
    (presentation-style 'popup-item (color 6) (color 3) 'regular)
    (presentation-style 'popup-detail (color 8) (color 3) 'regular)
    (presentation-style 'popup-selected (color 1) (color 8) 'regular)
    (presentation-style 'echo-key (color 6) #f 'regular)
    (presentation-style 'modeline-chip (color 1) #f 'regular)
    (presentation-style 'modeline-inactive (color 9) (color 2) 'regular)
    (presentation-style 'modeline-inactive-chip (color 8) (color 4) 'regular))
   (vector (color 7) (color 6) (color 8) (color 9) (color 10) (color 12))))

(define (default-presentation-motion host)
  (vector 'presentation-motion 70 32.0 0.001 0.01))

(define (default-presentation-metrics host)
  (vector 'presentation-metrics
          12.0 8.0 12.0 8.0 10.0 16.0 14.0 2.0 40 6))

(define (default-presentation-typography host)
  (vector 'presentation-typography "monospace" 16.0))

(define (default-chrome-content host context facts)
  (let* ((kind (vector-ref facts 1))
         (prompt (vector-ref facts 2))
         (input (vector-ref facts 3))
         (input-caret (vector-ref facts 4))
         (candidates (vector-ref facts 5))
         (selection (vector-ref facts 6))
         (message (vector-ref facts 7))
         (preedit (vector-ref facts 8))
         (pending-sequence (vector-ref facts 9))
         (pending-prefix (vector-ref facts 10))
         (hints (vector-ref facts 11))
         (prompt-bytes (vector-ref facts 12))
         (pending-key (cond ((zero? (string-length pending-prefix)) pending-sequence)
                            ((zero? (string-length pending-sequence)) pending-prefix)
                            (else (string-append pending-prefix " " pending-sequence)))))
    (cond
     ((eq? kind 'picker)
      (chrome-content pending-key "" #f prompt candidates 12 selection input input-caret))
     ((eq? kind 'text)
      (chrome-content pending-key (string-append prompt input)
                      (+ prompt-bytes input-caret) "" #() 0 #f #f #f))
     ((> (string-length preedit) 0)
      (chrome-content pending-key preedit #f "" #() 0 #f #f #f))
     ((> (vector-length hints) 0)
      (let ((items (make-vector (vector-length hints))))
        (let loop ((index 0))
          (when (< index (vector-length hints))
            (let* ((hint (vector-ref hints index))
                   (detail (vector-ref hint 2)))
              (vector-set! items index
                           (vector 'chrome-item
                                   (vector-ref hint 1)
                                   (if (and (zero? (string-length detail))
                                            (vector-ref hint 3))
                                       "prefix"
                                       detail))))
            (loop (+ index 1))))
        (chrome-content pending-key
                        (if (> (string-length message) 0)
                            message
                            (idle-echo-text host context))
                        #f (string-append pending-sequence " …") items
                        (vector-length items) #f #f #f)))
     (else
      (chrome-content pending-key
                      (if (> (string-length message) 0)
                          message
                          (idle-echo-text host context))
                      #f "" #() 0 #f #f #f)))))

(define (install-presentation-policies! host)
  (configure-modeline-policy! host modeline-content)
  (configure-chrome-policy! host default-chrome-content)
  (configure-theme-policy! host default-presentation-theme)
  (configure-style-policy! host default-presentation-styles)
  (configure-motion-policy! host default-presentation-motion)
  (configure-metrics-policy! host default-presentation-metrics)
  (configure-typography-policy! host default-presentation-typography)
  7)

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

(define (buffers-provider host context query widen?)
  (let ((summaries (workbench-buffer-summaries
                    host (current-workbench host) widen?)))
    (let loop ((index 0)
               (candidates '()))
      (if (= index (vector-length summaries))
          (list->vector (reverse candidates))
          (let* ((summary (vector-ref summaries index))
                 (name (vector-ref summary 0))
                 (resource (vector-ref summary 1))
                 (modified? (vector-ref summary 2))
                 (visitor? (vector-ref summary 3))
                 (base-detail (cond ((and resource modified?)
                                     (string-append resource " · modified"))
                                    (resource resource)
                                    (modified? "modified")
                                    (else "")))
                 (detail (if visitor?
                             (string-append base-detail
                                            (if (zero? (string-length base-detail)) "" " · ")
                                            "visitor")
                             base-detail))
                 (filter-text (if resource
                                  (string-append name " " resource)
                                  name)))
            (loop (+ index 1)
                  (cons (interaction-candidate name name detail filter-text)
                        candidates)))))))

(define (files-provider-directory host context query)
  (cond ((= (string-length query) 0)
         (let ((resource (buffer-resource host (context-buffer context))))
           (if resource
               (let ((parent (path-parent host resource)))
                 (if (= (string-length parent) 0) "." parent))
               ".")))
        ((directory-path? host query) query)
        (else
         (let ((parent (path-parent host query)))
           (if (= (string-length parent) 0) "." parent)))))

(define (directory-list-candidates host result)
  (let* ((directory (vector-ref result 1))
         (entries (vector-ref result 2))
         (parent (path-parent host directory))
         (parent-candidate
          (and (> (string-length parent) 0)
               (not (string=? parent directory))
               (let ((value (path-as-directory host parent)))
                 (interaction-candidate value "../" parent value)))))
    (let loop ((index 0)
               (candidates (if parent-candidate (list parent-candidate) '())))
      (if (= index (vector-length entries))
          (list->vector (reverse candidates))
          (let* ((entry (vector-ref entries index))
                 (path (vector-ref entry 0))
                 (name (vector-ref entry 1))
                 (directory? (vector-ref entry 2))
                 (value (if directory? (path-as-directory host path) path))
                 (label (if directory? (path-as-directory host name) name)))
            (loop (+ index 1)
                  (cons (interaction-candidate value label directory value)
                        candidates)))))))

(define (files-provider host context query)
  (interaction-provider-task
   (async-directory-list (files-provider-directory host context query) 1000)
   (lambda (result) (directory-list-candidates host result))))

(define (effective-projects host context)
  (let ((scope (workbench-scope host (current-workbench host))))
    (if (> (vector-length scope) 0)
        scope
        (let ((project (context-project context)))
          (if project (vector project) (vector))))))

(define (project-files-provider host context query)
  (let* ((projects (effective-projects host context))
         (multiple-projects? (> (vector-length projects) 1)))
    (let project-loop ((project-index 0)
                       (candidates '()))
      (if (= project-index (vector-length projects))
          (list->vector (reverse candidates))
          (let* ((project (vector-ref projects project-index))
                 (root (project-root host project))
                 (files (project-files host project)))
            (if (not root)
                (project-loop (+ project-index 1) candidates)
                (let file-loop ((file-index 0)
                                (result candidates))
                  (if (= file-index (vector-length files))
                      (project-loop (+ project-index 1) result)
                      (let* ((file (vector-ref files file-index))
                             (relative-value (path-relative host file root))
                             (relative (if (= (string-length relative-value) 0)
                                           (path-filename host file)
                                           relative-value)))
                        (file-loop
                         (+ file-index 1)
                         (cons (interaction-candidate
                                file relative
                                (if multiple-projects?
                                    root
                                    (path-parent host relative))
                                (string-append relative " " root " " file))
                               result)))))))))))

(define (workbenches-provider host context query)
  (let ((workbenches (workbench-list host)))
    (let loop ((index 0)
               (candidates '()))
      (if (= index (vector-length workbenches))
          (list->vector (reverse candidates))
          (let* ((summary (vector-ref workbenches index))
                 (name (vector-ref summary 1))
                 (label (if (zero? (string-length name)) "default" name))
                 (active? (vector-ref summary 2)))
            (loop (+ index 1)
                  (cons (interaction-candidate name label
                                               (if active? "active" "workbench")
                                               label)
                        candidates)))))))

(define (projects-provider host context query)
  (let ((projects (project-list host)))
    (let loop ((index 0)
               (candidates '()))
      (if (= index (vector-length projects))
          (list->vector (reverse candidates))
          (let* ((summary (vector-ref projects index))
                 (name (vector-ref summary 1))
                 (root (vector-ref summary 2)))
            (loop (+ index 1)
                  (if root
                      (cons (interaction-candidate root name root
                                                   (string-append name " " root))
                            candidates)
                      candidates)))))))

(define (window-roles-provider host context query)
  (list->vector
   (map (lambda (role)
          (interaction-candidate role role "display slot" role))
        '("jump" "tools" "doc"))))

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

(define-record-type <pending-open>
  (make-pending-open resource window intent line column mode contents style style-origin
                     discovery file-ready? style-ready? project-ready? tasks)
  pending-open?
  (resource pending-open-resource)
  (window pending-open-window set-pending-open-window!)
  (intent pending-open-intent set-pending-open-intent!)
  (line pending-open-line set-pending-open-line!)
  (column pending-open-column set-pending-open-column!)
  (mode pending-open-mode)
  (contents pending-open-contents set-pending-open-contents!)
  (style pending-open-style set-pending-open-style!)
  (style-origin pending-open-style-origin set-pending-open-style-origin!)
  (discovery pending-open-discovery set-pending-open-discovery!)
  (file-ready? pending-open-file-ready? set-pending-open-file-ready!)
  (style-ready? pending-open-style-ready? set-pending-open-style-ready!)
  (project-ready? pending-open-project-ready? set-pending-open-project-ready!)
  (tasks pending-open-tasks set-pending-open-tasks!))

;; Values do not retain the host. Outstanding callback closures keep their host
;; alive only until the native task reaches a terminal state.
(define pending-opens (make-weak-key-hash-table))

(define (host-pending-opens host)
  (or (hashq-ref pending-opens host) '()))

(define (set-host-pending-opens! host opens)
  (if (null? opens)
      (hashq-remove! pending-opens host)
      (hashq-set! pending-opens host opens)))

(define (pending-open-live? host open)
  (memq open (host-pending-opens host)))

(define (remove-pending-open! host open)
  (set-host-pending-opens! host (delq open (host-pending-opens host))))

(define (find-pending-open host resource)
  (find (lambda (open) (string=? resource (pending-open-resource open)))
        (host-pending-opens host)))

(define (remove-open-task! open task)
  (set-pending-open-tasks! open (delv task (pending-open-tasks open))))

(define (cancel-open-tasks! host open)
  (for-each (lambda (task) (cancel-async-task! host task))
            (pending-open-tasks open))
  (set-pending-open-tasks! open '()))

(define (fail-open! host open message)
  (when (pending-open-live? host open)
    (remove-pending-open! host open)
    (cancel-open-tasks! host open)
    (set-message! host (string-append "open failed: " message))))

(define (cancel-open! host open)
  (when (pending-open-live? host open)
    (remove-pending-open! host open)
    (cancel-open-tasks! host open)
    (set-message! host
                  (string-append "open cancelled: "
                                 (pending-open-resource open)))))

(define (display-open-buffer! host open buffer)
  (if (pending-open-line open)
      (display-buffer-at! host (pending-open-window open) buffer
                          (pending-open-intent open)
                          (pending-open-line open)
                          (or (pending-open-column open) 0))
      (display-buffer! host (pending-open-window open) buffer
                       (pending-open-intent open))))

(define (project-from-discovery! host discovery)
  (and discovery
       (let* ((root (vector-ref discovery 0))
              (provider (vector-ref discovery 1))
              (marker (vector-ref discovery 2)))
         (or (project-id-by-root host root)
             (create-project! host
                              (let ((name (path-filename host root)))
                                (if (= (string-length name) 0) root name))
                              (vector root) provider marker)))))

(define (ensure-project-index! host project)
  (let ((state (project-index-state host project)))
    (when (and (= (vector-ref state 0) 0)
               (not (vector-ref state 1)))
      (request-project-index! host project))))

(define (project-index-updated! host project)
  (when (and (equal? (interaction-provider host) "project-files")
             (equal? (interaction-origin-project host) project))
    (refresh-interaction! host)))

(define (release-startup-placeholder! host buffer)
  (let ((placeholder (startup-placeholder host)))
    (when (and placeholder
               (not (equal? placeholder buffer))
               (not (buffer-modified? host placeholder)))
      (set-startup-placeholder! host #f)
      (catch #t
        (lambda () (release-buffer! host placeholder buffer))
        (lambda (key . arguments)
          (set-startup-placeholder! host placeholder)
          (apply throw key arguments))))))

(define (finish-open! host open)
  (when (and (pending-open-live? host open)
             (pending-open-file-ready? open)
             (pending-open-style-ready? open)
             (pending-open-project-ready? open))
    (catch #t
      (lambda ()
        (let* ((resource (pending-open-resource open))
               (buffer
                (or (buffer-id-by-resource host resource)
                    (create-buffer! host (path-filename host resource)
                                    (pending-open-contents open) 'file resource #f
                                    (pending-open-mode open) (pending-open-style open)
                                    (pending-open-style-origin open))))
               (project
                (or (project-from-discovery! host (pending-open-discovery open))
                    (project-for-resource host resource))))
          (set-buffer-project! host buffer project)
          (when project (ensure-project-index! host project))
          (display-open-buffer! host open buffer)
          (release-startup-placeholder! host buffer)
          (remove-pending-open! host open)
          (set-message! host (string-append "opened " resource))))
      (lambda (key . arguments)
        (fail-open! host open (format #f "~S: ~S" key arguments))))))

(define (track-open-task! open task)
  (set-pending-open-tasks! open (cons task (pending-open-tasks open))))

(define (start-open-task! host open request completed)
  (let ((task
         (start-async-task!
          host request
          (lambda (task result)
            (remove-open-task! open task)
            (when (pending-open-live? host open)
              (completed result)
              (finish-open! host open)))
          #:failed
          (lambda (task message)
            (remove-open-task! open task)
            (fail-open! host open message))
          #:cancelled
          (lambda (task)
            (remove-open-task! open task)
            (cancel-open! host open)))))
    (track-open-task! open task)))

(define (cpp-style-mode? host mode)
  (eq? (vector-ref (mode-properties host mode) 7) 'cind.cpp))

(define (start-open! host open providers)
  (start-open-task!
   host open (async-file-read (pending-open-resource open))
   (lambda (result)
     (set-pending-open-contents! open (vector-ref result 3))
     (set-pending-open-file-ready! open #t)))
  (unless (pending-open-style-ready? open)
    (start-open-task!
     host open (async-clang-format-style (pending-open-resource open)
                                         "LLVM" "llvm (fallback)")
     (lambda (result)
       (set-pending-open-style! open (vector-ref result 3))
       (set-pending-open-style-origin! open (vector-ref result 4))
       (set-pending-open-style-ready! open #t))))
  (unless (pending-open-project-ready? open)
    (start-open-task!
     host open (async-project-discovery (pending-open-resource open) providers)
     (lambda (result)
       (set-pending-open-discovery!
        open (and (vector-ref result 2)
                  (vector (vector-ref result 2)
                          (vector-ref result 3)
                          (vector-ref result 4))))
       (set-pending-open-project-ready! open #t)))))

(define (open-resource-with-intent! host window path line column intent)
  (unless (string? path)
    (error "resource path must be a string" path))
  (unless (or (not line) (and (integer? line) (>= line 0)))
    (error "resource line must be a non-negative integer or #f" line))
  (unless (or (not column) (and (integer? column) (>= column 0)))
    (error "resource column must be a non-negative integer or #f" column))
  (let* ((resource (normalize-resource-path host path))
         (buffer (buffer-id-by-resource host resource)))
    (cond (buffer
           (let ((open (make-pending-open resource window intent line column #f "" #f ""
                                          #f #t #t #t '())))
             (display-open-buffer! host open buffer)))
          ((find-pending-open host resource)
           => (lambda (open)
                (set-pending-open-window! open window)
                (set-pending-open-intent! open intent)
                (set-pending-open-line! open line)
                (set-pending-open-column! open column)
                (set-message! host (string-append "opening " resource "…"))))
          (else
           (let* ((mode (or (resource-mode host resource) 'fundamental-mode))
                  (providers (project-provider-definitions host))
                  (style-ready? (not (cpp-style-mode? host mode)))
                  (open (make-pending-open
                         resource window intent line column mode "" #f
                         (if style-ready? "plain text" "llvm (fallback)")
                         #f #f style-ready? (= (vector-length providers) 0) '())))
             (set-host-pending-opens! host (cons open (host-pending-opens host)))
             (catch #t
               (lambda () (start-open! host open providers))
               (lambda (key . arguments)
                 (fail-open! host open (format #f "~S: ~S" key arguments))
                 (apply throw key arguments)))
             (set-message! host (string-append "opening " resource "…")))))))

(define (open-resource! host window path line column)
  (open-resource-with-intent! host window path line column 'edit))

(define (file-open-accept host context invocation)
  (let ((path (last-string-argument invocation)))
    (cond ((not path)
           (command-error "open file requires a path"))
          ((= (string-length path) 0)
           (command-error "file path is empty"))
          ((directory-path? host path)
           (file-open-interaction (path-as-directory host path)))
          (else
           (open-resource! host (context-window context) path #f #f)
           (command-completed)))))

(define (file-save-completed host buffer resource task result)
  (let ((newer? (complete-buffer-save! host buffer)))
    (set-message! host
                  (string-append "saved " resource
                                 (if newer? " · newer edits remain" "")))))

(define (file-save-failed host buffer message)
  (abort-buffer-save! host buffer)
  (set-message! host (string-append "save failed: " message)))

(define (file-save-cancelled host buffer)
  (abort-buffer-save! host buffer)
  (set-message! host "save cancelled"))

(define (file-save host context invocation)
  (let* ((buffer (context-buffer context))
         (resource (buffer-resource host buffer)))
    (cond ((not resource)
           (command-dispatch "file.save-as"))
          ((buffer-saving? host buffer)
           (command-error "save already in progress"))
          (else
           (let ((contents (begin-buffer-save! host buffer)))
             (catch #t
               (lambda ()
                 (start-async-task!
                  host (async-file-write resource contents)
                  (lambda (task result)
                    (file-save-completed host buffer resource task result))
                  #:failed
                  (lambda (task message)
                    (file-save-failed host buffer message))
                  #:cancelled
                  (lambda (task)
                    (file-save-cancelled host buffer)))
                 (set-message! host (string-append "saving " resource "…"))
                 (command-completed))
               (lambda (key . arguments)
                 (abort-buffer-save! host buffer)
                 (apply throw key arguments))))))))

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
           (let* ((buffer (context-buffer context))
                  (resource (normalize-resource-path host path))
                  (mode (or (resource-mode host resource) 'fundamental-mode))
                  (project (project-for-resource host resource)))
             (set-buffer-resource! host buffer resource)
             (rename-buffer! host buffer (path-filename host resource))
             (set-buffer-major-mode! host buffer mode)
             (set-buffer-project! host buffer project)
             (command-dispatch "file.save"))))))

(define (buffer-switch context invocation)
  (completing-read "Switch buffer: " "buffers" "buffer.switch.accept"
                   #:history "buffers"
                   #:keymap 'workbench.buffer-picker))

(define (buffer-switch-accept host context invocation)
  (let ((name (last-string-argument invocation)))
    (if (not name)
        (command-error "switch buffer requires a buffer name")
        (let ((buffer (buffer-id-by-name host name)))
          (if buffer
              (begin
                (display-buffer! host (context-window context) buffer 'explicit)
                (command-completed))
              (command-error (string-append "unknown buffer '" name "'")))))))

(define (vector-index-of values target)
  (let loop ((index 0))
    (cond ((= index (vector-length values)) #f)
          ((equal? (vector-ref values index) target) index)
          (else (loop (+ index 1))))))

(define buffer-cycle-states (make-weak-key-hash-table))

(define (same-buffer-set? left right)
  (and (= (vector-length left) (vector-length right))
       (let loop ((index 0))
         (or (= index (vector-length left))
             (and (vector-index-of right (vector-ref left index))
                  (loop (+ index 1)))))))

(define (buffer-switch-relative host context delta)
  (let* ((workbench (current-workbench host))
         (buffers (workbench-buffer-ids host workbench #f))
         (count (vector-length buffers)))
    (if (< count 2)
        (begin
          (hashq-remove! buffer-cycle-states host)
          (command-completed))
        (let* ((current-buffer (context-buffer context))
               (saved (hashq-ref buffer-cycle-states host))
               (saved-order (and saved (vector-ref saved 1)))
               (saved-index (and saved (vector-ref saved 2)))
               (continuing?
                (and saved
                     (equal? workbench (vector-ref saved 0))
                     (same-buffer-set? saved-order buffers)
                     (< saved-index (vector-length saved-order))
                     (equal? current-buffer (vector-ref saved-order saved-index))))
               (order (if continuing? saved-order buffers))
               (current (if continuing?
                            saved-index
                            (vector-index-of order current-buffer))))
          (if (not current)
              (command-error "current buffer is not open")
              (let* ((target-index (modulo (+ current delta) count))
                     (target (vector-ref order target-index)))
                (display-buffer! host (context-window context)
                                 target
                                 'explicit)
                (hashq-set! buffer-cycle-states host
                            (vector workbench order target-index))
                (command-completed)))))))

(define (buffer-switch-widen host context invocation)
  (let ((provider (interaction-provider host)))
    (cond ((equal? provider "buffers")
           (set-interaction-provider! host "buffers-global")
           (set-message! host "showing all buffers")
           (command-completed/preserve))
          ((equal? provider "buffers-global")
           (set-interaction-provider! host "buffers")
           (set-message! host "showing workbench buffers")
           (command-completed/preserve))
          (else
           (command-error "buffer picker is not active")))))

(define (buffer-picker-active? host context)
  (let ((provider (interaction-provider host)))
    (or (equal? provider "buffers")
        (equal? provider "buffers-global"))))

(define (workbench-summary-by-name host name)
  (let ((workbenches (workbench-list host)))
    (let loop ((index 0))
      (and (< index (vector-length workbenches))
           (let ((summary (vector-ref workbenches index)))
             (if (string=? (vector-ref summary 1) name)
                 summary
                 (loop (+ index 1))))))))

(define (project-summary-by-root host root)
  (let ((projects (project-list host)))
    (let loop ((index 0))
      (and (< index (vector-length projects))
           (let ((summary (vector-ref projects index)))
             (if (and (vector-ref summary 2)
                      (string=? (vector-ref summary 2) root))
                 summary
                 (loop (+ index 1))))))))

(define (workbench-new context invocation)
  (read-from-minibuffer "New workbench: " "workbench.new.accept"
                        #:history "workbenches"))

(define (workbench-new-accept host context invocation)
  (let ((name (last-string-argument invocation)))
    (cond ((not name)
           (command-error "new workbench requires a name"))
          ((zero? (string-length name))
           (command-error "workbench name is empty"))
          (else
           (new-workbench! host name (context-project context))
           (set-message! host (string-append "workbench " name))
           (command-completed)))))

(define (workbench-switch context invocation)
  (completing-read "Switch workbench: " "workbenches"
                   "workbench.switch.accept"
                   #:history "workbenches"))

(define (workbench-switch-accept host context invocation)
  (let* ((name (last-string-argument invocation))
         (summary (and name (workbench-summary-by-name host name))))
    (if (not summary)
        (command-error "unknown workbench")
        (begin
          (switch-workbench! host (vector-ref summary 0))
          (set-message! host
                        (string-append "workbench "
                                       (if (zero? (string-length name)) "default" name)))
          (command-completed)))))

(define (workbench-close host context invocation)
  (close-workbench! host (current-workbench host))
  (set-message! host "workbench closed")
  (command-completed))

(define (workbench-adopt-project context invocation)
  (completing-read "Adopt project: " "projects"
                   "workbench.adopt-project.accept"
                   #:history "projects"))

(define (workbench-adopt-project-accept host context invocation)
  (let* ((root (last-string-argument invocation))
         (summary (and root (project-summary-by-root host root))))
    (if (not summary)
        (command-error "unknown project")
        (begin
          (adopt-project! host (current-workbench host) (vector-ref summary 0))
          (set-message! host (string-append "adopted " (vector-ref summary 1)))
          (command-completed)))))

(define (workbench-expel-buffer host context invocation)
  (expel-buffer! host (current-workbench host) (context-buffer context))
  (set-message! host "buffer expelled from workbench")
  (command-completed))

(define pending-workbench-session-restores (make-weak-key-hash-table))

(define (workbench-save-session context invocation)
  (read-from-minibuffer "Save workbench session: "
                        "workbench.save-session.accept"
                        #:history "workbench-sessions"))

(define (workbench-save-session-accept host context invocation)
  (let ((path (last-string-argument invocation)))
    (if (or (not path) (zero? (string-length path)))
        (command-error "workbench session path is empty")
        (let ((resource (normalize-resource-path host path))
              (state (workbench-session-state host)))
          (start-async-task!
           host (async-file-write resource state)
           (lambda (task result)
             (set-message! host (string-append "saved workbench session " resource)))
           #:failed
           (lambda (task message)
             (set-message! host
                           (string-append "workbench session save failed: " message)))
           #:cancelled
           (lambda (task)
             (set-message! host "workbench session save cancelled")))
          (set-message! host "saving workbench session…")
          (command-completed)))))

(define (workbench-restore-session context invocation)
  (read-from-minibuffer "Restore workbench session: "
                        "workbench.restore-session.accept"
                        #:history "workbench-sessions"))

(define (workbench-session-restore-live? host task)
  (eqv? (hashq-ref pending-workbench-session-restores host) task))

(define (finish-workbench-session-restore! host task)
  (when (workbench-session-restore-live? host task)
    (hashq-remove! pending-workbench-session-restores host)))

(define (workbench-restore-session-accept host context invocation)
  (let ((path (last-string-argument invocation)))
    (if (or (not path) (zero? (string-length path)))
        (command-error "workbench session path is empty")
        (let* ((resource (normalize-resource-path host path))
               (previous (hashq-ref pending-workbench-session-restores host))
               (task #f))
          (when previous
            (hashq-remove! pending-workbench-session-restores host)
            (cancel-async-task! host previous))
          (set! task
                (start-async-task!
                 host (async-file-read resource)
                 (lambda (completed result)
                   (when (workbench-session-restore-live? host completed)
                     (finish-workbench-session-restore! host completed)
                     (if (vector-ref result 2)
                         (catch #t
                           (lambda ()
                             (set-message! host "restoring workbench session…")
                             (restore-workbench-session! host (vector-ref result 3)))
                           (lambda (key . arguments)
                             (set-message!
                              host
                              (string-append
                               "workbench session restore failed: "
                               (format #f "~S: ~S" key arguments)))))
                         (set-message! host "workbench session file does not exist"))))
                 #:failed
                 (lambda (failed message)
                   (when (workbench-session-restore-live? host failed)
                     (finish-workbench-session-restore! host failed)
                     (set-message! host
                                   (string-append
                                    "workbench session restore failed: " message))))
                 #:cancelled
                 (lambda (cancelled)
                   (when (workbench-session-restore-live? host cancelled)
                     (finish-workbench-session-restore! host cancelled)
                     (set-message! host "workbench session restore cancelled")))))
          (hashq-set! pending-workbench-session-restores host task)
          (set-message! host "reading workbench session…")
          (command-completed)))))

(define (first-other-buffer buffers target)
  (let loop ((index 0))
    (cond ((= index (vector-length buffers)) #f)
          ((equal? (vector-ref buffers index) target) (loop (+ index 1)))
          (else (vector-ref buffers index)))))

(define (buffer-kill host context force?)
  (let ((target (context-buffer context)))
    (cond ((buffer-saving? host target)
           (command-error "buffer has a save in progress"))
          ((and (buffer-modified? host target) (not force?))
           (command-error "buffer has unsaved changes"))
          (else
           (let* ((buffers (open-buffer-ids host))
                  (existing (first-other-buffer buffers target))
                  (replacement (if existing existing (create-fallback-buffer! host)))
                  (error (release-buffer! host target replacement)))
             (if error (command-error error) (command-completed)))))))

(define (completed-or-error error)
  (if error (command-error error) (command-completed)))

(define (completed-with-message host message)
  (set-message! host message)
  (command-completed))

(define (modified-buffer-count host)
  (let ((buffers (open-buffer-ids host)))
    (let loop ((index 0)
               (count 0))
      (if (= index (vector-length buffers))
          count
          (loop (+ index 1)
                (if (buffer-modified? host (vector-ref buffers index))
                    (+ count 1)
                    count))))))

(define (application-quit host)
  (let ((modified (modified-buffer-count host)))
    (if (= modified 0)
        (begin
          (exit-editor! host)
          (command-completed))
        (read-from-minibuffer
         (string-append (number->string modified)
                        " modified buffer"
                        (if (= modified 1) "" "s")
                        "; exit anyway? (yes or no) ")
         "application.quit.accept"))))

(define (application-quit-accept host invocation)
  (let ((answer (last-string-argument invocation)))
    (cond ((not answer)
           (command-error "quit confirmation requires yes or no"))
          ((string-ci=? answer "yes")
           (exit-editor! host)
           (command-completed))
          ((string-ci=? answer "no")
           (set-message! host "quit cancelled")
           (command-completed))
          (else
           (command-error "please answer yes or no")))))

(define (application-force-quit host)
  (exit-editor! host)
  (command-completed))

(define (window-split host context axis)
  (let ((error (split-window! host (context-window context) axis)))
    (if error
        (command-error error)
        (completed-with-message
         host (if (eq? axis 'rows) "window split below" "window split right")))))

(define (window-delete host context)
  (let ((error (delete-window! host (context-window context))))
    (if error
        (command-error error)
        (completed-with-message host "window deleted"))))

(define (window-delete-others host context)
  (if (<= (vector-length (open-window-ids host)) 1)
      (completed-with-message host "only window")
      (let ((error (delete-other-windows! host (context-window context))))
        (if error
            (command-error error)
            (completed-with-message host "other windows deleted")))))

(define (window-other host context)
  (let* ((windows (open-window-ids host))
         (count (vector-length windows))
         (current (vector-index-of windows (context-window context))))
    (cond ((< count 2)
           (command-error "only window"))
          ((not current)
           (command-error "current window is not open"))
          (else
           (completed-or-error
            (focus-window! host (vector-ref windows (modulo (+ current 1) count))))))))

(define (window-set-role host context invocation)
  (let ((role (window-role host (context-window context))))
    (completing-read "Window role: " "window-roles" "window.set-role.accept"
                     #:initial-input (if role (symbol->string role) "")
                     #:history "window-roles"
                     #:allow-custom-input? #t)))

(define (window-set-role-accept host context invocation)
  (let ((role (last-string-argument invocation)))
    (if (not role)
        (command-error "window role requires a value")
        (begin
          (set-window-role! host (context-window context)
                            (if (zero? (string-length role)) #f (string->symbol role)))
          (completed-with-message
           host (if (zero? (string-length role))
                    "window role cleared"
                    (string-append "window role " role)))))))

(define (window-toggle-pinned host context invocation)
  (let* ((window (context-window context))
         (pinned? (not (window-pinned? host window))))
    (set-window-pinned! host window pinned?)
    (completed-with-message host (if pinned? "window pinned" "window unpinned"))))

(define (window-dismiss host context invocation)
  (let ((window (context-window context)))
    (if (not (window-created-by-policy? host window))
        (command-error "window was not created by display policy")
        (window-delete host context))))

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

(define-record-type <pending-project-search>
  (make-pending-project-search projects window query root tasks)
  pending-project-search?
  (projects pending-project-search-projects)
  (window pending-project-search-window)
  (query pending-project-search-query)
  (root pending-project-search-root)
  (tasks pending-project-search-tasks set-pending-project-search-tasks!))

(define pending-project-searches (make-weak-key-hash-table))

(define (project-search-running? host)
  (and (hashq-ref pending-project-searches host) #t))

(define (project-search-live? host search)
  (eq? search (hashq-ref pending-project-searches host)))

(define (remove-project-search-task! search task)
  (set-pending-project-search-tasks!
   search (delv task (pending-project-search-tasks search))))

(define (cancel-project-search-tasks! host search)
  (for-each (lambda (task) (cancel-async-task! host task))
            (pending-project-search-tasks search))
  (set-pending-project-search-tasks! search '()))

(define (clear-project-search! host search)
  (when (project-search-live? host search)
    (hashq-remove! pending-project-searches host)))

(define (replace-project-search! host search)
  (let ((previous (hashq-ref pending-project-searches host)))
    (when previous
      (hashq-remove! pending-project-searches host)
      (cancel-project-search-tasks! host previous))
    (hashq-set! pending-project-searches host search)))

(define (trim-line-endings text)
  (let loop ((end (string-length text)))
    (if (and (> end 0)
             (let ((character (string-ref text (- end 1))))
               (or (char=? character #\newline)
                   (char=? character #\return))))
        (loop (- end 1))
        (substring text 0 end))))

(define (fail-project-search! host search message)
  (when (project-search-live? host search)
    (clear-project-search! host search)
    (cancel-project-search-tasks! host search)
    (set-message! host (string-append "project search failed: " message))))

(define (cancel-project-search! host search)
  (when (project-search-live? host search)
    (clear-project-search! host search)
    (cancel-project-search-tasks! host search)
    (set-message! host "project search cancelled")))

(define (finish-project-search! host search result)
  (when (project-search-live? host search)
    (catch #t
      (lambda ()
        (let* ((query (pending-project-search-query search))
               (text (vector-ref result 1))
               (contents (if (= (string-length text) 0)
                             (string-append "No matches for: " query "\n")
                             text))
               (buffer
                (create-buffer! host (string-append "*project grep: " query "*")
                                contents 'process #f #t 'cind.location-list #f
                                "location-list")))
          (set-buffer-locations! host buffer (vector-ref result 2))
          (set-location-navigation! host buffer #f)
          (let ((projects (pending-project-search-projects search)))
            (set-buffer-project! host buffer
                                 (and (= (vector-length projects) 1)
                                      (vector-ref projects 0))))
          (display-buffer! host (pending-project-search-window search) buffer 'tools)
          (clear-project-search! host search)
          (set-message! host (string-append "project search finished: " query))))
      (lambda (key . arguments)
        (fail-project-search! host search (format #f "~S: ~S" key arguments))))))

(define (start-project-search-parser! host search output)
  (catch #t
    (lambda ()
      (let ((task #f))
        (set! task
              (start-async-task!
               host
               (async-rg-result-parse (pending-project-search-root search) output)
               (lambda (completed-task result)
                 (remove-project-search-task! search completed-task)
                 (finish-project-search! host search result))
               #:failed
               (lambda (failed-task message)
                 (remove-project-search-task! search failed-task)
                 (fail-project-search! host search message))
               #:cancelled
               (lambda (cancelled-task)
                 (remove-project-search-task! search cancelled-task)
                 (cancel-project-search! host search))))
        (set-pending-project-search-tasks!
         search (cons task (pending-project-search-tasks search)))
        (set-message! host "preparing project search results…")))
    (lambda (key . arguments)
      (fail-project-search! host search (format #f "~S: ~S" key arguments)))))

(define (project-search-process-completed! host search task result)
  (remove-project-search-task! search task)
  (when (project-search-live? host search)
    (let ((status (vector-ref result 1))
          (signal (vector-ref result 2))
          (output (vector-ref result 3))
          (error-output (trim-line-endings (vector-ref result 4))))
      (if (or (not (= signal 0)) (> status 1))
          (fail-project-search!
           host search
           (if (= (string-length error-output) 0)
               (if (= signal 0)
                   (string-append "process exited with status " (number->string status))
                   (string-append "process terminated by signal "
                                  (number->string signal)))
               error-output))
          (start-project-search-parser! host search output)))))

(define (start-project-search! host projects window query)
  (when (= (vector-length projects) 0)
    (error "project search requires at least one project"))
  (let ((roots
         (let loop ((index 0)
                    (result '()))
           (if (= index (vector-length projects))
               (reverse result)
               (let ((root (project-root host (vector-ref projects index))))
                 (unless root
                   (error "project has no root" (vector-ref projects index)))
                 (loop (+ index 1) (cons root result)))))))
    (let* ((root (car roots))
           (search (make-pending-project-search projects window query root '()))
           (targets (if (= (length roots) 1) (list ".") roots)))
      (replace-project-search! host search)
      (catch #t
        (lambda ()
          (let ((task #f))
            (set! task
                  (start-async-task!
                   host
                   (async-process
                    "rg"
                    (append
                     (list "--line-number" "--column" "--no-heading" "--color" "never"
                           "--smart-case" "--null" "--" query)
                     targets)
                    root)
                   (lambda (completed-task result)
                     (project-search-process-completed! host search completed-task result))
                   #:failed
                   (lambda (failed-task message)
                     (remove-project-search-task! search failed-task)
                     (fail-project-search! host search message))
                   #:cancelled
                   (lambda (cancelled-task)
                     (remove-project-search-task! search cancelled-task)
                     (cancel-project-search! host search))))
            (set-pending-project-search-tasks! search (list task))
            (set-message! host (if (= (length roots) 1)
                                   "searching project…"
                                   "searching workbench projects…"))))
        (lambda (key . arguments)
          (fail-project-search! host search (format #f "~S: ~S" key arguments))
          (apply throw key arguments))))))

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
        (projects (effective-projects host context)))
    (cond ((or (not query) (= (string-length query) 0))
           (command-error "project search query is empty"))
          ((= (vector-length projects) 0)
           (command-error "workbench has no project"))
          (else
           (start-project-search! host projects (context-window context) query)
           (command-completed)))))

(define (project-find-file host context invocation)
  (let ((projects (effective-projects host context)))
    (if (= (vector-length projects) 0)
        (command-error "workbench has no project")
        (begin
          (let loop ((index 0))
            (when (< index (vector-length projects))
              (ensure-project-index! host (vector-ref projects index))
              (loop (+ index 1))))
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
           (open-resource! host (context-window context) path #f #f)
           (command-completed)))))

(define (help-keys context invocation)
  (completing-read "Key bindings: " "key-bindings" "help.keys.accept"
                   #:history "key-bindings"))

(define (help-keys-accept host context invocation)
  (let ((binding (last-string-argument invocation)))
    (if binding
        (set-message! host binding))
    (command-completed)))

(define (context-has-project? host context)
  (> (vector-length (effective-projects host context)) 0))

(define (interaction-active? host context)
  (vector-ref (interaction-status host) 0))

(define (interaction-picker-active? host context)
  (vector-ref (interaction-status host) 1))

(define (interaction-history-active? host context)
  (vector-ref (interaction-status host) 2))

(define (keyboard-quit host context invocation)
  (reset-input-states! host (context-view context))
  (cancel-pending-input! host)
  (unless (or (cancel-completion! host)
              (cancel-interaction! host))
    (clear-selection! host (context-view context)))
  (set-message! host "cancelled")
  (command-completed/preserve))

(define (completion-start host context invocation)
  (let ((major (vector-ref (buffer-mode-summary host (context-buffer context)) 0)))
    (start-completion! host context
                       (if (eq? major 'cind.cpp)
                           '("lsp:cpp:clangd" "word" "path")
                           '("word" "path"))))
  (request-redraw! host)
  (command-completed/preserve))

(define (completion-move host delta)
  (lambda (context invocation)
    (move-completion! host delta)
    (request-redraw! host)
    (command-completed/preserve)))

(define (completion-accept host context invocation)
  (apply-completion! host #f)
  (request-redraw! host)
  (command-completed/preserve))

(define (completion-cancel host context invocation)
  (cancel-completion! host)
  (request-redraw! host)
  (command-completed/preserve))

(define (interaction-move-candidate host delta)
  (lambda (context invocation)
    (move-minibuffer-candidate! host delta)
    (command-completed/preserve)))

(define (interaction-move-history host delta)
  (lambda (context invocation)
    (move-minibuffer-history! host context delta)
    (command-completed/preserve)))

(define (interaction-submit host context invocation)
  (let* ((submission (submit-interaction! host))
         (arguments (vector-ref submission 1))
         (target (vector-ref submission 2))
         (history (vector-ref submission 3))
         (value (and (> (vector-length arguments) 0)
                     (vector-ref arguments (- (vector-length arguments) 1)))))
    (when (and history (string? value))
      (record-minibuffer-history! host history value))
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

(define (location-list-context? host context)
  (eq? (vector-ref (buffer-mode-summary host (context-buffer context)) 0)
       'cind.location-list))

(define (location-navigation-available? host context)
  (or (> (vector-length (buffer-locations host (context-buffer context))) 0)
      (> (vector-ref (location-navigation host) 2) 0)))

(define (location-message host index count)
  (set-message! host
                (string-append "location " (number->string (+ index 1))
                               "/" (number->string count))))

(define (location-visit-index host context list index)
  (let ((locations (buffer-locations host list)))
    (if (or (< index 0) (>= index (vector-length locations)))
        (command-error "location list is no longer available")
        (let ((location (vector-ref locations index)))
          (set-location-navigation! host list index)
          (position-buffer-view! host (context-window context) list
                                 (vector-ref location 0))
          (location-message host index (vector-length locations))
          (open-resource-with-intent! host (context-window context)
                                      (vector-ref location 2)
                                      (vector-ref location 3)
                                      (vector-ref location 4)
                                      'list)
          (command-completed/preserve)))))

(define (location-at-point locations caret)
  (let loop ((index 0))
    (if (= index (vector-length locations))
        #f
        (let ((location (vector-ref locations index)))
          (if (or (and (<= (vector-ref location 0) caret)
                       (< caret (vector-ref location 1)))
                  (>= (vector-ref location 0) caret))
              index
              (loop (+ index 1)))))))

(define (location-visit host context invocation)
  (let* ((locations (buffer-locations host (context-buffer context)))
         (index (location-at-point locations
                                   (view-caret host (context-view context)))))
    (if index
        (location-visit-index host context (context-buffer context) index)
        (command-error "no location at point"))))

(define (location-index-after locations caret)
  (let loop ((index 0))
    (cond ((= index (vector-length locations)) #f)
          ((> (vector-ref (vector-ref locations index) 0) caret) index)
          (else (loop (+ index 1))))))

(define (location-index-before locations caret)
  (let loop ((index (- (vector-length locations) 1)))
    (cond ((< index 0) #f)
          ((< (vector-ref (vector-ref locations index) 0) caret) index)
          (else (loop (- index 1))))))

(define (location-move host context direction)
  (let* ((locations (buffer-locations host (context-buffer context)))
         (count (vector-length locations)))
    (if (= count 0)
        (command-error "location list is empty")
        (let* ((view (context-view context))
               (caret (view-caret host view))
               (index (if (> direction 0)
                          (location-index-after locations caret)
                          (location-index-before locations caret))))
          (if (not index)
              (command-error (if (> direction 0)
                                 "end of location list"
                                 "beginning of location list"))
              (begin
                (set-view-caret! host view
                                 (vector-ref (vector-ref locations index) 0))
                (reset-preferred-column! host view)
                (request-redraw! host)
                (location-message host index count)
                (command-completed/preserve)))))))

(define (location-index-at-or-after locations caret)
  (let loop ((index 0))
    (cond ((= index (vector-length locations)) #f)
          ((> (vector-ref (vector-ref locations index) 1) caret) index)
          (else (loop (+ index 1))))))

(define (location-index-at-or-before locations caret)
  (let loop ((index (- (vector-length locations) 1)))
    (cond ((< index 0) #f)
          ((<= (vector-ref (vector-ref locations index) 0) caret) index)
          (else (loop (- index 1))))))

(define (location-navigate host context direction)
  (let* ((context-locations (buffer-locations host (context-buffer context)))
         (at-list? (> (vector-length context-locations) 0))
         (navigation (location-navigation host))
         (navigation-buffer (vector-ref navigation 0))
         (list (if at-list? (context-buffer context) navigation-buffer)))
    (if (not list)
        (command-error "no current location list")
        (let* ((locations (if at-list?
                              context-locations
                              (buffer-locations host list)))
               (count (vector-length locations)))
          (if (= count 0)
              (begin
                (set-location-navigation! host #f #f)
                (command-error "no current location list"))
              (let* ((same-list? (and navigation-buffer
                                      (equal? navigation-buffer list)))
                     (current (and same-list? (vector-ref navigation 1)))
                     (valid? (and current (< current count)))
                     (caret (view-caret host (context-view context)))
                     (continuing?
                      (and valid?
                           (or (not at-list?)
                               (= caret
                                  (vector-ref (vector-ref locations current) 0)))))
                     (selected
                      (cond
                       (continuing?
                        (cond ((and (> direction 0) (< (+ current 1) count))
                               (+ current 1))
                              ((and (< direction 0) (> current 0))
                               (- current 1))
                              (else #f)))
                       (at-list?
                        (if (> direction 0)
                            (location-index-at-or-after locations caret)
                            (location-index-at-or-before locations caret)))
                       ((> direction 0) 0)
                       (else (- count 1)))))
                (if selected
                    (location-visit-index host context list selected)
                    (command-error (if (> direction 0)
                                       "end of location list"
                                       "beginning of location list")))))))))

(define (jump-move host context invocation direction)
  (let* ((count (or (invocation-repeat-count invocation) 1))
         (delta (* direction count)))
    (if (navigate-jump! host (context-window context) delta)
        (begin
          (request-redraw! host)
          (command-completed/preserve))
        (command-error (if (< direction 0)
                           "beginning of jump history"
                           "end of jump history")))))

(define (jump-mark host context)
  (let ((node (mark-jump! host (context-window context))))
    (if node
        (begin
          (set-message! host (format #f "jump node ~a" node))
          (command-completed/preserve))
        (command-error "current position cannot be marked"))))

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
               (if (and (structural-typed-character? text)
                        (buffer-language-facet? host (context-buffer context)
                                                'structural-editing))
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
          (let* ((effective-mode
                  (if (and (eq? mode 'structural)
                           (not (buffer-language-facet? host (context-buffer context)
                                                        'structural-editing)))
                      'raw
                      mode))
                 (outcome (delete-grapheme! host view direction effective-mode)))
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
      (let ((range (soft-kill-range
                    host (context-view context)
                    (if (buffer-language-facet? host (context-buffer context)
                                                'structural-editing)
                        'structural
                        'plain))))
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
         (list "completion.start"
               (lambda (context invocation)
                 (completion-start host context invocation))
               (lambda (context) (not (completion-active? host))))
         (list "completion.next"
               (completion-move host 1)
               (lambda (context) (completion-active? host)))
         (list "completion.previous"
               (completion-move host -1)
               (lambda (context) (completion-active? host)))
         (list "completion.accept"
               (lambda (context invocation)
                 (completion-accept host context invocation))
               (lambda (context) (completion-active? host)))
         (list "completion.cancel"
               (lambda (context invocation)
                 (completion-cancel host context invocation))
               (lambda (context) (completion-active? host)))
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
         (list "location.visit"
               (lambda (context invocation)
                 (location-visit host context invocation))
               (lambda (context) (location-list-context? host context)))
         (list "location.next"
               (lambda (context invocation)
                 (location-move host context 1))
               (lambda (context) (location-list-context? host context)))
         (list "location.previous"
               (lambda (context invocation)
                 (location-move host context -1))
               (lambda (context) (location-list-context? host context)))
         (list "location.next-error"
               (lambda (context invocation)
                 (location-navigate host context 1))
               (lambda (context)
                 (location-navigation-available? host context)))
         (list "location.previous-error"
               (lambda (context invocation)
                 (location-navigate host context -1))
               (lambda (context)
                 (location-navigation-available? host context)))
         (list "jump.back"
               (lambda (context invocation)
                 (jump-move host context invocation -1))
               #f)
         (list "jump.forward"
               (lambda (context invocation)
                 (jump-move host context invocation 1))
               #f)
         (list "jump.mark"
               (lambda (context invocation)
                 (jump-mark host context))
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
        (list "buffer.switch-widen"
              (lambda (context invocation)
                (buffer-switch-widen host context invocation))
              (lambda (context) (buffer-picker-active? host context)))
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
        (list "workbench.new.accept"
              (lambda (context invocation)
                (workbench-new-accept host context invocation))
              #f)
        (list "workbench.new" workbench-new #f)
        (list "workbench.switch.accept"
              (lambda (context invocation)
                (workbench-switch-accept host context invocation))
              #f)
        (list "workbench.switch" workbench-switch #f)
        (list "workbench.close"
              (lambda (context invocation)
                (workbench-close host context invocation))
              #f)
        (list "workbench.adopt-project.accept"
              (lambda (context invocation)
                (workbench-adopt-project-accept host context invocation))
              #f)
        (list "workbench.adopt-project" workbench-adopt-project #f)
        (list "workbench.expel"
              (lambda (context invocation)
                (workbench-expel-buffer host context invocation))
              #f)
        (list "workbench.save-session.accept"
              (lambda (context invocation)
                (workbench-save-session-accept host context invocation))
              #f)
        (list "workbench.save-session" workbench-save-session #f)
        (list "workbench.restore-session.accept"
              (lambda (context invocation)
                (workbench-restore-session-accept host context invocation))
              #f)
        (list "workbench.restore-session" workbench-restore-session #f)
        (list "application.quit"
              (lambda (context invocation)
                (application-quit host))
              #f)
        (list "application.quit.accept"
              (lambda (context invocation)
                (application-quit-accept host invocation))
              #f)
        (list "application.force-quit"
              (lambda (context invocation)
                (application-force-quit host))
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
        (list "window.set-role.accept"
              (lambda (context invocation)
                (window-set-role-accept host context invocation))
              #f)
        (list "window.set-role"
              (lambda (context invocation)
                (window-set-role host context invocation))
              #f)
        (list "window.toggle-pinned"
              (lambda (context invocation)
                (window-toggle-pinned host context invocation))
              #f)
        (list "window.dismiss"
              (lambda (context invocation)
                (window-dismiss host context invocation))
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
              (lambda (context) (context-has-project? host context)))
        (list "project.search.accept"
              (lambda (context invocation)
                (project-search-accept host context invocation))
              #f)
        (list "project.search" project-search
              (lambda (context) (context-has-project? host context)))
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
  (configure-minibuffer-history-policy! host (make-bounded-history-policy 100))
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
                 (buffers-provider host context query #f)))
         (cons "buffers-global"
               (lambda (context query)
                 (buffers-provider host context query #t)))
         (cons "files"
               (lambda (context query)
                 (files-provider host context query)))
         (cons "project-files"
               (lambda (context query)
                 (project-files-provider host context query)))
         (cons "workbenches"
               (lambda (context query)
                 (workbenches-provider host context query)))
         (cons "projects"
               (lambda (context query)
                 (projects-provider host context query)))
         (cons "window-roles"
               (lambda (context query)
                 (window-roles-provider host context query)))
         (cons "key-bindings"
               (lambda (context query)
                 (key-bindings-provider host context query))))
   (introspection-providers host)))

(define (install-core-providers! host)
  (let ((providers (core-providers host)))
    (for-each (lambda (provider)
                (define-interaction-provider!
                 host (car provider)
                 (lambda (context query)
                   (rank-provider-result ((cdr provider) context query) query))))
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
                            (language #f)
                            (keymap #f)
                            (interaction-class #f)
                            (initial-state #f)
                            (things '()))
  (%define-mode! host name 'major parent language keymap interaction-class initial-state things))

(define* (define-minor-mode! host name
                            #:key
                            (parent #f)
                            (keymap #f)
                            (interaction-class #f)
                            (initial-state #f)
                            (things '()))
  (%define-mode! host name 'minor parent #f keymap interaction-class initial-state things))

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
  (define-language-profile!
    host 'cind.cpp
    '((lexing . cind.c-family.lexer)
      (syntax . cind.c-family.syntax)
      (indentation . cind.c-family.indentation)
      (structural-motion . cind.c-family.structural-motion)
      (structural-editing . cind.c-family.structural-editing)
      (highlighting . cind.c-family.highlighting))
    '((language.c-family.dialect . "c++")))
  (define-language-profile!
    host 'cind.scheme
    '((structural-motion . cind.scheme.structural-motion))
    '())
  (define-keymap! host 'scheme-mode-map #f)
  (define-keymap! host 'cind.location-list.map #f)
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
    #:language 'cind.scheme
    #:keymap 'scheme-mode-map
    #:interaction-class 'editing)
  (define-major-mode! host 'cind.cpp
    #:parent 'prog-mode
    #:language 'cind.cpp
    #:interaction-class 'editing
    #:things '((angle . cind.angle)
               (defun . cind.defun)
               (string . cind.string)))
  (define-major-mode! host 'cind.location-list
    #:parent 'special-mode
    #:keymap 'cind.location-list.map
    #:interaction-class 'interface)
  6)

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
    ("C-M-i" . "completion.start")
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
    ("M-," . "jump.back")
    ("C-M-," . "jump.forward")
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

(define completion-bindings
  '(("C-n" . "completion.next")
    ("Down" . "completion.next")
    ("C-p" . "completion.previous")
    ("Up" . "completion.previous")
    ("TAB" . "completion.accept")
    ("RET" . "completion.accept")
    ("C-e" . "completion.cancel")
    ("ESC" . "completion.cancel")))

(define workbench-bindings
  '(("n" . "workbench.new")
    ("s" . "workbench.switch")
    ("k" . "workbench.close")
    ("a" . "workbench.adopt-project")
    ("e" . "workbench.expel")
    ("S" . "workbench.save-session")
    ("R" . "workbench.restore-session")
    ("r" . "window.set-role")
    ("p" . "window.toggle-pinned")
    ("d" . "window.dismiss")))

(define policy-created-window-bindings
  '(("q" . "window.dismiss")))

(define scheme-mode-bindings
  '(("C-c C-e" . "scheme.eval-expression")
    ("C-c C-r" . "scheme.eval-region")
    ("C-c C-b" . "scheme.eval-buffer")))

(define location-list-bindings
  '(("RET" . "location.visit")
    ("M-n" . "location.next")
    ("M-p" . "location.previous")))

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
  (define-keymap! host 'completion.active #f)
  (define-keymap! host 'workbench.buffer-picker 'interaction.picker)
  (define-keymap! host 'editor.workbench #f)
  (define-keymap! host 'window.policy-created #f)
  (define-keymap! host 'scheme-mode-map #f)
  (define-keymap! host 'cind.location-list.map #f)
  (bind-key! host 'editor.default "C-x" '(prefix editor.control-x "C-x"))
  (bind-key! host 'editor.control-x "w" '(prefix editor.workbench "C-x w"))
  (let ((picker-binding
         (if (bind-key-if-command! host 'workbench.buffer-picker
                                   "C-x b" "buffer.switch-widen")
             1 0)))
    (+ 2 picker-binding
       (bind-all! host 'editor.control-x control-x-bindings)
       (bind-all! host 'editor.text-input text-input-bindings)
       (bind-all! host 'editor.default editor-bindings)
       (bind-all! host 'application.global application-bindings)
       (bind-all! host 'editor.system system-bindings)
       (bind-all! host 'interaction.text interaction-text-bindings)
       (bind-all! host 'interaction.picker interaction-picker-bindings)
       (bind-all! host 'completion.active completion-bindings)
       (bind-all! host 'editor.workbench workbench-bindings)
       (bind-all! host 'window.policy-created policy-created-window-bindings)
       (bind-all! host 'scheme-mode-map scheme-mode-bindings)
       (bind-all! host 'cind.location-list.map location-list-bindings)
       (install-helix-keymaps! host)
       (install-meow-keymaps! host)
       (install-structural-keymap! host)
       (install-vim-keymaps! host)
       (install-toy-modal-keymap! host))))
