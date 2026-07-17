(define-module (cind development)
  #:use-module (ice-9 format)
  #:use-module (srfi srfi-1)
  #:use-module (srfi srfi-13)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (development-command-definitions
            install-development-documentation!
            make-evaluation-module
            evaluate-source))

(define evaluation-buffer-name "*Scheme Evaluation*")
(define evaluation-modules (make-weak-key-hash-table))

(define (make-evaluation-module host)
  (or (hashq-ref evaluation-modules host)
      (let ((module (make-fresh-user-module)))
        (module-use! module (resolve-interface '(cind command)))
        (module-use! module (resolve-interface '(cind input)))
        (module-use! module (resolve-interface '(cind host)))
        (module-define! module 'host host)
        (hashq-set! evaluation-modules host module)
        module)))

(define (read-source source source-name)
  (let ((port (open-input-string source)))
    (set-port-filename! port source-name)
    (let loop ((forms '()))
      (let ((form (read port)))
        (if (eof-object? form)
            (reverse forms)
            (loop (cons form forms)))))))

(define (render-value value)
  (call-with-output-string
   (lambda (port)
     (write value port))))

(define (evaluate-forms module forms)
  (let loop ((remaining forms) (values '()))
    (if (null? remaining)
        values
        (call-with-values
            (lambda () (eval (car remaining) module))
          (lambda results
            (loop (cdr remaining) results))))))

(define (evaluate-source module source source-name)
  (let ((output-port (open-output-string))
        (error-port (open-output-string)))
    (catch #t
      (lambda ()
        (let ((values
               (with-output-to-port output-port
                 (lambda ()
                   (with-error-to-port error-port
                     (lambda ()
                       (evaluate-forms module (read-source source source-name))))))))
          (vector 'ok
                  (list->vector
                   (map render-value (remove unspecified? values)))
                  (get-output-string output-port)
                  (get-output-string error-port))))
      (lambda (key . arguments)
        (vector 'error
                (format #f "~S: ~S" key arguments)
                (get-output-string output-port)
                (get-output-string error-port))))))

(define (last-string-argument invocation)
  (let ((arguments (invocation-arguments invocation)))
    (and (> (vector-length arguments) 0)
         (let ((argument (vector-ref arguments (- (vector-length arguments) 1))))
           (and (string? argument) argument)))))

(define (buffer-source-name host buffer)
  (or (buffer-resource host buffer)
      (buffer-name host buffer)))

(define (string-lines text)
  (if (string-null? text)
      '()
      (string-split text #\newline)))

(define (result-text source-name values output error-output)
  (string-append
   "Scheme evaluation: " source-name "\n\n"
   (if (string-null? output)
       ""
       (string-append "Output:\n" output
                      (if (string-suffix? "\n" output) "" "\n") "\n"))
   (if (string-null? error-output)
       ""
       (string-append "Error output:\n" error-output
                      (if (string-suffix? "\n" error-output) "" "\n") "\n"))
   "Values:\n"
   (if (= (vector-length values) 0)
       "  <unspecified>\n"
       (string-concatenate
        (map (lambda (value) (string-append "  " value "\n"))
             (vector->list values))))))

(define (present-evaluation! host context source-name result always-buffer?)
  (let ((status (vector-ref result 0)))
    (if (eq? status 'error)
        (let ((message (vector-ref result 1))
              (output (vector-ref result 2))
              (error-output (vector-ref result 3)))
          (if (or (not (string-null? output))
                  (not (string-null? error-output)))
              (display-generated-buffer!
               host (context-window context) evaluation-buffer-name
               (string-append
                "Scheme evaluation: " source-name "\n\n"
                "Error:\n  " message "\n\n"
                (if (string-null? output) "" (string-append "Output:\n" output "\n"))
                (if (string-null? error-output)
                    ""
                    (string-append "Error output:\n" error-output "\n")))))
          (command-error message))
        (let* ((values (vector-ref result 1))
               (output (vector-ref result 2))
               (error-output (vector-ref result 3))
               (single-value? (= (vector-length values) 1))
               (compact? (and single-value?
                              (string-null? output)
                              (string-null? error-output)
                              (null? (cdr (string-lines (vector-ref values 0)))))))
          (if (and compact? (not always-buffer?))
              (set-message! host (vector-ref values 0))
              (display-generated-buffer!
               host (context-window context) evaluation-buffer-name
               (result-text source-name values output error-output)))
          (command-completed)))))

(define (evaluate! host context source source-name always-buffer?)
  (present-evaluation! host context source-name
                       (evaluate-scheme! host source source-name)
                       always-buffer?))

(define (eval-expression context invocation)
  (interaction 'text "Eval: " "" "scheme-expression" "" #t
               "scheme.eval-expression.accept"))

(define (eval-expression-accept host context invocation)
  (let ((source (last-string-argument invocation)))
    (if source
        (evaluate! host context source "<minibuffer>" #f)
        (command-error "eval-expression requires Scheme source"))))

(define (primary-region-source host context)
  (let* ((view (context-view context))
         (mark (view-mark host view))
         (selected (view-selection host view))
         (ranges (selection-ranges selected))
         (primary (selection-primary selected)))
    (and mark
         (let* ((range (vector-ref ranges primary))
                (anchor (selection-range-anchor range))
                (head (selection-range-head range)))
           (buffer-substring host (context-buffer context)
                             (min anchor head) (max anchor head))))))

(define (region-active? host context)
  (and (view-mark host (context-view context)) #t))

(define (eval-region host context invocation)
  (let ((source (primary-region-source host context))
        (name (buffer-source-name host (context-buffer context))))
    (if source
        (evaluate! host context source name #f)
        (command-error "eval-region requires an active region"))))

(define (eval-buffer host context invocation)
  (let ((buffer (context-buffer context)))
    (evaluate! host context (buffer-text host buffer)
               (buffer-source-name host buffer) #t)))

(define (development-command-definitions host)
  (list
   (list "scheme.eval-expression" eval-expression #f)
   (list "scheme.eval-expression.accept"
         (lambda (context invocation)
           (eval-expression-accept host context invocation))
         #f)
   (list "scheme.eval-region"
         (lambda (context invocation)
           (eval-region host context invocation))
         (lambda (context) (region-active? host context)))
   (list "scheme.eval-buffer"
         (lambda (context invocation)
           (eval-buffer host context invocation))
         #f)))

(define command-documentation
  '(("scheme.eval-expression" .
     "Read Scheme source in the minibuffer and evaluate it in the application user module.")
    ("scheme.eval-region" .
     "Evaluate the primary active region in the application user module.")
    ("scheme.eval-buffer" .
     "Evaluate the current buffer in the application user module.")))

(define (install-development-documentation! host)
  (for-each (lambda (entry)
              (set-command-documentation! host (car entry) (cdr entry)))
            command-documentation))
