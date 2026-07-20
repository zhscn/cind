(define-module (cind workbench)
  #:use-module ((cind host)
                #:select (open-buffer-ids
                          buffer-project-id
                          buffer-name
                          buffer-resource
                          buffer-modified?
                          project-root))
  #:export (workbench-created!
            active-workbench
            workbench-summaries
            workbench-activate!
            workbench-next
            workbench-active-window
            workbench-focus-window!
            workbench-visit-buffer!
            workbench-expel-buffer!
            workbench-forget-buffer!
            workbench-released!
            workbench-mru
            workbench-buffer-ids
            workbench-buffer-summaries
            replace-workbench-mru!
            workbench-name
            workbench-find-by-name
            workbench-rename!
            workbench-scope
            workbench-adopt-project!
            workbench-window-added!
            workbench-forget-window!
            workbench-window-state
            workbench-window-state-or-default
            workbench-set-window-role!
            workbench-set-window-pinned!
            workbench-set-window-created-by-policy!
            workbench-slot
            workbench-slots))

;; Entries are #(workbench name mru-list scope-list window-list active-window).
;; Window records are #(window role-or-#f pinned? created-by-policy?). Window
;; layouts and entity lifetimes are native data-plane state; selection,
;; descriptive, membership and display policy state is owned by Guile.
(define workbench-states (make-weak-key-hash-table))
(define active-workbenches (make-weak-key-hash-table))

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

(define (window-entry-in-workbench entry window)
  (let loop ((windows (vector-ref entry 4)))
    (and (pair? windows)
         (if (equal? window (vector-ref (car windows) 0))
             (car windows)
             (loop (cdr windows))))))

(define (window-entry-with-workbench host window)
  (let loop ((entries (host-workbench-states host)))
    (and (pair? entries)
         (let ((window-entry (window-entry-in-workbench (car entries) window)))
           (if window-entry
               (cons (car entries) window-entry)
               (loop (cdr entries)))))))

(define (require-window-entry-with-workbench host window)
  (or (window-entry-with-workbench host window)
      (error "unknown workbench window policy state" window)))

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

(define (workbench-created! host workbench name root-window initial-buffer scope)
  (when (workbench-entry host workbench)
    (error "workbench policy state already exists" workbench))
  (unless (string? name)
    (error "workbench name must be a string" name))
  (when (workbench-entry-by-name host name)
    (error "workbench name is already in use" name))
  (when (window-entry-with-workbench host root-window)
    (error "window policy state already exists" root-window))
  (unless (vector? scope)
    (error "workbench scope must be a vector" scope))
  (let ((entries (host-workbench-states host)))
    (hashq-set! workbench-states host
                (cons (vector workbench
                              name
                              (if initial-buffer (list initial-buffer) '())
                              (unique-values (vector->list scope))
                              (list (vector root-window #f #f #f))
                              root-window)
                      entries))
    (when (null? entries)
      (hashq-set! active-workbenches host workbench)))
  workbench)

(define (active-workbench host)
  (or (hashq-ref active-workbenches host)
      (error "workbench selection state is unavailable")))

(define (workbench-summaries host)
  (let ((selected (active-workbench host)))
    (list->vector
     (map (lambda (entry)
            (vector (vector-ref entry 0)
                    (vector-ref entry 1)
                    (equal? selected (vector-ref entry 0))))
          (reverse (host-workbench-states host))))))

(define (workbench-activate! host workbench)
  (require-workbench-entry host workbench)
  (hashq-set! active-workbenches host workbench)
  workbench)

(define (workbench-next host workbench delta)
  (unless (integer? delta)
    (error "workbench selection delta must be an integer" delta))
  (let* ((entries (reverse (host-workbench-states host)))
         (count (length entries))
         (current
          (let loop ((remaining entries) (index 0))
            (cond ((null? remaining)
                   (error "unknown workbench policy state" workbench))
                  ((equal? workbench (vector-ref (car remaining) 0)) index)
                  (else (loop (cdr remaining) (+ index 1)))))))
    (vector-ref (list-ref entries (modulo (+ current delta) count)) 0)))

(define (workbench-active-window host workbench)
  (vector-ref (require-workbench-entry host workbench) 5))

(define (workbench-focus-window! host workbench window)
  (let ((entry (require-workbench-entry host workbench)))
    (unless (window-entry-in-workbench entry window)
      (error "focused window does not belong to the workbench" window))
    (vector-set! entry 5 window))
  window)

(define (workbench-visit-buffer! host workbench buffer)
  (let ((entry (require-workbench-entry host workbench)))
    (vector-set! entry 2
                 (cons buffer (without-buffer (vector-ref entry 2) buffer))))
  buffer)

(define (workbench-expel-buffer! host workbench buffer)
  (buffer-name host buffer)
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
  (let* ((entries (host-workbench-states host))
         (entry (require-workbench-entry host workbench))
         (remaining (without-workbench entries workbench)))
    (when (and (pair? remaining)
               (equal? workbench (active-workbench host)))
      (error "cannot release the active workbench" workbench))
    (if (null? remaining)
        (begin
          (hashq-remove! workbench-states host)
          (hashq-remove! active-workbenches host))
        (hashq-set! workbench-states host remaining))
    entry))

(define (workbench-mru host workbench)
  (list->vector (vector-ref (require-workbench-entry host workbench) 2)))

(define (select-values predicate values)
  (let loop ((remaining values)
             (result '()))
    (if (null? remaining)
        (reverse result)
        (loop (cdr remaining)
              (if (predicate (car remaining))
                  (cons (car remaining) result)
                  result)))))

(define (workbench-buffer-ids host workbench widen?)
  (require-workbench-entry host workbench)
  (unless (boolean? widen?)
    (error "workbench widen flag must be a boolean" widen?))
  (let* ((open (vector->list (open-buffer-ids host)))
         (scope (vector->list (workbench-scope host workbench))))
    (list->vector
     (if widen?
         open
         (unique-values
          (append
           (select-values
            (lambda (buffer) (member buffer open))
            (vector->list (workbench-mru host workbench)))
           (select-values
            (lambda (buffer)
              (let ((project (buffer-project-id host buffer)))
                (and project (member project scope))))
            open)))))))

(define (workbench-buffer-summaries host workbench widen?)
  (let* ((scope (vector->list (workbench-scope host workbench)))
         (buffers (workbench-buffer-ids host workbench widen?)))
    (let loop ((index 0)
               (summaries '()))
      (if (= index (vector-length buffers))
          (list->vector (reverse summaries))
          (let* ((buffer (vector-ref buffers index))
                 (project (buffer-project-id host buffer)))
            (loop (+ index 1)
                  (cons (vector (buffer-name host buffer)
                                (buffer-resource host buffer)
                                (buffer-modified? host buffer)
                                (not (and project (member project scope))))
                        summaries)))))))

(define (replace-workbench-mru! host workbench buffers)
  (unless (vector? buffers)
    (error "workbench MRU must be a vector" buffers))
  (for-each (lambda (buffer) (buffer-name host buffer))
            (vector->list buffers))
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
  (project-root host project)
  (let* ((entry (require-workbench-entry host workbench))
         (scope (vector-ref entry 3)))
    (if (member project scope)
        #f
        (begin
          (vector-set! entry 3 (append scope (list project)))
          #t))))

(define (workbench-window-added! host workbench window)
  (let ((entry (require-workbench-entry host workbench)))
    (when (window-entry-with-workbench host window)
      (error "window policy state already exists" window))
    (vector-set! entry 4
                 (append (vector-ref entry 4)
                         (list (vector window #f #f #f)))))
  window)

(define (without-window windows window)
  (cond ((null? windows) '())
        ((equal? window (vector-ref (car windows) 0)) (cdr windows))
        (else (cons (car windows) (without-window (cdr windows) window)))))

(define (workbench-forget-window! host window)
  (let ((removed? #f))
    (for-each
     (lambda (entry)
       (let* ((before (vector-ref entry 4))
              (after (without-window before window)))
         (unless (= (length before) (length after))
           (when (equal? window (vector-ref entry 5))
             (error "cannot forget the active workbench window" window))
           (set! removed? #t)
           (vector-set! entry 4 after))))
     (host-workbench-states host))
    removed?))

(define (workbench-window-state host window)
  (let* ((pair (require-window-entry-with-workbench host window))
         (entry (car pair))
         (state (cdr pair)))
    (vector (vector-ref entry 0)
            (vector-ref state 1)
            (vector-ref state 2)
            (vector-ref state 3))))

(define (workbench-window-state-or-default host window)
  (let ((pair (window-entry-with-workbench host window)))
    (if pair
        (let ((entry (car pair))
              (state (cdr pair)))
          (vector (vector-ref entry 0)
                  (vector-ref state 1)
                  (vector-ref state 2)
                  (vector-ref state 3)))
        (vector #f #f #f #f))))

(define (workbench-set-window-role! host window role)
  (unless (or (not role) (and (string? role) (> (string-length role) 0)))
    (error "window role must be a non-empty string or #f" role))
  (let* ((pair (require-window-entry-with-workbench host window))
         (entry (car pair))
         (state (cdr pair)))
    (when role
      (for-each
       (lambda (candidate)
         (when (and (not (eq? candidate state))
                    (equal? role (vector-ref candidate 1)))
           (vector-set! candidate 1 #f)))
       (vector-ref entry 4)))
    (vector-set! state 1 role))
  window)

(define (workbench-set-window-pinned! host window pinned?)
  (unless (boolean? pinned?)
    (error "window pinned state must be boolean" pinned?))
  (vector-set! (cdr (require-window-entry-with-workbench host window)) 2 pinned?)
  window)

(define (workbench-set-window-created-by-policy! host window created?)
  (unless (boolean? created?)
    (error "window policy provenance must be boolean" created?))
  (vector-set! (cdr (require-window-entry-with-workbench host window)) 3 created?)
  window)

(define (workbench-slot host workbench role)
  (unless (and (string? role) (> (string-length role) 0))
    (error "workbench slot role must be a non-empty string" role))
  (let loop ((windows (vector-ref (require-workbench-entry host workbench) 4)))
    (and (pair? windows)
         (if (equal? role (vector-ref (car windows) 1))
             (vector-ref (car windows) 0)
             (loop (cdr windows))))))

(define (workbench-slots host workbench)
  (let loop ((windows (vector-ref (require-workbench-entry host workbench) 4))
             (slots '()))
    (if (null? windows)
        (list->vector (reverse slots))
        (let ((state (car windows)))
          (loop (cdr windows)
                (if (vector-ref state 1)
                    (cons (vector (vector-ref state 1) (vector-ref state 0)) slots)
                    slots))))))
