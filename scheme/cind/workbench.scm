(define-module (cind workbench)
  #:export (workbench-created!
            workbench-visit-buffer!
            workbench-expel-buffer!
            workbench-forget-buffer!
            workbench-released!
            workbench-mru
            replace-workbench-mru!
            workbench-name
            workbench-find-by-name
            workbench-rename!
            workbench-scope
            workbench-adopt-project!))

;; Entries are #(workbench name mru-list scope-list). Window layouts and entity
;; lifetimes are native data-plane state; descriptive and membership policy is
;; owned by Guile.
(define workbench-states (make-weak-key-hash-table))

(define (host-workbench-states host)
  (or (hashq-ref workbench-states host) '()))

(define (workbench-entry host workbench)
  (let loop ((entries (host-workbench-states host)))
    (and (pair? entries)
         (if (equal? workbench (vector-ref (car entries) 0))
             (car entries)
             (loop (cdr entries))))))

(define (require-workbench-entry host workbench)
  (or (workbench-entry host workbench)
      (error "unknown workbench policy state" workbench)))

(define (workbench-entry-by-name host name)
  (let loop ((entries (host-workbench-states host)))
    (and (pair? entries)
         (if (string=? name (vector-ref (car entries) 1))
             (car entries)
             (loop (cdr entries))))))

(define (without-workbench entries workbench)
  (cond ((null? entries) '())
        ((equal? workbench (vector-ref (car entries) 0))
         (cdr entries))
        (else
         (cons (car entries) (without-workbench (cdr entries) workbench)))))

(define (without-buffer buffers buffer)
  (cond ((null? buffers) '())
        ((equal? buffer (car buffers)) (without-buffer (cdr buffers) buffer))
        (else (cons (car buffers) (without-buffer (cdr buffers) buffer)))))

(define (unique-values buffers)
  (let loop ((remaining buffers)
             (result '()))
    (if (null? remaining)
        (reverse result)
        (loop (cdr remaining)
              (if (member (car remaining) result)
                  result
                  (cons (car remaining) result))))))

(define (workbench-created! host workbench name initial-buffer scope)
  (when (workbench-entry host workbench)
    (error "workbench policy state already exists" workbench))
  (unless (string? name)
    (error "workbench name must be a string" name))
  (when (workbench-entry-by-name host name)
    (error "workbench name is already in use" name))
  (unless (vector? scope)
    (error "workbench scope must be a vector" scope))
  (hashq-set! workbench-states host
              (cons (vector workbench
                            name
                            (if initial-buffer (list initial-buffer) '())
                            (unique-values (vector->list scope)))
                    (host-workbench-states host)))
  workbench)

(define (workbench-visit-buffer! host workbench buffer)
  (let ((entry (require-workbench-entry host workbench)))
    (vector-set! entry 2
                 (cons buffer (without-buffer (vector-ref entry 2) buffer))))
  buffer)

(define (workbench-expel-buffer! host workbench buffer)
  (let* ((entry (require-workbench-entry host workbench))
         (before (vector-ref entry 2))
         (after (without-buffer before buffer)))
    (vector-set! entry 2 after)
    (< (length after) (length before))))

(define (workbench-forget-buffer! host buffer)
  (for-each
   (lambda (entry)
     (vector-set! entry 2 (without-buffer (vector-ref entry 2) buffer)))
   (host-workbench-states host)))

(define (workbench-released! host workbench)
  (let ((remaining (without-workbench (host-workbench-states host) workbench)))
    (if (null? remaining)
        (hashq-remove! workbench-states host)
        (hashq-set! workbench-states host remaining))))

(define (workbench-mru host workbench)
  (list->vector (vector-ref (require-workbench-entry host workbench) 2)))

(define (replace-workbench-mru! host workbench buffers)
  (unless (vector? buffers)
    (error "workbench MRU must be a vector" buffers))
  (vector-set! (require-workbench-entry host workbench) 2
               (unique-values (vector->list buffers)))
  workbench)

(define (workbench-name host workbench)
  (vector-ref (require-workbench-entry host workbench) 1))

(define (workbench-find-by-name host name)
  (unless (string? name)
    (error "workbench name must be a string" name))
  (let ((entry (workbench-entry-by-name host name)))
    (and entry (vector-ref entry 0))))

(define (workbench-rename! host workbench name)
  (unless (string? name)
    (error "workbench name must be a string" name))
  (let ((entry (require-workbench-entry host workbench))
        (existing (workbench-entry-by-name host name)))
    (if (and existing (not (equal? workbench (vector-ref existing 0))))
        #f
        (begin
          (vector-set! entry 1 name)
          #t))))

(define (workbench-scope host workbench)
  (list->vector (vector-ref (require-workbench-entry host workbench) 3)))

(define (workbench-adopt-project! host workbench project)
  (let* ((entry (require-workbench-entry host workbench))
         (scope (vector-ref entry 3)))
    (if (member project scope)
        #f
        (begin
          (vector-set! entry 3 (append scope (list project)))
          #t))))
