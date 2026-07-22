(define-module (cind development)
  #:use-module (ares repl)
  #:use-module (cind buffers)
  #:use-module (rnrs bytevectors)
  #:use-module (srfi srfi-1)
  #:use-module (srfi srfi-13)
  #:use-module (cind async)
  #:use-module (cind command)
  #:use-module (cind host)
  #:use-module (cind minibuffer)
  #:use-module (cind state)
  #:export (development-command-definitions
            install-development-documentation!
            make-evaluation-module
            evaluate-source))

(define evaluation-buffer-name "*Scheme Evaluation*")
(define repl-buffer-name "*Scheme REPL*")
(define repl-history-name "scheme-repl-buffer")
(define repl-prompt "scheme> ")
(define repl-banner
  (string-append "Cind Scheme REPL\n"
                 "Definitions share the editor's persistent evaluation module.\n\n"
                 repl-prompt))
(define-state-slot! 'evaluation-module (lambda () #f))
(define-state-slot! 'repl-buffers (lambda () (make-hash-table)))

(define (utf8-length text)
  (bytevector-length (string->utf8 text)))

(define (host-repl-states host)
  (state-ref host 'repl-buffers))

(define (repl-state host buffer)
  (hash-ref (host-repl-states host) buffer))

(define (set-repl-state! host buffer state)
  (hash-set! (host-repl-states host) buffer state))

(define (make-evaluation-module host)
  (or (state-ref host 'evaluation-module)
      (let ((module (make-fresh-user-module)))
        (module-use! module (resolve-interface '(cind application)))
        (module-use! module (resolve-interface '(cind command)))
        (module-use! module (resolve-interface '(cind async)))
        (module-use! module (resolve-interface '(cind input)))
        (module-use! module (resolve-interface '(cind minibuffer)))
        (module-use! module (resolve-interface '(cind host)))
        (module-define! module 'host host)
        (state-set! host 'evaluation-module module))))

(define (render-value value)
  (call-with-output-string
   (lambda (port)
     (write value port))))

(define (evaluate-source module source source-name)
  (let ((result (repl-evaluate (make-repl module) source source-name)))
    (if (eq? (repl-result-status result) 'ok)
        (vector 'ok
                (list->vector
                 (map render-value
                      (remove unspecified? (repl-result-values result))))
                (repl-result-output result)
                (repl-result-error-output result))
        (vector 'error
                (repl-result-error result)
                (repl-result-output result)
                (repl-result-error-output result)))))

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
                    (string-append "Error output:\n" error-output "\n")))
               'special-mode "scheme evaluation" 'tools))
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
               (result-text source-name values output error-output)
               'special-mode "scheme evaluation" 'tools))
          (command-completed)))))

(define (evaluate! host context source source-name always-buffer?)
  (present-evaluation! host context source-name
                       (evaluate-scheme! host source source-name)
                       always-buffer?))

(define (eval-expression context invocation)
  (completing-read "Eval: " "scheme-repl" "scheme.eval-expression.accept"
                   #:history "scheme-expression"
                   #:allow-custom-input? #t))

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

(define (repl-stream text)
  (cond ((string-null? text) "")
        ((string-suffix? "\n" text) text)
        (else (string-append text "\n"))))

(define (repl-output result)
  (let ((status (vector-ref result 0)))
    (if (eq? status 'error)
        (string-append
         (repl-stream (vector-ref result 2))
         (repl-stream (vector-ref result 3))
         "error: " (vector-ref result 1) "\n")
        (let ((values (vector-ref result 1)))
          (string-append
           (repl-stream (vector-ref result 2))
           (repl-stream (vector-ref result 3))
           (string-concatenate
            (map (lambda (value) (string-append "=> " value "\n"))
                 (vector->list values))))))))

(define (replace-repl-input! host context state input)
  (let* ((view (context-view context))
         (buffer (context-buffer context))
         (start (buffer-marker-offset host buffer (vector-ref state 0)))
         (end (buffer-byte-size host buffer)))
    (replace-selection!
     host view (selection (list (selection-range start end 'char))) input)
    (set-view-caret! host view (+ start (utf8-length input)))
    (command-completed/preserve)))

(define (open-repl host context invocation)
  (let* ((existing (buffer-id-by-name host repl-buffer-name))
         (buffer (or existing
                     (create-buffer! host repl-buffer-name repl-banner
                                     'process #f #f 'scheme-repl-mode #f
                                     "scheme repl")))
         (window (context-window context)))
    (unless (repl-state host buffer)
      (set-repl-state! host buffer
                       (vector (create-buffer-marker!
                                host buffer (buffer-byte-size host buffer) 'before)
                               #f "")))
    (set-buffer-editable-start!
     host buffer
     (buffer-marker-offset host buffer (vector-ref (repl-state host buffer) 0)))
    (let ((target (display-buffer! host window buffer 'tools)))
      (set-view-caret! host (window-view-id host target)
                       (buffer-byte-size host buffer)))
    (command-completed)))

(define (submit-repl host context invocation)
  (let* ((buffer (context-buffer context))
         (state (repl-state host buffer)))
    (if (not state)
        (command-error "current buffer is not a Scheme REPL")
        (let* ((marker (vector-ref state 0))
               (start (buffer-marker-offset host buffer marker))
               (end (buffer-byte-size host buffer))
               (source (buffer-substring host buffer start end))
               (result (and (not (string-null? source))
                            (evaluate-scheme! host source repl-buffer-name)))
               (transcript
                (string-append "\n"
                               (if result (repl-output result) "")
                               repl-prompt)))
          (record-minibuffer-history! host repl-history-name source)
          (set-view-caret! host (context-view context) end)
          (insert-text! host (context-view context) transcript)
          (let ((new-end (buffer-byte-size host buffer)))
            (remove-buffer-marker! host buffer marker)
            (vector-set! state 0
                         (create-buffer-marker! host buffer new-end 'before))
            (set-buffer-editable-start! host buffer new-end)
            (vector-set! state 1 #f)
            (vector-set! state 2 "")
            (set-view-caret! host (context-view context) new-end))
          (command-completed/preserve)))))

(define (move-repl-history host context delta)
  (let* ((buffer (context-buffer context))
         (state (repl-state host buffer)))
    (if (not state)
        (command-error "current buffer is not a Scheme REPL")
        (let* ((entries (minibuffer-history host repl-history-name))
               (count (vector-length entries))
               (index (vector-ref state 1))
               (start (buffer-marker-offset host buffer (vector-ref state 0)))
               (current (buffer-substring host buffer start
                                          (buffer-byte-size host buffer)))
               (target
                (cond ((zero? count) #f)
                      ((< delta 0)
                       (if index (max 0 (- index 1)) (- count 1)))
                      ((not index) #f)
                      ((< (+ index 1) count) (+ index 1))
                      (else count))))
          (cond ((not target) (command-completed/preserve))
                ((= target count)
                 (vector-set! state 1 #f)
                 (replace-repl-input! host context state (vector-ref state 2)))
                (else
                 (unless index (vector-set! state 2 current))
                 (vector-set! state 1 target)
                 (replace-repl-input! host context state
                                      (vector-ref entries target))))))))

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
         #f)
   (list "scheme.repl"
         (lambda (context invocation)
           (open-repl host context invocation))
         #f)
   (list "scheme.repl.submit"
         (lambda (context invocation)
           (submit-repl host context invocation))
         #f)
   (list "scheme.repl.previous-input"
         (lambda (context invocation)
           (move-repl-history host context -1))
         #f)
   (list "scheme.repl.next-input"
         (lambda (context invocation)
           (move-repl-history host context 1))
         #f)))

(define command-documentation
  '(("scheme.eval-expression" .
     "Read Scheme source in the minibuffer and evaluate it in the application user module.")
    ("scheme.eval-region" .
     "Evaluate the primary active region in the application user module.")
    ("scheme.eval-buffer" .
     "Evaluate the current buffer in the application user module.")
    ("scheme.repl" .
     "Open the editor-owned Scheme REPL buffer for the application user module.")
    ("scheme.repl.submit" .
     "Evaluate the current Scheme REPL input and append its result to the transcript.")
    ("scheme.repl.previous-input" .
     "Replace the current Scheme REPL input with the previous history entry.")
    ("scheme.repl.next-input" .
     "Replace the current Scheme REPL input with the next history entry.")))

(define (install-development-documentation! host)
  (for-each (lambda (entry)
              (set-command-documentation! host (car entry) (cdr entry)))
            command-documentation))
