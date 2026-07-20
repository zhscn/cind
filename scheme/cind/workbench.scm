(define-module (cind workbench)
  #:export (workbench-created!
            workbench-visit-buffer!
            workbench-expel-buffer!
            workbench-forget-buffer!
            workbench-released!
            workbench-mru
            replace-workbench-mru!))

;; Entries are #(workbench mru-list). Window layouts and entity lifetimes are
;; native data-plane state; recency is application policy owned by Guile.
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

(define (unique-buffers buffers)
  (let loop ((remaining buffers)
             (result '()))
    (if (null? remaining)
        (reverse result)
        (loop (cdr remaining)
              (if (member (car remaining) result)
                  result
                  (cons (car remaining) result))))))

(define (workbench-created! host workbench initial-buffer)
  (when (workbench-entry host workbench)
    (error "workbench policy state already exists" workbench))
  (hashq-set! workbench-states host
              (cons (vector workbench
                            (if initial-buffer (list initial-buffer) '()))
                    (host-workbench-states host)))
  workbench)

(define (workbench-visit-buffer! host workbench buffer)
  (let ((entry (require-workbench-entry host workbench)))
    (vector-set! entry 1
                 (cons buffer (without-buffer (vector-ref entry 1) buffer))))
  buffer)

(define (workbench-expel-buffer! host workbench buffer)
  (let* ((entry (require-workbench-entry host workbench))
         (before (vector-ref entry 1))
         (after (without-buffer before buffer)))
    (vector-set! entry 1 after)
    (< (length after) (length before))))

(define (workbench-forget-buffer! host buffer)
  (for-each
   (lambda (entry)
     (vector-set! entry 1 (without-buffer (vector-ref entry 1) buffer)))
   (host-workbench-states host)))

(define (workbench-released! host workbench)
  (let ((remaining (without-workbench (host-workbench-states host) workbench)))
    (if (null? remaining)
        (hashq-remove! workbench-states host)
        (hashq-set! workbench-states host remaining))))

(define (workbench-mru host workbench)
  (list->vector (vector-ref (require-workbench-entry host workbench) 1)))

(define (replace-workbench-mru! host workbench buffers)
  (unless (vector? buffers)
    (error "workbench MRU must be a vector" buffers))
  (vector-set! (require-workbench-entry host workbench) 1
               (unique-buffers (vector->list buffers)))
  workbench)
