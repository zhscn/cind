(define-module (cind async)
  #:use-module (ice-9 optargs)
  #:use-module (cind host)
  #:export (async-file-read
            async-file-write
            async-directory-list
            async-process
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

(define* (async-process file arguments #:optional (working-directory ""))
  (unless (string? file)
    (error "async process executable must be a string" file))
  (unless (or (list? arguments) (vector? arguments))
    (error "async process arguments must be a list or vector" arguments))
  (unless (string? working-directory)
    (error "async process working directory must be a string" working-directory))
  (vector 'process file arguments working-directory))

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
