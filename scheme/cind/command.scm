(define-module (cind command)
  #:export (command-completed
            command-completed/preserve
            command-completed/collapse
            command-completed/selection
            command-prefix
            command-error
            command-dispatch
            interaction
            interaction-candidate
            context-window
            context-buffer
            context-view
            context-project
            invocation-arguments
            invocation-repeat-count
            invocation-register
            invocation-prefix-extra
            selection
            selection-range))

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

(define (interaction kind prompt initial-input history provider
                     allow-custom-input? accept-command . arguments)
  (vector 'interaction
          kind
          prompt
          initial-input
          history
          provider
          allow-custom-input?
          accept-command
          (list->vector arguments)))

(define (interaction-candidate value label detail filter-text)
  (vector value label detail filter-text))

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

(define (selection ranges . options)
  (let ((primary (if (pair? options) (car options) 0))
        (metadata (if (and (pair? options) (pair? (cdr options)))
                      (cadr options)
                      '())))
    (if (and (pair? options) (pair? (cdr options)) (pair? (cddr options)))
        (error "selection accepts ranges, optional primary, and optional metadata")
        (vector 'selection primary metadata (list->vector ranges)))))
