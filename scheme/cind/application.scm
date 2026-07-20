(define-module (cind application)
  #:export (application-state
            exit-editor!
            request-redraw!
            caret-reveal?
            set-caret-reveal!
            page-rows
            set-page-rows!
            reconcile-completion!
            completion-transition!
            finish-completion!
            completion-selection
            move-completion-selection!))

;; #(exit-requested? caret-reveal? page-rows). Application instances are
;; represented by weak host capabilities, so state disappears with the owning
;; editor.
(define application-states (make-weak-key-hash-table))
(define completion-states (make-weak-key-hash-table))

(define (host-application-state host)
  (or (hashq-ref application-states host)
      (let ((state (vector #f #t 1)))
        (hashq-set! application-states host state)
        state)))

(define (application-state host)
  (let ((state (host-application-state host)))
    (vector (vector-ref state 0) (vector-ref state 1) (vector-ref state 2))))

(define (exit-editor! host)
  (vector-set! (host-application-state host) 0 #t))

(define (caret-reveal? host)
  (vector-ref (host-application-state host) 1))

(define (set-caret-reveal! host reveal?)
  (unless (boolean? reveal?)
    (error "caret reveal state must be a boolean" reveal?))
  (vector-set! (host-application-state host) 1 reveal?))

(define (request-redraw! host)
  (set-caret-reveal! host #t))

(define (page-rows host)
  (vector-ref (host-application-state host) 2))

(define (set-page-rows! host rows)
  (unless (and (integer? rows) (positive? rows))
    (error "page rows must be a positive integer" rows))
  (vector-set! (host-application-state host) 2 rows))

;; #(item-ids selected-item-id selected-index). The item id keeps selection
;; stable while asynchronous providers replace and reorder native candidates.
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
  (if (zero? (vector-length item-ids))
      (begin
        (hashq-set! completion-states host (vector item-ids #f #f))
        #f)
      (let* ((state (hashq-ref completion-states host))
             (selected-id (and state (vector-ref state 1)))
             (selected-index (and selected-id
                                  (completion-index item-ids selected-id)))
             (index (or selected-index 0))
             (id (vector-ref item-ids index)))
        (hashq-set! completion-states host (vector item-ids id index))
        index)))

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
