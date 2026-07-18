(define-module (cind command)
  #:use-module (ice-9 optargs)
  #:use-module (cind host)
  #:export (command-completed
            command-completed/preserve
            command-completed/collapse
            command-completed/selection
            command-prefix
            command-error
            command-dispatch
            command-dispatch-to
            interaction
            interaction-candidate
            interaction-provider-task
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
            configure-chrome-policy!
            resolve-chrome-content
            configure-theme-policy!
            resolve-presentation-theme
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

(define (interaction kind keymap input-state prompt initial-input history provider
                     allow-custom-input? accept-command . arguments)
  (vector 'interaction
          kind
          keymap
          input-state
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

(define keymap-root-policies (make-weak-key-hash-table))

(define default-keymap-root-policy
  (vector (vector 'editor.default)
          (vector 'application.global)
          (vector 'editor.system)))

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
                                   (overrides (vector 'editor.system)))
  (let ((policy (vector (keymap-name-vector editor "editor keymaps")
                        (keymap-name-vector application "application keymaps")
                        (keymap-name-vector overrides "override keymaps"))))
    (hashq-set! keymap-root-policies host policy)
    policy))

(define (keymap-root-policy host)
  (or (hashq-ref keymap-root-policies host) default-keymap-root-policy))

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
         (view-layers (append-keymap-vector
                       window-layers view-maps
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
         (state-layers (append-state-layers '() (vector-ref snapshot 2)))
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

(define modeline-policies (make-weak-key-hash-table))

(define (configure-modeline-policy! host procedure)
  (unless (procedure? procedure)
    (error "modeline policy must be a procedure" procedure))
  (hashq-set! modeline-policies host procedure)
  procedure)

(define (resolve-modeline-content host context facts)
  (let ((procedure (hashq-ref modeline-policies host)))
    (unless procedure
      (error "modeline policy is not configured"))
    (procedure host context facts)))

(define chrome-policies (make-weak-key-hash-table))

(define (configure-chrome-policy! host procedure)
  (unless (procedure? procedure)
    (error "chrome policy must be a procedure" procedure))
  (hashq-set! chrome-policies host procedure)
  procedure)

(define (resolve-chrome-content host context facts)
  (let ((procedure (hashq-ref chrome-policies host)))
    (unless procedure
      (error "chrome policy is not configured"))
    (procedure host context facts)))

(define theme-policies (make-weak-key-hash-table))

(define (configure-theme-policy! host procedure)
  (unless (procedure? procedure)
    (error "theme policy must be a procedure" procedure))
  (hashq-set! theme-policies host procedure)
  procedure)

(define (resolve-presentation-theme host)
  (let ((procedure (hashq-ref theme-policies host)))
    (unless procedure
      (error "theme policy is not configured"))
    (procedure host)))

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
