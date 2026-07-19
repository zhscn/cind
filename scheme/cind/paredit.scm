(define-module (cind paredit)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (paredit-command-definitions
            install-paredit-mode!
            install-paredit-keymap!
            install-paredit-documentation!))

(define paredit-mode-map 'paredit-mode-map)

(define (minor-mode-enabled? host buffer mode)
  (let ((modes (vector-ref (buffer-mode-summary host buffer) 1)))
    (let loop ((index 0))
      (and (< index (vector-length modes))
           (or (eq? (vector-ref modes index) mode)
               (loop (+ index 1)))))))

(define (paredit-transform-command host operation)
  (lambda (context invocation)
    (structural-edit! host (context-view context) operation)
    (command-completed)))

(define (paredit-toggle-command host)
  (lambda (context invocation)
    (let* ((buffer (context-buffer context))
           (enabled? (minor-mode-enabled? host buffer 'paredit-mode)))
      (set-buffer-minor-mode! host buffer 'paredit-mode (not enabled?))
      (set-message! host (if enabled? "Paredit disabled" "Paredit enabled"))
      (command-completed))))

(define (paredit-command-definitions host)
  (list (list "paredit.mode" (paredit-toggle-command host) #f)
        (list "paredit.splice"
              (paredit-transform-command host 'splice) #f)
        (list "paredit.forward-slurp"
              (paredit-transform-command host 'forward-slurp) #f)
        (list "paredit.forward-barf"
              (paredit-transform-command host 'forward-barf) #f)
        (list "paredit.backward-slurp"
              (paredit-transform-command host 'backward-slurp) #f)
        (list "paredit.backward-barf"
              (paredit-transform-command host 'backward-barf) #f)))

(define paredit-command-documentation
  '(("paredit.mode" . "Toggle paredit-mode in the current buffer.")
    ("paredit.splice" . "Remove the delimiters of the enclosing list.")
    ("paredit.forward-slurp" . "Move the closing delimiter forward over the next datum.")
    ("paredit.forward-barf" . "Move the closing delimiter backward before the last datum.")
    ("paredit.backward-slurp" . "Move the opening delimiter backward over the previous datum.")
    ("paredit.backward-barf" . "Move the opening delimiter forward after the first datum.")))

(define (install-paredit-documentation! host)
  (for-each (lambda (entry)
              (set-command-documentation! host (car entry) (cdr entry)))
            paredit-command-documentation)
  (length paredit-command-documentation))

(define (install-paredit-mode! host)
  (define-keymap! host paredit-mode-map #f)
  (%define-mode! host 'paredit-mode 'minor #f #f paredit-mode-map
                 #f #f '() #f)
  (set-mode-completion-auto! host 'paredit-mode 'inherit)
  1)

(define paredit-bindings
  '(("M-s" . "paredit.splice")
    ("C-)" . "paredit.forward-slurp")
    ("C-}" . "paredit.forward-barf")
    ("C-(" . "paredit.backward-slurp")
    ("C-{" . "paredit.backward-barf")))

(define (install-paredit-keymap! host)
  (define-keymap! host paredit-mode-map #f)
  (let loop ((bindings paredit-bindings)
             (count 0))
    (if (null? bindings)
        count
        (let ((binding (car bindings)))
          (loop (cdr bindings)
                (if (bind-key-if-command! host paredit-mode-map
                                          (car binding) (cdr binding))
                    (+ count 1)
                    count))))))
