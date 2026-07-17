(define-module (cind minibuffer)
  #:use-module (ice-9 optargs)
  #:use-module (cind command)
  #:export (read-from-minibuffer
            completing-read))

(define (require-string procedure field value)
  (unless (string? value)
    (error (string-append procedure ": " field " must be a string") value)))

(define (require-arguments procedure arguments)
  (unless (list? arguments)
    (error (string-append procedure ": arguments must be a proper list") arguments)))

(define* (read-from-minibuffer prompt accept-command
                               #:key
                               (initial-input "")
                               (history "")
                               (arguments '()))
  (require-string "read-from-minibuffer" "prompt" prompt)
  (require-string "read-from-minibuffer" "accept-command" accept-command)
  (require-string "read-from-minibuffer" "initial-input" initial-input)
  (require-string "read-from-minibuffer" "history" history)
  (require-arguments "read-from-minibuffer" arguments)
  (apply interaction 'text prompt initial-input history "" #t accept-command arguments))

(define* (completing-read prompt provider accept-command
                          #:key
                          (initial-input "")
                          (history "")
                          (allow-custom-input? #f)
                          (arguments '()))
  (require-string "completing-read" "prompt" prompt)
  (require-string "completing-read" "provider" provider)
  (require-string "completing-read" "accept-command" accept-command)
  (require-string "completing-read" "initial-input" initial-input)
  (require-string "completing-read" "history" history)
  (require-arguments "completing-read" arguments)
  (apply interaction 'picker prompt initial-input history provider
         (and allow-custom-input? #t) accept-command arguments))
