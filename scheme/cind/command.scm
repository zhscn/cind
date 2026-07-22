(define-module (cind command)
  #:use-module (ice-9 optargs)
  #:use-module (cind host)
  #:use-module (cind state)
  #:export (command-completed
            command-completed/preserve
            command-completed/collapse
            command-completed/selection
            command-prefix
            command-prefix-state
            set-command-prefix-state!
            clear-command-prefix-state!
            command-error
            command-feedback-state
            command-input!
            command-result!
            record-command!
            set-message!
            command-dispatch
            command-dispatch-to
            interaction
            interaction-candidate
            interaction-provider-task
            completion-provider-task
            completion-item
            completion-item-label
            completion-item-kind
            completion-item-detail
            completion-item-filter-text
            completion-item-sort-text
            completion-item-insert-text
            completion-item-documentation
            completion-item-start
            completion-item-end
            completion-result
            completion-request-query
            completion-request-anchor
            completion-request-caret
            completion-request-line
            completion-request-trigger
            completion-request-trigger-character
            context-window
            context-buffer
            context-view
            context-project
            configure-keymap-policy!
            resolve-keymap-policy
            resolve-base-keymap-policy
            base-keymap-layers
            active-keymap-layers
            configure-modeline-policy!
            resolve-modeline-content
            configure-display-policy!
            resolve-display-plan
            configure-chrome-policy!
            resolve-chrome-content
            configure-theme-policy!
            resolve-presentation-theme
            configure-style-policy!
            resolve-presentation-styles
            configure-motion-policy!
            resolve-presentation-motion
            configure-metrics-policy!
            resolve-presentation-metrics
            configure-typography-policy!
            resolve-presentation-typography
            resolve-presentation-profile
            invocation-arguments
            invocation-repeat-count
            invocation-register
            invocation-prefix-extra
            selection
            selection-range
            selection-primary
            selection-metadata
            selection-ranges
            selection-range-anchor
            selection-range-head
            selection-range-granularity
            selection-with-metadata
            selection-with-ranges))

;; Runtime state lives in the application state root rather than in tables
;; private to this module (design/09-guile-first.md §4).
(define-state-slot! 'command-feedback (lambda () (vector "" "" "")))
(define-state-slot! 'command-prefix (lambda () #(#f #f ())))

(define (valid-command-prefix-extra? extra)
  (and (list? extra)
       (let loop ((entries extra)
                  (names '()))
         (if (null? entries)
             #t
             (let ((entry (car entries)))
               (and (pair? entry)
                    (string? (car entry))
                    (> (string-length (car entry)) 0)
                    (not (member (car entry) names))
                    (let ((value (cdr entry)))
                      (or (boolean? value)
                          (real? value)
                          (string? value)))
                    (loop (cdr entries) (cons (car entry) names))))))))

(define (validate-command-prefix-state state)
  (unless (and (vector? state)
               (= (vector-length state) 3)
               (let ((count (vector-ref state 0)))
                 (or (not count) (integer? count)))
               (let ((register (vector-ref state 1)))
                 (or (not register)
                     (and (string? register) (> (string-length register) 0))))
               (valid-command-prefix-extra? (vector-ref state 2)))
    (error "invalid command prefix state" state))
  state)

(define (command-prefix-state host)
  (state-ref host 'command-prefix))

(define (set-command-prefix-state! host state)
  (validate-command-prefix-state state)
  (state-set! host 'command-prefix state))

(define (clear-command-prefix-state! host)
  (state-clear! host 'command-prefix))

(define (command-feedback-entry host)
  (state-ref host 'command-feedback))

(define (command-feedback-state host)
  (let ((state (command-feedback-entry host)))
    (vector (vector-ref state 0)
            (vector-ref state 1)
            (vector-ref state 2))))

(define (command-input! host key clear-message?)
  (unless (string? key)
    (error "command input key must be a string" key))
  (unless (boolean? clear-message?)
    (error "command input clear-message flag must be a boolean" clear-message?))
  (let ((state (command-feedback-entry host)))
    (when clear-message?
      (vector-set! state 0 ""))
    (vector-set! state 1 key)))

(define (record-command! host command)
  (unless (string? command)
    (error "command name must be a string" command))
  (vector-set! (command-feedback-entry host) 2 command))

(define command-result-statuses
  '(not-handled prefix prefix-argument executed awaiting-input disabled cancelled error))

(define (command-result! host status consumed? command interaction-started? message)
  (unless (memq status command-result-statuses)
    (error "unknown command result status" status))
  (unless (boolean? consumed?)
    (error "command result consumed flag must be a boolean" consumed?))
  (unless (or (not command) (string? command))
    (error "command result command must be a string or #f" command))
  (unless (boolean? interaction-started?)
    (error "command result interaction flag must be a boolean" interaction-started?))
  (unless (string? message)
    (error "command result message must be a string" message))
  (when command
    (record-command! host command))
  (cond (interaction-started?
         (set-message! host ""))
        ((or (memq status '(disabled cancelled error))
             (and (eq? status 'not-handled) consumed?))
         (set-message! host message)))
  (let ((completion-error (refresh-completion! host)))
    (when completion-error
      (set-message! host completion-error))))

(define (set-message! host message)
  (unless (string? message)
    (error "command message must be a string" message))
  (vector-set! (command-feedback-entry host) 0 message))

(define (completed-result tag values)
  (cond ((null? values) (vector tag))
        ((null? (cdr values)) (vector tag (car values)))
        (else (error "command-completed accepts at most one value"))))

(define (command-completed . values)
  (completed-result 'completed values))

(define (command-completed/preserve . values)
  (completed-result 'completed-preserve values))

(define (command-completed/collapse . values)
  (completed-result 'completed-collapse values))

(define (command-completed/selection selection . values)
  (cond ((null? values) (vector 'completed-selection selection))
        ((null? (cdr values))
         (vector 'completed-selection selection (car values)))
        (else (error "command-completed/selection accepts at most one value"))))

(define (command-prefix count register extra)
  (vector 'prefix count register extra))

(define (command-error message)
  (vector 'error message))

(define (command-dispatch name . arguments)
  (vector 'dispatch name (list->vector arguments)))

(define (command-dispatch-to name window buffer view . arguments)
  (vector 'dispatch-target name (list->vector arguments)
          (vector window buffer view)))

(define (interaction kind keymap input-state buffer-name prompt initial-input history provider
                     allow-custom-input? accept-command . arguments)
  (vector 'interaction
          kind
          keymap
          input-state
          buffer-name
          prompt
          initial-input
          history
          provider
          allow-custom-input?
          accept-command
          (list->vector arguments)))

(define (interaction-candidate value label detail filter-text)
  (vector value label detail filter-text))

(define (interaction-provider-task request transform)
  (unless (procedure? transform)
    (error "interaction provider transform must be a procedure" transform))
  (vector 'async-provider request transform))

(define (completion-provider-task request transform)
  (unless (procedure? transform)
    (error "completion provider transform must be a procedure" transform))
  (vector 'async-completion-provider request transform))

(define* (completion-item label
                          #:key
                          (kind "")
                          (detail "")
                          (filter-text label)
                          (sort-text label)
                          (insert-text label)
                          (documentation "")
                          (start #f)
                          (end #f))
  (vector 'completion-item label kind detail filter-text sort-text insert-text documentation
          start end))

(define (completion-item-field item index)
  (unless (and (vector? item)
               (= (vector-length item) 10)
               (eq? (vector-ref item 0) 'completion-item))
    (error "expected a completion item" item))
  (vector-ref item index))

(define (completion-item-label item) (completion-item-field item 1))
(define (completion-item-kind item) (completion-item-field item 2))
(define (completion-item-detail item) (completion-item-field item 3))
(define (completion-item-filter-text item) (completion-item-field item 4))
(define (completion-item-sort-text item) (completion-item-field item 5))
(define (completion-item-insert-text item) (completion-item-field item 6))
(define (completion-item-documentation item) (completion-item-field item 7))
(define (completion-item-start item) (completion-item-field item 8))
(define (completion-item-end item) (completion-item-field item 9))

(define* (completion-result candidates #:key (incomplete? #f))
  (vector 'completion-result
          (cond ((vector? candidates) candidates)
                ((list? candidates) (list->vector candidates))
                (else (error "completion candidates must be a list or vector" candidates)))
          (and incomplete? #t)))

(define (completion-request-field request index)
  (unless (and (vector? request)
               (= (vector-length request) 7)
               (eq? (vector-ref request 0) 'completion-request))
    (error "invalid completion request" request))
  (vector-ref request index))

(define (completion-request-query request) (completion-request-field request 1))
(define (completion-request-anchor request) (completion-request-field request 2))
(define (completion-request-caret request) (completion-request-field request 3))
(define (completion-request-line request) (completion-request-field request 4))
(define (completion-request-trigger request) (completion-request-field request 5))
(define (completion-request-trigger-character request) (completion-request-field request 6))

(define (context-value context key)
  (let ((entry (assq key context)))
    (and entry (cdr entry))))

(define (context-window context)
  (context-value context 'window))

(define (context-buffer context)
  (context-value context 'buffer))

(define (context-view context)
  (context-value context 'view))

(define (context-project context)
  (context-value context 'project))

(define default-keymap-root-policy
  (vector (vector 'editor.default)
          (vector 'application.global)
          (vector 'editor.system)
          (vector 'window.policy-created)
          (vector 'completion.active)))

(define-state-slot! 'keymap-roots (lambda () default-keymap-root-policy))

(define (keymap-name-vector value name)
  (let ((result (cond ((vector? value) value)
                      ((list? value) (list->vector value))
                      (else (error (string-append name " must be a list or vector") value)))))
    (let loop ((index 0))
      (when (< index (vector-length result))
        (let ((keymap (vector-ref result index)))
          (unless (or (symbol? keymap) (string? keymap))
            (error (string-append name " contains an invalid keymap name") keymap)))
        (loop (+ index 1))))
    result))

(define* (configure-keymap-policy! host
                                   #:key
                                   (editor (vector 'editor.default))
                                   (application (vector 'application.global))
                                   (overrides (vector 'editor.system))
                                   (policy-created-window (vector 'window.policy-created))
                                   (completion (vector 'completion.active)))
  (let ((policy (vector (keymap-name-vector editor "editor keymaps")
                        (keymap-name-vector application "application keymaps")
                        (keymap-name-vector overrides "override keymaps")
                        (keymap-name-vector policy-created-window
                                            "policy-created window keymaps")
                        (keymap-name-vector completion "completion keymaps"))))
    (state-set! host 'keymap-roots policy)))

(define (keymap-root-policy host)
  (state-ref host 'keymap-roots))

(define (keymap-name-text name)
  (if (symbol? name) (symbol->string name) name))

(define (append-keymap-layer layers keymap scope)
  (if (let loop ((remaining layers))
        (and (pair? remaining)
             (or (equal? keymap (vector-ref (car remaining) 1))
                 (loop (cdr remaining)))))
      layers
      (append layers (list (vector 'keymap-layer keymap scope)))))

(define (append-keymap-vector layers keymaps scope)
  (let loop ((index 0)
             (result layers))
    (if (= index (vector-length keymaps))
        result
        (loop (+ index 1)
              (append-keymap-layer result (vector-ref keymaps index) scope)))))

(define (append-state-layers layers states)
  (let loop ((index (- (vector-length states) 1))
             (result layers))
    (if (< index 0)
        result
        (let* ((state (vector-ref states index))
               (name (vector-ref state 1))
               (scope (string-append "input-state:" (keymap-name-text name)
                                     (if (= index 0) "" ":transient"))))
          (loop (- index 1)
                (append-keymap-vector result (vector-ref state 2) scope))))))

(define (append-mode-layers layers modes prefix)
  (let loop ((index (- (vector-length modes) 1))
             (result layers))
    (if (< index 0)
        result
        (let* ((mode (vector-ref modes index))
               (scope (string-append prefix (keymap-name-text (vector-ref mode 1)))))
          (loop (- index 1)
                (append-keymap-vector result (vector-ref mode 2) scope))))))

(define (assemble-base-keymap-layers snapshot roots initial)
  (let* ((kind (vector-ref snapshot 1))
         (window-maps (vector-ref snapshot 3))
         (view-maps (vector-ref snapshot 4))
         (buffer-maps (vector-ref snapshot 5))
         (minor-modes (vector-ref snapshot 6))
         (major-mode (vector-ref snapshot 7))
         (window-layers (append-keymap-vector initial window-maps "window"))
         (policy-window-layers
          (if (and (not (eq? kind 'minibuffer)) (vector-ref snapshot 8))
              (append-keymap-vector window-layers (vector-ref roots 3)
                                    "window:policy-created")
              window-layers))
         (view-layers (append-keymap-vector
                       policy-window-layers view-maps
                       (if (eq? kind 'minibuffer) "minibuffer" "view")))
         (buffer-layers (append-keymap-vector view-layers buffer-maps "buffer"))
         (minor-layers (append-mode-layers buffer-layers minor-modes "minor-mode:"))
         (major-layers (if major-mode
                           (append-keymap-vector
                            minor-layers (vector-ref major-mode 2)
                            (string-append "major-mode:"
                                           (keymap-name-text (vector-ref major-mode 1))))
                           minor-layers))
         (editor-layers (if (eq? kind 'minibuffer)
                            major-layers
                            (append-keymap-vector major-layers (vector-ref roots 0) "editor"))))
    (append-keymap-vector editor-layers (vector-ref roots 1) "global")))

(define (resolve-base-keymap-policy host context)
  (let* ((snapshot (keymap-context-snapshot host context))
         (roots (keymap-root-policy host))
         (layers (assemble-base-keymap-layers snapshot roots '())))
    (vector 'keymap-policy (list->vector layers) (vector))))

(define (resolve-keymap-policy host context)
  (let* ((snapshot (keymap-context-snapshot host context))
         (roots (keymap-root-policy host))
         (context-layers
          (if (vector-ref snapshot 9)
              (append-keymap-vector '() (vector-ref roots 4) "completion-active")
              '()))
         (state-layers (append-state-layers context-layers (vector-ref snapshot 2)))
         (layers (assemble-base-keymap-layers snapshot roots state-layers)))
    (vector 'keymap-policy (list->vector layers) (vector-ref roots 2))))

(define (keymap-policy-names policy include-overrides?)
  (let* ((layers (vector-ref policy 1))
         (overrides (vector-ref policy 2))
         (result-size (+ (if include-overrides? (vector-length overrides) 0)
                         (vector-length layers)))
         (result (make-vector result-size)))
    (let loop-overrides ((index 0))
      (when (and include-overrides? (< index (vector-length overrides)))
        (vector-set! result index (vector-ref overrides index))
        (loop-overrides (+ index 1))))
    (let ((offset (if include-overrides? (vector-length overrides) 0)))
      (let loop-layers ((index 0))
        (when (< index (vector-length layers))
          (vector-set! result (+ offset index)
                       (vector-ref (vector-ref layers index) 1))
          (loop-layers (+ index 1)))))
    result))

(define (base-keymap-layers host context)
  (keymap-policy-names (resolve-base-keymap-policy host context) #f))

(define (active-keymap-layers host context)
  (keymap-policy-names (resolve-keymap-policy host context) #t))

;; Presentation policies are configured procedures with no default: each is a
;; declared policy slot instead of a private table plus a hand-written
;; "configured or raise" accessor pair (design/09-guile-first.md §4).
(define-policy-slot! 'display)
(define-policy-slot! 'modeline)
(define-policy-slot! 'chrome)
(define-policy-slot! 'theme)
(define-policy-slot! 'style)
(define-policy-slot! 'motion)
(define-policy-slot! 'metrics)
(define-policy-slot! 'typography)

(define (configure-display-policy! host procedure)
  (policy-set! host 'display procedure))

(define (resolve-display-plan host facts)
  ((policy-ref host 'display) host facts))

(define (configure-modeline-policy! host procedure)
  (policy-set! host 'modeline procedure))

(define (resolve-modeline-content host context facts)
  (let ((effective-facts (list->vector (vector->list facts))))
    (vector-set! effective-facts 9
                 (if (equal? (context-window context) (active-window-id host))
                     (vector-ref (command-feedback-entry host) 1)
                     ""))
    ((policy-ref host 'modeline) host context effective-facts)))

(define (configure-chrome-policy! host procedure)
  (policy-set! host 'chrome procedure))

(define (resolve-chrome-content host context facts)
  (let ((effective-facts (list->vector (vector->list facts))))
    (vector-set! effective-facts 7
                 (vector-ref (command-feedback-entry host) 0))
    ((policy-ref host 'chrome) host context effective-facts)))

(define (configure-theme-policy! host procedure)
  (policy-set! host 'theme procedure))

(define (resolve-presentation-theme host)
  ((policy-ref host 'theme) host))

(define (configure-style-policy! host procedure)
  (policy-set! host 'style procedure))

(define (resolve-presentation-styles host)
  ((policy-ref host 'style) host (resolve-presentation-theme host)))

(define (configure-motion-policy! host procedure)
  (policy-set! host 'motion procedure))

(define (resolve-presentation-motion host)
  ((policy-ref host 'motion) host))

(define (configure-metrics-policy! host procedure)
  (policy-set! host 'metrics procedure))

(define (resolve-presentation-metrics host)
  ((policy-ref host 'metrics) host))

(define (configure-typography-policy! host procedure)
  (policy-set! host 'typography procedure))

(define (resolve-presentation-typography host)
  ((policy-ref host 'typography) host))

;; Resolves the theme once and hands that exact value to the style policy, so
;; the frontend snapshot cannot mix two theme resolutions.
(define (resolve-presentation-profile host)
  (let ((theme (resolve-presentation-theme host)))
    (vector 'presentation-profile
            theme
            ((policy-ref host 'style) host theme)
            (resolve-presentation-motion host)
            (resolve-presentation-metrics host)
            (resolve-presentation-typography host))))

(define (invocation-arguments invocation)
  (vector-ref invocation 1))

(define (invocation-repeat-count invocation)
  (vector-ref invocation 2))

(define (invocation-register invocation)
  (vector-ref invocation 3))

(define (invocation-prefix-extra invocation)
  (vector-ref invocation 4))

(define (selection-range anchor head granularity)
  (vector anchor head granularity))

(define (selection-primary value)
  (vector-ref value 1))

(define (selection-metadata value)
  (vector-ref value 2))

(define (selection-ranges value)
  (vector-ref value 3))

(define (selection-range-anchor range)
  (vector-ref range 0))

(define (selection-range-head range)
  (vector-ref range 1))

(define (selection-range-granularity range)
  (vector-ref range 2))

(define (selection ranges . options)
  (let ((primary (if (pair? options) (car options) 0))
        (metadata (if (and (pair? options) (pair? (cdr options)))
                      (cadr options)
                      '())))
    (if (and (pair? options) (pair? (cdr options)) (pair? (cddr options)))
        (error "selection accepts ranges, optional primary, and optional metadata")
        (vector 'selection primary metadata (list->vector ranges)))))

(define (selection-with-metadata value metadata)
  (selection (vector->list (selection-ranges value))
             (selection-primary value)
             metadata))

(define (selection-with-ranges value ranges)
  (selection ranges
             (selection-primary value)
             (selection-metadata value)))
