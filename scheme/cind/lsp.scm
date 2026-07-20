(define-module (cind lsp)
  #:use-module (ice-9 format)
  #:use-module (srfi srfi-1)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (define-lsp-provider!
            resolve-lsp-provider
            lsp-provider?
            lsp-provider-name
            lsp-provider-features
            lsp-provider-supports?
            bind-lsp-provider!
            lsp-session?
            lsp-session-id
            lsp-session-bound?
            lsp-buffer-edited!
            lsp-buffer-released!))

(define provider-tables (make-weak-key-hash-table))
(define session-bindings (make-weak-key-hash-table))

(define (host-provider-table host)
  (or (hashq-ref provider-tables host)
      (let ((table (make-hash-table)))
        (hashq-set! provider-tables host table)
        table)))

(define (non-empty-string? value)
  (and (string? value) (> (string-length value) 0)))

(define (string-vector value label)
  (unless (or (list? value) (vector? value))
    (error (string-append label " must be a list or vector") value))
  (let ((result (if (vector? value)
                    (list->vector (vector->list value))
                    (list->vector value))))
    (let loop ((index 0))
      (when (< index (vector-length result))
        (unless (string? (vector-ref result index))
          (error (string-append label " must contain strings") value))
        (loop (+ index 1))))
    result))

(define known-features '("completion" "diagnostics" "navigation"))

(define (validate-features features)
  (let ((result (string-vector features "LSP provider features")))
    (let loop ((index 0)
               (seen '()))
      (if (= index (vector-length result))
          result
          (let ((feature (vector-ref result index)))
            (unless (member feature known-features)
              (error "unknown LSP provider feature" feature))
            (when (member feature seen)
              (error "duplicate LSP provider feature" feature))
            (loop (+ index 1) (cons feature seen)))))))

(define (define-lsp-provider! host name language-id command arguments features)
  (unless (non-empty-string? name)
    (error "LSP provider name must be a non-empty string" name))
  (unless (non-empty-string? language-id)
    (error "LSP language ID must be a non-empty string" language-id))
  (unless (non-empty-string? command)
    (error "LSP command must be a non-empty string" command))
  (let ((definition
         (vector 'lsp-provider-definition
                 name language-id command
                 (string-vector arguments "LSP provider arguments")
                 (validate-features features))))
    (hash-set! (host-provider-table host) name definition)
    definition))

(define (provider-root host context)
  (let ((project (context-project context)))
    (if project
        (project-root host project)
        (let ((resource (buffer-resource host (context-buffer context))))
          (if resource
              (let ((parent (path-parent host resource)))
                (if (zero? (string-length parent)) "." parent))
              ".")))))

(define (resolve-lsp-provider host context name)
  (unless (string? name)
    (error "LSP provider name must be a string" name))
  (let ((definition (hash-ref (host-provider-table host) name)))
    (and definition
         (vector 'lsp-provider
                 (vector-ref definition 1)
                 (vector-ref definition 2)
                 (vector-ref definition 3)
                 (list->vector (vector->list (vector-ref definition 4)))
                 (provider-root host context)
                 (list->vector (vector->list (vector-ref definition 5)))))))

(define (lsp-provider? value)
  (and (vector? value)
       (= (vector-length value) 7)
       (eq? (vector-ref value 0) 'lsp-provider)))

(define (require-lsp-provider value)
  (unless (lsp-provider? value)
    (error "expected an LSP provider specification" value))
  value)

(define (lsp-provider-name value)
  (vector-ref (require-lsp-provider value) 1))

(define (lsp-provider-features value)
  (vector-ref (require-lsp-provider value) 6))

(define (lsp-provider-supports? value feature)
  (and (string? feature)
       (let ((features (lsp-provider-features value)))
         (let loop ((index 0))
           (and (< index (vector-length features))
                (or (string=? (vector-ref features index) feature)
                    (loop (+ index 1))))))))

(define (lsp-session? value)
  (and (vector? value)
       (= (vector-length value) 4)
       (eq? (vector-ref value 0) 'lsp-session)
       (integer? (vector-ref value 1))
       (positive? (vector-ref value 1))))

(define (lsp-session-id value)
  (unless (lsp-session? value)
    (error "expected an LSP session binding" value))
  (vector-ref value 1))

;; Binding entries are #(buffer provider-name provider session). Provider
;; definitions and bindings are application-local policy state; native session
;; objects remain opaque mechanism handles.
(define (host-session-bindings host)
  (or (hashq-ref session-bindings host) '()))

(define (binding-for host buffer name)
  (let loop ((bindings (host-session-bindings host)))
    (and (pair? bindings)
         (let ((binding (car bindings)))
           (if (and (equal? buffer (vector-ref binding 0))
                    (string=? name (vector-ref binding 1)))
               binding
               (loop (cdr bindings)))))))

(define (without-binding bindings buffer name)
  (cond ((null? bindings) '())
        ((and (equal? buffer (vector-ref (car bindings) 0))
              (string=? name (vector-ref (car bindings) 1)))
         (cdr bindings))
        (else
         (cons (car bindings) (without-binding (cdr bindings) buffer name)))))

(define (bind-lsp-provider! host context provider)
  (let* ((validated (require-lsp-provider provider))
         (buffer (context-buffer context))
         (name (lsp-provider-name validated))
         (existing (binding-for host buffer name))
         (id (ensure-lsp-session! host context validated)))
    (if (and existing
             (equal? validated (vector-ref existing 2))
             (= id (lsp-session-id (vector-ref existing 3))))
        (vector-ref existing 3)
        (let* ((session (vector 'lsp-session id name
                                (lsp-provider-features validated))))
          (when (lsp-provider-supports? validated "diagnostics")
            (attach-lsp-diagnostics! host id))
          (hashq-set! session-bindings host
                      (cons (vector buffer name validated session)
                            (without-binding (host-session-bindings host)
                                             buffer name)))
          session))))

(define (lsp-session-bound? host buffer session)
  (and (integer? session)
       (let loop ((bindings (host-session-bindings host)))
         (and (pair? bindings)
              (or (and (equal? buffer (vector-ref (car bindings) 0))
                       (= session
                          (lsp-session-id (vector-ref (car bindings) 3))))
                  (loop (cdr bindings)))))))

(define (lsp-buffer-edited! host buffer)
  (let loop ((bindings (host-session-bindings host))
             (synchronized '()))
    (when (pair? bindings)
      (let* ((binding (car bindings))
             (session (lsp-session-id (vector-ref binding 3))))
        (if (or (not (equal? buffer (vector-ref binding 0)))
                (memv session synchronized))
            (loop (cdr bindings) synchronized)
            (begin
              (catch #t
                (lambda () (synchronize-lsp-session! host buffer session))
                (lambda (key . arguments)
                  (set-message! host
                                (format #f "LSP synchronization failed: ~a"
                                        (if (pair? arguments)
                                            (car arguments)
                                            key)))))
              (loop (cdr bindings) (cons session synchronized))))))))

(define (lsp-buffer-released! host buffer)
  (let ((remaining
         (filter (lambda (binding)
                   (not (equal? buffer (vector-ref binding 0))))
                 (host-session-bindings host))))
    (if (null? remaining)
        (hashq-remove! session-bindings host)
        (hashq-set! session-bindings host remaining))))
