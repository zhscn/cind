(define-module (cind ares)
  #:use-module (ares completion)
  #:use-module (ares repl)
  #:use-module (rnrs bytevectors)
  #:use-module (cind command)
  #:use-module (cind development)
  #:use-module (cind host)
  #:export (ares-provider-definitions
            ares-completion-provider-definitions))

(define (scheme-delimiter? character)
  (or (char-whitespace? character)
      (memv character '(#\( #\) #\[ #\] #\{ #\} #\' #\` #\, #\; #\"))))

(define (completion-prefix-start query)
  (let loop ((index (string-length query)))
    (cond ((zero? index) 0)
          ((scheme-delimiter? (string-ref query (- index 1))) index)
          (else (loop (- index 1))))))

(define (candidate-detail candidate)
  (string-append (completion-candidate-type candidate)
                 "  "
                 (completion-candidate-namespace candidate)))

(define (scheme-repl-candidates host query)
  (let* ((prefix-start (completion-prefix-start query))
         (source-prefix (substring query 0 prefix-start))
         (symbol-prefix (substring query prefix-start (string-length query)))
         (candidates
          (if (zero? (string-length symbol-prefix))
              #()
              (repl-complete (make-repl (make-evaluation-module host)) symbol-prefix))))
    (let loop ((index 0)
               (result '()))
      (if (= index (vector-length candidates))
          (list->vector (reverse result))
          (let* ((candidate (vector-ref candidates index))
                 (name (completion-candidate-name candidate))
                 (value (string-append source-prefix name)))
            (loop (+ index 1)
                  (cons (vector value name (candidate-detail candidate) value)
                        result)))))))

(define (ares-provider-definitions host)
  (list
   (cons "scheme-repl"
         (lambda (context query)
           (scheme-repl-candidates host query)))))

(define (scheme-byte-delimiter? byte)
  (or (memv byte '(9 10 11 12 13 32))
      (memv byte '(34 39 40 41 44 59 91 93 96 123 124 125))))

(define (scheme-symbol-prefix text caret)
  (let* ((bytes (string->utf8 text))
         (limit (min caret (bytevector-length bytes))))
    (let loop ((start limit))
      (if (or (zero? start)
              (scheme-byte-delimiter? (bytevector-u8-ref bytes (- start 1))))
          (let* ((length (- limit start))
                 (prefix (make-bytevector length)))
            (bytevector-copy! bytes start prefix 0 length)
            (values (utf8->string prefix) start))
          (loop (- start 1))))))

(define (ares-document-completions host context request)
  (call-with-values
      (lambda ()
        (scheme-symbol-prefix (buffer-text host (context-buffer context))
                              (completion-request-caret request)))
    (lambda (prefix start)
      (let ((candidates
             (if (zero? (string-length prefix))
                 #()
                 (repl-complete-summaries
                  (make-repl (make-evaluation-module host)) prefix))))
        (let loop ((index 0)
                   (result '()))
          (if (= index (vector-length candidates))
              (completion-result (reverse result))
              (let* ((candidate (vector-ref candidates index))
                     (name (completion-candidate-name candidate)))
                (loop (+ index 1)
                      (cons (completion-item
                             name
                             #:kind (completion-candidate-type candidate)
                             #:detail (completion-candidate-namespace candidate)
                             #:start start
                             #:end (completion-request-caret request))
                            result)))))))))

(define (ares-resolve-document-completion host context request item)
  (let* ((metadata
          (repl-lookup (make-repl (make-evaluation-module host))
                       (string->symbol (completion-item-label item))))
         (documentation (or (and metadata (assoc-ref metadata "docstring")) "")))
    (completion-item
     (completion-item-label item)
     #:kind (completion-item-kind item)
     #:detail (completion-item-detail item)
     #:filter-text (completion-item-filter-text item)
     #:sort-text (completion-item-sort-text item)
     #:insert-text (completion-item-insert-text item)
     #:documentation documentation
     #:start (completion-item-start item)
     #:end (completion-item-end item))))

(define (ares-completion-provider-definitions host)
  (list
   (list "ares"
         (lambda (context request)
           (ares-document-completions host context request))
         (lambda (context request item)
           (ares-resolve-document-completion host context request item)))))
