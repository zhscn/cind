(define-module (cind lsp)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (define-lsp-provider!
            resolve-lsp-provider
            lsp-provider?
            lsp-provider-name
            lsp-provider-features
            lsp-provider-supports?))

(define provider-tables (make-weak-key-hash-table))

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
