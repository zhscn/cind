(define-module (cind ares)
  #:use-module (ares completion)
  #:use-module (ares repl)
  #:use-module (cind development)
  #:export (ares-provider-definitions))

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
