(define-module (cind emacs)
  #:use-module (cind host)
  #:export (install-emacs-input-state!))

(define (install-emacs-input-state! host)
  (define-input-state! host 'emacs '#() 'accept 'beam "" #f)
  (%set-interaction-class-state! host 'editing 'emacs)
  (%set-interaction-class-state! host 'interface 'emacs)
  1)
