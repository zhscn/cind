(define-module (cind extension)
  #:export (load-extension-file))

(define (load-extension-file path host)
  (let ((module (make-fresh-user-module)))
    ;; Buffer identity moved out of (cind host) into (cind buffers); user code
    ;; documented against buffer-name, rename-buffer! and set-buffer-project!
    ;; must keep resolving them without a new import.
    (module-use! module (resolve-interface '(cind buffers)))
    (module-use! module (resolve-interface '(cind application)))
    (module-use! module (resolve-interface '(cind command)))
    (module-use! module (resolve-interface '(cind completion)))
    (module-use! module (resolve-interface '(cind async)))
    (module-use! module (resolve-interface '(cind input)))
    (module-use! module (resolve-interface '(cind lifecycle)))
    (module-use! module (resolve-interface '(cind minibuffer)))
    (module-use! module (resolve-interface '(cind pointer)))
    (module-use! module (resolve-interface '(cind host)))
    (module-define! module 'host host)
    (save-module-excursion
     (lambda ()
       (set-current-module module)
       (primitive-load path)))
    module))
