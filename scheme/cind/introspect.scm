(define-module (cind introspect)
  #:use-module (ice-9 documentation)
  #:use-module (ice-9 format)
  #:use-module (srfi srfi-1)
  #:use-module (srfi srfi-13)
  #:use-module (cind command)
  #:use-module (cind host)
  #:use-module (cind input)
  #:use-module (cind minibuffer)
  #:export (introspection-command-definitions
            introspection-providers
            install-introspection-documentation!))

(define help-buffer-name "*Help*")

(define bundled-module-names
  '((cind command)
    (cind input)
    (cind development)
    (cind extension)
    (cind emacs)
    (cind toy-modal)
    (cind meow)
    (cind minibuffer)
    (cind vim)
    (cind helix)
    (cind structural)
    (cind introspect)
    (cind core)))

(define (last-string-argument invocation)
  (let ((arguments (invocation-arguments invocation)))
    (and (> (vector-length arguments) 0)
         (let ((argument (vector-ref arguments (- (vector-length arguments) 1))))
           (and (string? argument) argument)))))

(define (display-help! host context text)
  (display-generated-buffer! host (context-window context) help-buffer-name text
                             'special-mode "help"))

(define (vector->strings values)
  (let loop ((index 0) (result '()))
    (if (= index (vector-length values))
        (reverse result)
        (loop (+ index 1) (cons (format #f "~a" (vector-ref values index)) result)))))

(define (indented-lines values empty-text)
  (if (null? values)
      (string-append "  " empty-text "\n")
      (string-concatenate
       (map (lambda (value) (string-append "  " value "\n")) values))))

(define (command-help-text host context name . key-sequence)
  (let ((properties (command-properties host context name)))
    (if (not properties)
        (string-append "Unknown command: " name "\n")
        (let ((documentation (vector-ref properties 1))
              (source (vector-ref properties 2))
              (enabled? (vector-ref properties 3))
              (bindings (vector-ref properties 4)))
          (string-append
           name " is a command.\n\n"
           (if (null? key-sequence)
               ""
               (string-append "Key sequence: " (car key-sequence) "\n"))
           "Status: " (if enabled? "enabled" "disabled") "\n"
           "Source: " source "\n\n"
           "Key bindings:\n"
           (indented-lines (vector->strings bindings) "none in the active keymap stack")
           "\n"
           (if documentation documentation "No documentation is registered.")
           "\n")))))

(define (describe-command-name! host context name)
  (display-help! host context (command-help-text host context name)))

(define (describe-command context invocation)
  (completing-read "Describe command: " "commands"
                   "help.describe-command.accept"
                   #:history "describe-command"))

(define (describe-command-accept host context invocation)
  (let ((name (last-string-argument invocation)))
    (if (not name)
        (command-error "describe-command requires a command name")
        (begin
          (describe-command-name! host context name)
          (command-completed)))))

(define (describe-key-finish! host context sequence resolution)
  (let ((kind (vector-ref resolution 0)))
    (if (eq? kind 'command)
        (display-help! host context
                       (command-help-text host context
                                          (symbol->string (vector-ref resolution 1))
                                          sequence))
        (display-help! host context
                       (string-append sequence " is undefined in the active keymap stack.\n")))))

(define (describe-key-read! host context layers sequence)
  (read-key-then!
   host (context-view context)
   (lambda (key)
     (let* ((next (if (= (string-length sequence) 0)
                      key
                      (string-append sequence " " key)))
            (resolution (resolve-key-sequence host layers next))
            (kind (vector-ref resolution 0)))
       (if (eq? kind 'prefix)
           (describe-key-read! host context layers next)
           (describe-key-finish! host context next resolution))
       'consume))
   #:sequence sequence
   #:hints (key-sequence-completions host layers sequence)))

(define (describe-key host context invocation)
  (let ((layers (active-keymap-layers host context)))
    (describe-key-read! host context layers "")
    (command-completed/preserve)))

(define (describe-bindings host context invocation)
  (let ((bindings (active-key-bindings host)))
    (display-help!
     host context
     (string-append
      "Active key bindings\n\n"
      (if (= (vector-length bindings) 0)
          "  none\n"
          (let loop ((index 0) (lines '()))
            (if (= index (vector-length bindings))
                (string-concatenate-reverse lines)
                (let ((binding (vector-ref bindings index)))
                  (loop (+ index 1)
                        (cons (format #f "  ~a  ~a\n"
                                      (vector-ref binding 0)
                                      (vector-ref binding 1))
                              lines))))))))
    (command-completed)))

(define (mode-properties-text host mode)
  (let* ((properties (mode-properties host mode))
         (parent (vector-ref properties 2))
         (interaction (vector-ref properties 3))
         (initial-state (vector-ref properties 4))
         (things (vector-ref properties 5))
         (keymaps (vector-ref properties 6))
         (language (vector-ref properties 7)))
    (string-append
     (format #f "~a (~a mode)\n" (vector-ref properties 0) (vector-ref properties 1))
     (format #f "  parent: ~a\n" (if parent parent "none"))
     (format #f "  language profile: ~a\n" (if language language "none"))
     (format #f "  interaction class: ~a\n" (if interaction interaction "inherited"))
     (format #f "  initial input state: ~a\n" (if initial-state initial-state "inherited"))
     "  keymaps:\n" (indented-lines (vector->strings keymaps) "none")
     "  semantic things:\n"
     (if (null? things)
         "    none\n"
         (string-concatenate
          (map (lambda (entry) (format #f "    ~a -> ~a\n" (car entry) (cdr entry)))
               things))))))

(define (describe-mode host context invocation)
  (let* ((summary (buffer-mode-summary host (context-buffer context)))
         (major (vector-ref summary 0))
         (minors (vector-ref summary 1))
         (policy (vector-ref summary 2)))
    (display-help!
     host context
     (string-append
      "Mode state for the current buffer\n\n"
      "Major mode:\n"
      (if major (mode-properties-text host major) "  none\n")
      "\nMinor modes:\n"
      (if (= (vector-length minors) 0)
          "  none\n"
          (string-concatenate
           (map (lambda (mode) (string-append (mode-properties-text host mode) "\n"))
                (vector->list minors))))
      "\nEffective policy:\n"
      (format #f "  interaction class: ~a\n" (vector-ref policy 0))
      (format #f "  initial input state: ~a\n"
              (if (vector-ref policy 1) (vector-ref policy 1) "none"))
      "  semantic things:\n"
      (if (null? (vector-ref policy 2))
          "    none\n"
          (string-concatenate
           (map (lambda (entry) (format #f "    ~a -> ~a\n" (car entry) (cdr entry)))
                (vector-ref policy 2))))))
    (command-completed)))

(define (introspection-modules host)
  (append
   (map (lambda (name) (vector (format #f "~s" name) (resolve-module name)))
        bundled-module-names)
  (let ((extensions (owned-user-modules host)))
     (let loop ((index 0) (result '()))
       (if (= index (vector-length extensions))
           (reverse result)
           (let ((entry (vector-ref extensions index)))
             (loop (+ index 1)
                   (cons (vector (vector-ref entry 0) (vector-ref entry 1)) result))))))))

(define (safe-variable-value variable)
  (and (variable-bound? variable)
       (catch #t (lambda () (vector (variable-ref variable))) (lambda arguments #f))))

(define (binding-token module-label name)
  (format #f "~s" (list module-label (symbol->string name))))

(define (binding-candidates host procedures-only?)
  (let loop ((modules (introspection-modules host)) (result '()))
    (if (null? modules)
        (list->vector (sort result
                            (lambda (left right)
                              (string<? (vector-ref left 1) (vector-ref right 1)))))
        (let* ((entry (car modules))
               (label (vector-ref entry 0))
               (module (vector-ref entry 1))
               (bindings
                (module-map
                 (lambda (name variable)
                   (let ((value-box (safe-variable-value variable)))
                     (and value-box
                          (or (not procedures-only?)
                              (procedure? (vector-ref value-box 0)))
                          (interaction-candidate
                           (binding-token label name)
                           (symbol->string name)
                           label
                           (string-append (symbol->string name) " " label)))))
                 module)))
          (loop (cdr modules) (append (filter identity bindings) result))))))

(define (scheme-functions-provider host context query)
  (binding-candidates host #t))

(define (scheme-variables-provider host context query)
  (binding-candidates host #f))

(define (parse-binding-token token)
  (catch #t
    (lambda ()
      (let ((value (call-with-input-string token read)))
        (and (list? value) (= (length value) 2)
             (string? (car value)) (string? (cadr value)) value)))
    (lambda arguments #f)))

(define (lookup-binding host token)
  (let ((parts (parse-binding-token token)))
    (and parts
         (let loop ((modules (introspection-modules host)))
           (and (pair? modules)
                (let ((entry (car modules)))
                  (if (string=? (vector-ref entry 0) (car parts))
                      (let* ((name (string->symbol (cadr parts)))
                             (variable (module-local-variable (vector-ref entry 1) name)))
                        (and variable (variable-bound? variable)
                             (vector (car parts) name (variable-ref variable))))
                      (loop (cdr modules)))))))))

(define (bounded-object-string value)
  (let* ((rendered
          (catch #t
            (lambda () (call-with-output-string (lambda (port) (write value port))))
            (lambda arguments "#<unprintable>")))
         (limit 4000))
    (if (> (string-length rendered) limit)
        (string-append (substring rendered 0 limit) "…")
        rendered)))

(define (procedure-detail value)
  (let ((documentation
         (catch #t (lambda () (object-documentation value)) (lambda arguments #f)))
        (arity
         (catch #t (lambda () (bounded-object-string (procedure-minimum-arity value)))
                (lambda arguments "unknown")))
        (source
         (catch #t (lambda () (procedure-source value)) (lambda arguments #f))))
    (string-append
     "Kind: function\n"
     "Arity: " arity "\n"
     (if source (string-append "Source form: " (bounded-object-string source) "\n") "")
     "\n"
     (if documentation documentation "No documentation is attached to this function.")
     "\n")))

(define (binding-help-text binding requested-kind)
  (let ((module-label (vector-ref binding 0))
        (name (vector-ref binding 1))
        (value (vector-ref binding 2)))
    (if (and (eq? requested-kind 'function) (not (procedure? value)))
        (format #f "~a is not a function.\n" name)
        (string-append
         (format #f "~a is defined in ~a.\n\n" name module-label)
         (if (procedure? value)
             (procedure-detail value)
             (string-append "Kind: variable\nValue:\n  "
                            (bounded-object-string value) "\n"))))))

(define (describe-binding-accept host context invocation kind)
  (let* ((token (last-string-argument invocation))
         (binding (and token (lookup-binding host token))))
    (if (not binding)
        (command-error "the selected Scheme binding is no longer available")
        (begin
          (display-help! host context (binding-help-text binding kind))
          (command-completed)))))

(define (describe-function context invocation)
  (completing-read "Describe function: " "scheme-functions"
                   "help.describe-function.accept"
                   #:history "describe-function"))

(define (describe-variable context invocation)
  (completing-read "Describe variable: " "scheme-variables"
                   "help.describe-variable.accept"
                   #:history "describe-variable"))

(define (introspection-command-definitions host)
  (list
   (list "help.describe-command" describe-command #f)
   (list "help.describe-command.accept"
         (lambda (context invocation) (describe-command-accept host context invocation)) #f)
   (list "help.describe-key"
         (lambda (context invocation) (describe-key host context invocation)) #f)
   (list "help.describe-bindings"
         (lambda (context invocation) (describe-bindings host context invocation)) #f)
   (list "help.describe-mode"
         (lambda (context invocation) (describe-mode host context invocation)) #f)
   (list "help.describe-function" describe-function #f)
   (list "help.describe-function.accept"
         (lambda (context invocation)
           (describe-binding-accept host context invocation 'function)) #f)
   (list "help.describe-variable" describe-variable #f)
   (list "help.describe-variable.accept"
         (lambda (context invocation)
           (describe-binding-accept host context invocation 'variable)) #f)))

(define (introspection-providers host)
  (list
   (cons "scheme-functions"
         (lambda (context query) (scheme-functions-provider host context query)))
   (cons "scheme-variables"
         (lambda (context query) (scheme-variables-provider host context query)))))

(define command-documentation
  '(("help.describe-command" . "Display the registry metadata, active bindings, and documentation for a command.")
    ("help.describe-key" . "Read a key sequence and describe the command resolved by the active keymap stack.")
    ("help.describe-bindings" . "Display all bindings in the active keymap stack.")
    ("help.describe-mode" . "Display the current buffer's major mode, minor modes, and effective mode policy.")
    ("help.describe-function" . "Describe a Scheme function from bundled or loaded extension modules.")
    ("help.describe-variable" . "Describe a Scheme binding and its current value.")))

(define (install-introspection-documentation! host)
  (for-each (lambda (entry)
              (set-command-documentation! host (car entry) (cdr entry)))
            command-documentation))
