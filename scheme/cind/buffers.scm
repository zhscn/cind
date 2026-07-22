(define-module (cind buffers)
  #:use-module (ice-9 format)
  #:use-module (cind host)
  #:use-module (cind state)
  #:export (configure-buffer-naming-policy!
            buffer-name
            buffer-id-by-name
            buffer-names
            rename-buffer!
            buffer-project-id
            set-buffer-project!
            project-has-buffers?
            buffer-kind
            buffer-resource
            buffer-id-by-resource
            set-buffer-resource!
            register-buffer-name!
            forget-buffer-name!))

;; Buffer names are policy, not mechanism: which characters mark a buffer as
;; internal, what an unnamed buffer of each kind is called, and how a collision
;; is disambiguated are all conventions a configuration may replace. The host
;; creates the document; this module decides what it is called and keeps the
;; only copy of the answer (design/09-guile-first.md section 3.4).
;;
;; Entries are #(buffer name project kind resource). Entity IDs marshal as fresh two-element vectors
;; on every crossing, so they compare with equal? and cannot key a hashq table;
;; the surrounding modules use association lists for the same reason. Buffer
;; counts are in the tens, so the linear scan is not worth trading for a
;; equal?-hashed table that would have to be kept in sync in two directions.
(define-state-slot! 'buffer-identities (lambda () '()))

;; Chosen when the requested name is empty and nothing better can be derived.
(define %anonymous-buffer-name "*buffer*")

(define %default-kind-names
  '((file . "untitled")
    (scratch . "*scratch*")
    (generated . "*generated*")
    (process . "*process*")
    ;; Leading space keeps the minibuffer out of name-completion listings,
    ;; following the Emacs convention that the rest of the naming policy uses.
    (minibuffer . " *minibuffer*")))

;; A naming policy receives the requested name (possibly empty), the buffer
;; kind and the resource (or #f) and returns the name to disambiguate. The
;; default derives a file buffer's name from its path's last component.
(define-policy-slot! 'buffer-naming
  (lambda (host requested kind resource)
    (cond ((and (string? requested) (not (zero? (string-length requested))))
           requested)
          ((and resource (not (zero? (string-length resource))))
           (let ((filename (path-filename host resource)))
             (if (zero? (string-length filename))
                 %anonymous-buffer-name
                 filename)))
          (else
           (let ((entry (assq kind %default-kind-names)))
             (if entry (cdr entry) %anonymous-buffer-name))))))

(define (configure-buffer-naming-policy! host procedure)
  (unless (procedure? procedure)
    (error "buffer naming policy must be a procedure" procedure))
  (policy-set! host 'buffer-naming procedure))

(define (identities host)
  (state-ref host 'buffer-identities))

(define (without-buffer entries buffer)
  (cond ((null? entries) '())
        ((equal? buffer (vector-ref (car entries) 0)) (cdr entries))
        (else (cons (car entries) (without-buffer (cdr entries) buffer)))))

(define (identity-of host buffer)
  (let loop ((entries (identities host)))
    (cond ((null? entries) #f)
          ((equal? buffer (vector-ref (car entries) 0)) (car entries))
          (else (loop (cdr entries))))))

(define (require-identity host buffer)
  (or (identity-of host buffer)
      (error "buffer is not registered" buffer)))

(define (registered-name host buffer)
  (let ((entry (identity-of host buffer)))
    (and entry (vector-ref entry 1))))

(define (buffer-name host buffer)
  (or (registered-name host buffer)
      (error "buffer has no name" buffer)))

(define (buffer-project-id host buffer)
  (vector-ref (require-identity host buffer) 2))

(define (set-buffer-project! host buffer project)
  (let ((entry (require-identity host buffer)))
    (replace-entry! host buffer (vector buffer (vector-ref entry 1) project
                                        (vector-ref entry 3) (vector-ref entry 4))))
  project)

(define (buffer-kind host buffer)
  (vector-ref (require-identity host buffer) 3))

(define (buffer-resource host buffer)
  (vector-ref (require-identity host buffer) 4))

(define (buffer-id-by-resource host resource)
  (let loop ((entries (identities host)))
    (cond ((null? entries) #f)
          ((equal? resource (vector-ref (car entries) 4))
           (vector-ref (car entries) 0))
          (else (loop (cdr entries))))))

;; A resource is visited by at most one buffer: the association is the reason
;; opening the same file twice reuses a buffer, so the uniqueness check belongs
;; wherever the association lives.
(define (require-unclaimed-resource host resource self)
  (let ((holder (and resource (buffer-id-by-resource host resource))))
    (when (and holder (not (equal? holder self)))
      (error "another buffer already visits this resource" resource))))

(define (set-buffer-resource! host buffer resource kind)
  (unless (or (not resource) (string? resource))
    (error "buffer resource must be a string or #f" resource))
  (unless (symbol? kind)
    (error "buffer kind must be a symbol" kind))
  (let ((entry (require-identity host buffer)))
    (require-unclaimed-resource host resource buffer)
    (replace-entry! host buffer (vector buffer (vector-ref entry 1) (vector-ref entry 2)
                                        kind resource)))
  resource)

;; Answers what the native project registry used to answer by scanning buffers:
;; the association is here now, so the question is answered here.
(define (project-has-buffers? host project)
  (let loop ((entries (identities host)))
    (cond ((null? entries) #f)
          ((equal? project (vector-ref (car entries) 2)) #t)
          (else (loop (cdr entries))))))

(define (buffer-id-by-name host name)
  (let loop ((entries (identities host)))
    (cond ((null? entries) #f)
          ((string=? name (vector-ref (car entries) 1))
           (vector-ref (car entries) 0))
          (else (loop (cdr entries))))))

(define (buffer-names host)
  (map (lambda (entry) (vector-ref entry 1)) (identities host)))

;; Free names are the common case, so check the requested name before building
;; any candidate string.
(define (unique-name host requested self)
  (define (available? candidate)
    (let ((holder (buffer-id-by-name host candidate)))
      (or (not holder) (and self (equal? holder self)))))
  (let ((base (if (zero? (string-length requested))
                  %anonymous-buffer-name
                  requested)))
    (if (available? base)
        base
        (let loop ((suffix 2))
          (let ((candidate (format #f "~a<~a>" base suffix)))
            (if (available? candidate)
                candidate
                (loop (+ suffix 1))))))))

(define (replace-entry! host buffer entry)
  (state-set! host 'buffer-identities
              (cons entry (without-buffer (identities host) buffer)))
  entry)

(define (store-name! host buffer name)
  (let ((entry (identity-of host buffer)))
    (replace-entry! host buffer
                    (vector buffer name
                            (and entry (vector-ref entry 2))
                            (and entry (vector-ref entry 3))
                            (and entry (vector-ref entry 4)))))
  name)

;; Called from the creation hook. The requested name is what the caller asked
;; for; the returned name is what the buffer is actually called once the policy
;; and collision handling have run. Re-announcing a buffer that already has a
;; name keeps it -- the host reuses a generated buffer rather than making a new
;; one, and naming it again would disambiguate it against itself.
(define (register-buffer-name! host buffer requested kind resource)
  (unless (string? requested)
    (error "requested buffer name must be a string" requested))
  (unless (symbol? kind)
    (error "buffer kind must be a symbol" kind))
  (or (registered-name host buffer)
      (let ((chosen ((policy-ref host 'buffer-naming) host requested kind resource)))
        (unless (string? chosen)
          (error "buffer naming policy must return a string" chosen))
        (require-unclaimed-resource host resource buffer)
        (replace-entry! host buffer
                        (vector buffer (unique-name host chosen #f) #f kind resource))
        (buffer-name host buffer))))

(define (rename-buffer! host buffer requested)
  (unless (string? requested)
    (error "buffer name must be a string" requested))
  ;; Resolve the entry first so renaming an unknown buffer raises rather than
  ;; silently registering one.
  (buffer-name host buffer)
  (store-name! host buffer (unique-name host requested buffer)))

(define (forget-buffer-name! host buffer)
  (state-set! host 'buffer-identities (without-buffer (identities host) buffer)))
