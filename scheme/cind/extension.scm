(define-module (cind extension)
  #:export (user-module-interfaces
            make-user-module
            load-extension-file))

;; The modules a user init file or REPL evaluation sees without importing
;; anything. This list is the extension ABI's surface: a name documented in
;; docs/scripting.md resolves only if the module exporting it is here. It lived
;; in two copies -- one for init files, one for the evaluation module -- and the
;; copies drifted the first time buffer identity moved out of (cind host), which
;; silently unbound buffer-name in init files. One list, both callers.
(define user-module-interfaces
  '((cind application)
    (cind async)
    (cind buffers)
    (cind command)
    (cind completion)
    (cind input)
    (cind lifecycle)
    (cind minibuffer)
    (cind pointer)
    (cind workbench)
    (cind host)))

(define (make-user-module host)
  (let ((module (make-fresh-user-module)))
    (for-each (lambda (name)
                (module-use! module (resolve-interface name)))
              user-module-interfaces)
    (module-define! module 'host host)
    module))

(define (load-extension-file path host)
  (let ((module (make-user-module host)))
    (save-module-excursion
     (lambda ()
       (set-current-module module)
       (primitive-load path)))
    module))
