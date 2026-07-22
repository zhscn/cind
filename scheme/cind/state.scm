;;; Application state root.
;;;
;;; Editor state belongs to Guile (design/09-guile-first.md). Before this
;;; module every subsystem invented its own `(define x-states
;;; (make-weak-key-hash-table))` keyed by the host capability, so an
;;; application's state was scattered across a hundred private tables with no
;;; way to enumerate, inspect or release it as one value.
;;;
;;; A state root is one table per application holding named slots. Slots are
;;; declared up front, which is what makes the root enumerable: inspection and
;;; release walk the declarations instead of guessing which module owns what.
;;;
;;; Two slot kinds, because the lifecycles differ:
;;;
;;;   state  - mutable runtime data. Materialized from its initializer on first
;;;            reference and retained until the application is released.
;;;   policy - a configured procedure. Absent means "use the declared default";
;;;            a policy without a default is required and raises when unset.
;;;
;;; The root table is weak on the host, so a released application's state stays
;;; collectible even if `release-application-state!` never runs.

(define-module (cind state)
  #:export (define-state-slot!
            define-policy-slot!
            state-ref
            state-set!
            state-update!
            state-clear!
            policy-ref
            policy-set!
            policy-configured?
            state-slot-names
            application-state-snapshot
            release-application-state!))

;; slot name -> #(kind initializer-or-default)
(define slot-declarations (make-hash-table))

;; host -> (slot name -> value)
(define state-roots (make-weak-key-hash-table))

(define (declaration name)
  (or (hashq-ref slot-declarations name)
      (error "undeclared state slot" name)))

(define (declaration-kind name) (vector-ref (declaration name) 0))

(define (expect-kind name kind)
  (unless (eq? (declaration-kind name) kind)
    (error "state slot used with the wrong accessor" name kind))
  name)

(define (define-state-slot! name initializer)
  "Declare a mutable state slot. INITIALIZER is a thunk producing its initial
value on first reference."
  (unless (symbol? name)
    (error "state slot name must be a symbol" name))
  (unless (procedure? initializer)
    (error "state slot initializer must be a thunk" name))
  (hashq-set! slot-declarations name (vector 'state initializer))
  name)

(define* (define-policy-slot! name #:optional (default #f))
  "Declare a configurable policy slot. DEFAULT is the procedure used when the
application has not configured one; #f makes configuration mandatory."
  (unless (symbol? name)
    (error "policy slot name must be a symbol" name))
  (unless (or (not default) (procedure? default))
    (error "policy slot default must be a procedure or #f" name))
  (hashq-set! slot-declarations name (vector 'policy default))
  name)

;; Slot access sits on the keystroke path, so a hit must cost one lookup. The
;; root keeps state and policy values in separate tables: a hit is
;; root + vector-ref + hashq-ref, and reading a slot with the wrong accessor
;; simply misses its table and falls into the slow path, which consults the
;; declaration and raises. Validating the kind on every read would instead put
;; a second hash lookup on the hot path for a check that only fires on a bug.
(define (root host)
  (or (hashq-ref state-roots host)
      (let ((tables (vector (make-hash-table) (make-hash-table))))
        (hashq-set! state-roots host tables)
        tables)))

(define (state-table host) (vector-ref (root host) 0))
(define (policy-table host) (vector-ref (root host) 1))

(define %missing (list 'missing))

(define (state-ref-slow host name table)
  (expect-kind name 'state)
  (let ((initial ((vector-ref (declaration name) 1))))
    (hashq-set! table name initial)
    initial))

(define (state-ref host name)
  "Return the value of state slot NAME for HOST, initializing it on first use."
  (let* ((table (state-table host))
         (value (hashq-ref table name %missing)))
    (if (eq? value %missing)
        (state-ref-slow host name table)
        value)))

(define (state-set! host name value)
  (expect-kind name 'state)
  (hashq-set! (state-table host) name value)
  value)

(define (state-update! host name procedure)
  "Replace state slot NAME with (PROCEDURE current-value)."
  (state-set! host name (procedure (state-ref host name))))

(define (state-clear! host name)
  "Drop state slot NAME so its initializer runs again on the next reference."
  (expect-kind name 'state)
  (hashq-remove! (state-table host) name))

(define (policy-ref host name)
  "Return the procedure configured for policy slot NAME, or its declared
default. Raises when a policy without a default is unconfigured."
  (or (hashq-ref (policy-table host) name)
      (begin
        (expect-kind name 'policy)
        (or (vector-ref (declaration name) 1)
            (error "policy is not configured" name)))))

(define (policy-set! host name procedure)
  (expect-kind name 'policy)
  (unless (procedure? procedure)
    (error "policy must be a procedure" name))
  (hashq-set! (policy-table host) name procedure)
  procedure)

(define (policy-configured? host name)
  (expect-kind name 'policy)
  (and (hashq-ref (policy-table host) name) #t))

(define (state-slot-names)
  "All declared slots as (name . kind) pairs, sorted by name."
  (sort (hash-map->list (lambda (name declared) (cons name (vector-ref declared 0)))
                        slot-declarations)
        (lambda (left right)
          (string<? (symbol->string (car left)) (symbol->string (car right))))))

(define (application-state-snapshot host)
  "The application's live state as one enumerable value: a list of
#(name kind value-or-'unset) records in slot-name order. This is the
inspection payoff of a single root -- no per-subsystem serializer."
  (let ((tables (or (hashq-ref state-roots host)
                    (vector (make-hash-table) (make-hash-table)))))
    (map (lambda (entry)
           (let* ((name (car entry))
                  (kind (cdr entry))
                  (value (hashq-ref (vector-ref tables (if (eq? kind 'state) 0 1))
                                    name %missing)))
             (vector name kind (if (eq? value %missing) 'unset value))))
         (state-slot-names))))

(define (release-application-state! host)
  "Drop every slot owned by HOST. The weak root makes this an optimization
rather than a correctness requirement."
  (hashq-remove! state-roots host)
  *unspecified*)
