(define-module (cind command)
  #:export (command-completed
            command-error
            command-dispatch
            interaction
            interaction-candidate
            context-window
            context-buffer
            context-view
            context-project
            invocation-arguments
            invocation-repeat-count))

(define (command-completed . values)
  (cond ((null? values) (vector 'completed))
        ((null? (cdr values)) (vector 'completed (car values)))
        (else (error "command-completed accepts at most one value"))))

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
