(define-module (cind minibuffer)
  #:use-module (ice-9 optargs)
  #:use-module (srfi srfi-13)
  #:use-module (cind command)
  #:export (read-from-minibuffer
            completing-read
            rank-completion-candidates
            rank-provider-result))

(define (candidate-filter-text candidate)
  (let ((filter-text (vector-ref candidate 3)))
    (if (zero? (string-length filter-text))
        (vector-ref candidate 1)
        filter-text)))

(define (candidate-score candidate terms)
  (let ((haystack (string-downcase (candidate-filter-text candidate))))
    (let loop ((remaining terms)
               (score (string-length haystack)))
      (if (null? remaining)
          score
          (let ((position (string-contains haystack (car remaining))))
            (and position (loop (cdr remaining) (+ score position))))))))

(define (rank-completion-candidates candidates query)
  (unless (vector? candidates)
    (error "completion candidates must be a vector" candidates))
  (let ((terms (string-tokenize (string-downcase query))))
    (let loop ((index 0)
               (ranked '()))
      (if (= index (vector-length candidates))
          (list->vector
           (map (lambda (entry) (vector-ref entry 2))
                (sort ranked
                      (lambda (left right)
                        (or (< (vector-ref left 0) (vector-ref right 0))
                            (and (= (vector-ref left 0) (vector-ref right 0))
                                 (< (vector-ref left 1) (vector-ref right 1))))))))
          (let* ((candidate (vector-ref candidates index))
                 (score (candidate-score candidate terms)))
            (loop (+ index 1)
                  (if score
                      (cons (vector score index candidate) ranked)
                      ranked)))))))

(define (rank-provider-result result query)
  (if (and (vector? result)
           (= (vector-length result) 3)
           (eq? (vector-ref result 0) 'async-provider))
      (interaction-provider-task
       (vector-ref result 1)
       (let ((transform (vector-ref result 2)))
         (lambda (async-result)
           (rank-completion-candidates (transform async-result) query))))
      (rank-completion-candidates result query)))

(define (require-string procedure field value)
  (unless (string? value)
    (error (string-append procedure ": " field " must be a string") value)))

(define (require-symbol procedure field value)
  (unless (symbol? value)
    (error (string-append procedure ": " field " must be a symbol") value)))

(define (require-arguments procedure arguments)
  (unless (list? arguments)
    (error (string-append procedure ": arguments must be a proper list") arguments)))

(define* (read-from-minibuffer prompt accept-command
                               #:key
                               (initial-input "")
                               (history "")
                               (keymap 'interaction.text)
                               (input-state 'emacs)
                               (arguments '()))
  (require-string "read-from-minibuffer" "prompt" prompt)
  (require-string "read-from-minibuffer" "accept-command" accept-command)
  (require-string "read-from-minibuffer" "initial-input" initial-input)
  (require-string "read-from-minibuffer" "history" history)
  (require-symbol "read-from-minibuffer" "keymap" keymap)
  (require-symbol "read-from-minibuffer" "input-state" input-state)
  (require-arguments "read-from-minibuffer" arguments)
  (apply interaction 'text keymap input-state prompt initial-input history "" #t
         accept-command arguments))

(define* (completing-read prompt provider accept-command
                          #:key
                          (initial-input "")
                          (history "")
                          (allow-custom-input? #f)
                          (keymap 'interaction.picker)
                          (input-state 'emacs)
                          (arguments '()))
  (require-string "completing-read" "prompt" prompt)
  (require-string "completing-read" "provider" provider)
  (require-string "completing-read" "accept-command" accept-command)
  (require-string "completing-read" "initial-input" initial-input)
  (require-string "completing-read" "history" history)
  (require-symbol "completing-read" "keymap" keymap)
  (require-symbol "completing-read" "input-state" input-state)
  (require-arguments "completing-read" arguments)
  (apply interaction 'picker keymap input-state prompt initial-input history provider
         (and allow-custom-input? #t) accept-command arguments))
