(define-module (cind completion)
  #:export (configure-completion-policy!
            resolve-completion-policy
            completion-policy-replace?
            begin-completion!
            completion-session-replace?
            reconcile-completion!
            completion-transition!
            finish-completion!
            completion-selection
            move-completion-selection!))

;; A completion policy is
;; #(completion-policy rank-keys replace-on-accept? visible-resolve-count).
;; The procedure is resolved when a session starts, so modes and extensions
;; can vary behavior using the command context without changing the native
;; completion mechanism.
(define completion-policies (make-weak-key-hash-table))

(define (default-completion-policy host context trigger)
  (vector 'completion-policy
          #(match-tier fuzzy-score sort-text kind label)
          #f
          8))

(define (valid-rank-keys? keys)
  (and (vector? keys)
       (> (vector-length keys) 0)
       (let loop ((index 0)
                  (seen '()))
         (if (= index (vector-length keys))
             #t
             (let ((key (vector-ref keys index)))
               (and (memq key '(match-tier fuzzy-score sort-text kind label))
                    (not (memq key seen))
                    (loop (+ index 1) (cons key seen))))))))

(define (validate-completion-policy policy)
  (unless (and (vector? policy)
               (= (vector-length policy) 4)
               (eq? (vector-ref policy 0) 'completion-policy)
               (valid-rank-keys? (vector-ref policy 1))
               (boolean? (vector-ref policy 2))
               (integer? (vector-ref policy 3))
               (>= (vector-ref policy 3) 0))
    (error "invalid completion policy" policy))
  policy)

(define (configure-completion-policy! host procedure)
  (unless (procedure? procedure)
    (error "completion policy must be a procedure" procedure))
  (hashq-set! completion-policies host procedure)
  procedure)

(define (resolve-completion-policy host context trigger)
  (validate-completion-policy
   ((or (hashq-ref completion-policies host) default-completion-policy)
    host context trigger)))

(define (completion-policy-replace? policy)
  (validate-completion-policy policy)
  (vector-ref policy 2))

;; Each value is #(item-ids selected-item-id selected-index policy). The stable
;; item id preserves selection while asynchronous providers replace and
;; reorder native candidates. The resolved policy remains fixed for the
;; lifetime of a session.
(define completion-states (make-weak-key-hash-table))

(define (begin-completion! host policy)
  (validate-completion-policy policy)
  (hashq-set! completion-states host (vector #() #f #f policy))
  policy)

(define (completion-session-replace? host)
  (let ((state (hashq-ref completion-states host)))
    (and state
         (let ((policy (vector-ref state 3)))
           (and policy (completion-policy-replace? policy))))))

(define (completion-index item-ids item-id)
  (let loop ((index 0))
    (and (< index (vector-length item-ids))
         (if (= (vector-ref item-ids index) item-id)
             index
             (loop (+ index 1))))))

(define (reconcile-completion! host item-ids)
  (unless (and (vector? item-ids)
               (let loop ((index 0))
                 (or (= index (vector-length item-ids))
                     (and (integer? (vector-ref item-ids index))
                          (positive? (vector-ref item-ids index))
                          (loop (+ index 1))))))
    (error "completion item ids must be a vector of positive integers" item-ids))
  (let ((state (hashq-ref completion-states host)))
    (if (zero? (vector-length item-ids))
        (begin
          (hashq-set! completion-states
                      host
                      (vector item-ids #f #f (and state (vector-ref state 3))))
          #f)
        (let* ((selected-id (and state (vector-ref state 1)))
               (selected-index (and selected-id
                                    (completion-index item-ids selected-id)))
               (index (or selected-index 0))
               (id (vector-ref item-ids index)))
          (hashq-set! completion-states
                      host
                      (vector item-ids id index (and state (vector-ref state 3))))
          index))))

(define (completion-transition! host item-ids automatic? pending?)
  (unless (and (boolean? automatic?) (boolean? pending?))
    (error "completion transition flags must be booleans" automatic? pending?))
  (let* ((selection (reconcile-completion! host item-ids))
         (cancel? (and automatic?
                       (not pending?)
                       (zero? (vector-length item-ids)))))
    (when cancel?
      (finish-completion! host))
    (vector selection cancel?)))

(define (finish-completion! host)
  (hashq-remove! completion-states host))

(define (completion-selection host)
  (let ((state (hashq-ref completion-states host)))
    (and state (vector-ref state 2))))

(define (move-completion-selection! host delta)
  (unless (integer? delta)
    (error "completion selection delta must be an integer" delta))
  (let ((state (hashq-ref completion-states host)))
    (if (or (not state) (zero? (vector-length (vector-ref state 0))))
        #f
        (let* ((item-ids (vector-ref state 0))
               (count (vector-length item-ids))
               (index (modulo (+ (vector-ref state 2) delta) count)))
          (vector-set! state 1 (vector-ref item-ids index))
          (vector-set! state 2 index)
          index))))
