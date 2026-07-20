(define-module (cind async)
  #:use-module (ice-9 optargs)
  #:use-module (cind host)
  #:use-module (cind lsp)
  #:export (async-file-read
            async-file-write
            async-directory-list
            async-directory-list-many
            async-clang-format-style
            async-project-discovery
            async-rg-result-parse
            async-process
            async-lsp-navigation
            start-async-task!
            cancel-async-task!
            async-task-summaries))

(define (async-file-read path)
  (unless (string? path)
    (error "async file-read path must be a string" path))
  (vector 'file-read path))

(define (async-file-write path contents)
  (unless (string? path)
    (error "async file-write path must be a string" path))
  (unless (string? contents)
    (error "async file-write contents must be a string" contents))
  (vector 'file-write path contents))

(define* (async-directory-list path #:optional (maximum-entries 4096))
  (unless (string? path)
    (error "async directory-list path must be a string" path))
  (unless (and (integer? maximum-entries) (>= maximum-entries 0))
    (error "async directory-list limit must be a non-negative integer"
           maximum-entries))
  (vector 'directory-list path maximum-entries))

(define* (async-directory-list-many paths #:optional (maximum-entries 4096))
  (unless (or (list? paths) (vector? paths))
    (error "async directory-list-many paths must be a list or vector" paths))
  (let ((path-vector (if (vector? paths) paths (list->vector paths))))
    (when (zero? (vector-length path-vector))
      (error "async directory-list-many paths must not be empty" paths))
    (let loop ((index 0))
      (when (< index (vector-length path-vector))
        (unless (string? (vector-ref path-vector index))
          (error "async directory-list-many paths must contain strings" paths))
        (loop (+ index 1))))
    (unless (and (integer? maximum-entries) (>= maximum-entries 0))
      (error "async directory-list-many limit must be a non-negative integer"
             maximum-entries))
    (vector 'directory-list-many path-vector maximum-entries)))

(define (async-clang-format-style path fallback-preset fallback-origin)
  (unless (string? path)
    (error "clang-format style path must be a string" path))
  (unless (string? fallback-preset)
    (error "clang-format fallback preset must be a string" fallback-preset))
  (unless (string? fallback-origin)
    (error "clang-format fallback origin must be a string" fallback-origin))
  (vector 'clang-format-style path fallback-preset fallback-origin))

(define (async-project-discovery path providers)
  (unless (string? path)
    (error "project discovery path must be a string" path))
  (unless (vector? providers)
    (error "project discovery providers must be a vector" providers))
  (vector 'project-discovery path providers))

(define (async-rg-result-parse project-root output)
  (unless (string? project-root)
    (error "rg result project root must be a string" project-root))
  (unless (string? output)
    (error "rg result output must be a string" output))
  (vector 'rg-result-parse project-root output))

(define* (async-process file arguments #:optional (working-directory ""))
  (unless (string? file)
    (error "async process executable must be a string" file))
  (unless (or (list? arguments) (vector? arguments))
    (error "async process arguments must be a list or vector" arguments))
  (unless (string? working-directory)
    (error "async process working directory must be a string" working-directory))
  (vector 'process file arguments working-directory))

(define (async-lsp-navigation window buffer view kind session)
  (unless (symbol? kind)
    (error "LSP navigation kind must be a symbol" kind))
  (unless (lsp-session? session)
    (error "LSP navigation requires a bound session" session))
  (vector 'lsp-navigation window buffer view kind (lsp-session-id session)))

(define* (start-async-task! host request completed
                            #:key (failed #f) (cancelled #f))
  (unless (procedure? completed)
    (error "async completion callback must be a procedure" completed))
  (unless (or (not failed) (procedure? failed))
    (error "async failure callback must be a procedure or #f" failed))
  (unless (or (not cancelled) (procedure? cancelled))
    (error "async cancellation callback must be a procedure or #f" cancelled))
  (%start-async-task! host request completed failed cancelled))

(define (cancel-async-task! host task)
  (%cancel-async-task! host task))

(define (async-task-summaries host)
  (%async-task-summaries host))
