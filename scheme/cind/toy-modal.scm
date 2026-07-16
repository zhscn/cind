(define-module (cind toy-modal)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (install-toy-modal-input-state!
            toy-modal-command-definitions
            install-toy-modal-keymap!))

(define toy-normal-keymap 'toy-modal.normal)

(define (install-toy-modal-input-state! host)
  (define-keymap! host toy-normal-keymap #f)
  (define-input-state! host 'toy-normal
    (vector toy-normal-keymap) 'ignore 'block "N" #f)
  1)

(define (set-input-state-command host state)
  (lambda (context invocation)
    (set-base-input-state! host (context-view context) state)
    (command-completed)))

(define (toy-modal-command-definitions host)
  (list (list "toy-modal.enter-normal"
              (set-input-state-command host 'toy-normal)
              #f)
        (list "toy-modal.enter-emacs"
              (set-input-state-command host 'emacs)
              #f)))

(define toy-normal-bindings
  '(("h" . "cursor.backward-character")
    ("j" . "cursor.next-line")
    ("k" . "cursor.previous-line")
    ("l" . "cursor.forward-character")
    ("0" . "cursor.line-start")
    ("$" . "cursor.line-end")
    ("x" . "edit.delete-forward")
    ("i" . "toy-modal.enter-emacs")))

(define (install-toy-modal-keymap! host)
  (define-keymap! host toy-normal-keymap #f)
  (let loop ((bindings toy-normal-bindings)
             (count 0))
    (if (null? bindings)
        count
        (let ((binding (car bindings)))
          (loop (cdr bindings)
                (if (bind-key-if-command! host toy-normal-keymap
                                          (car binding) (cdr binding))
                    (+ count 1)
                    count))))))
