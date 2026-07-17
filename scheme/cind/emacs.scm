(define-module (cind emacs)
  #:use-module (cind host)
  #:export (install-emacs-input-state!))

(define (install-emacs-input-state! host)
  (define-input-state! host 'emacs '#() 'accept 'beam "" #f)
  (define-input-strategy! host 'emacs 'emacs 'emacs 'collapse)
  (set-default-input-strategy! host 'emacs)
  1)
