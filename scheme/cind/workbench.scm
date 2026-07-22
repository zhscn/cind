(define-module (cind workbench)
  #:use-module (cind state)
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
            workbench-location-list-published!
            workbench-location-navigation
            workbench-set-location-navigation!
            workbench-location-list-key
            workbench-move-location-list!
            workbench-location-list-states
            workbench-transaction-group-recorded!
            workbench-transaction-group-movable?
            workbench-transaction-group-moved!
            workbench-jump-record!
            workbench-jump-move!
            workbench-jump-current
            workbench-jump-forget!
            workbench-jump-restore!
            workbench-jump-walk
            workbench-jump-session-walk
            workbench-jump-track-intent?
            workbench-jump-transition!
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
            workbench-slots
            window-role
            set-window-role!
            window-pinned?
            set-window-pinned!
            window-created-by-policy?))

;; Entries are
;; #(workbench name mru-list scope-list window-list active-window
;;   location-list-records current-location-list-or-#f
;;   transaction-group-records).
;; Window records are
;; #(window role-or-#f pinned? created-by-policy? jump-list jump-cursor-or-#f).
;; Window layouts, LocationList items, transaction entries, anchors and entity
;; lifetimes are native data-plane state. Selection, descriptive, membership,
;; navigation, undo direction and display policy state is owned by Guile.
;; Location records are
;; #(list-id materialized-buffer-or-#f item-count selected-index-or-#f).
;; Transaction records are #(group-id undone?).
;; The active workbench is #f only before the first one is created; every
;; accessor treats that as an unavailable selection.
(define-state-slot! 'workbenches (lambda () '()))
(define-state-slot! 'active-workbench (lambda () #f))

(define (host-workbench-states host)
  (state-ref host 'workbenches))

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
    (state-set! host 'workbenches
                (cons (vector workbench
                              name
                              (if initial-buffer (list initial-buffer) '())
                              (unique-values (vector->list scope))
                              (list (vector root-window #f #f #f '() #f))
                              root-window
                              '()
                              #f
                              '())
                      entries))
    (when (null? entries)
      (state-set! host 'active-workbench workbench)))
  workbench)

(define (active-workbench host)
  (or (state-ref host 'active-workbench)
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
  (state-set! host 'active-workbench workbench)
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
     (vector-set! entry 2 (without-buffer (vector-ref entry 2) buffer))
     (for-each
      (lambda (record)
        (when (and (vector-ref record 1)
                   (equal? buffer (vector-ref record 1)))
          (vector-set! record 1 #f)))
      (vector-ref entry 6)))
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
          (state-clear! host 'workbenches)
          (state-clear! host 'active-workbench))
        (state-set! host 'workbenches remaining))
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

(define (location-record-by-id entry list-id)
  (let loop ((records (vector-ref entry 6)))
    (and (pair? records)
         (if (= list-id (vector-ref (car records) 0))
             (car records)
             (loop (cdr records))))))

(define (location-record-by-buffer entry buffer)
  (let loop ((records (vector-ref entry 6)))
    (and (pair? records)
         (if (and (vector-ref (car records) 1)
                  (equal? buffer (vector-ref (car records) 1)))
             (car records)
             (loop (cdr records))))))

(define (current-location-record entry)
  (let ((current (vector-ref entry 7)))
    (and current (location-record-by-id entry current))))

(define (workbench-location-list-published! host workbench list-id buffer item-count)
  (unless (and (integer? list-id) (> list-id 0))
    (error "location list ID must be a positive integer" list-id))
  (unless (and (integer? item-count) (>= item-count 0))
    (error "location list item count must be a non-negative integer" item-count))
  (buffer-name host buffer)
  (let ((entry (require-workbench-entry host workbench)))
    (when (location-record-by-id entry list-id)
      (error "location list policy state already exists" list-id))
    (vector-set! entry 6
                 (append (vector-ref entry 6)
                         (list (vector list-id buffer item-count #f))))
    (vector-set! entry 7 list-id))
  list-id)

(define (workbench-location-navigation host workbench)
  (let ((record (current-location-record
                 (require-workbench-entry host workbench))))
    (if record
        (vector (vector-ref record 1)
                (vector-ref record 3)
                (vector-ref record 2))
        (vector #f #f 0))))

(define (require-selected-index record selected)
  (unless (or (not selected)
              (and (integer? selected)
                   (>= selected 0)
                   (< selected (vector-ref record 2))))
    (error "location index is out of range" selected))
  selected)

(define (workbench-set-location-navigation! host workbench buffer selected)
  (let* ((entry (require-workbench-entry host workbench))
         (record (if buffer
                     (location-record-by-buffer entry buffer)
                     (current-location-record entry))))
    (if (not record)
        (if (and (not buffer) (not selected))
            #f
            (error "location list is no longer available" buffer))
        (begin
          (require-selected-index record selected)
          (vector-set! entry 7 (vector-ref record 0))
          (vector-set! record 3 selected)
          (vector-ref record 0)))))

(define (workbench-location-list-key host workbench buffer)
  (let* ((entry (require-workbench-entry host workbench))
         (record (if buffer
                     (location-record-by-buffer entry buffer)
                     (current-location-record entry))))
    (and record (vector workbench (vector-ref record 0)))))

(define (workbench-move-location-list! host workbench delta)
  (unless (integer? delta)
    (error "location list movement must be an integer" delta))
  (let* ((entry (require-workbench-entry host workbench))
         (records (vector-ref entry 6))
         (current (vector-ref entry 7)))
    (if (or (not current) (= delta 0))
        #f
        (let loop ((remaining records) (index 0))
          (cond ((null? remaining) #f)
                ((= current (vector-ref (car remaining) 0))
                 (let ((target (+ index delta)))
                   (if (or (< target 0) (>= target (length records)))
                       #f
                       (begin
                         (vector-set! entry 7
                                      (vector-ref (list-ref records target) 0))
                         #t))))
                (else (loop (cdr remaining) (+ index 1))))))))

(define (workbench-location-list-states host workbench)
  (let* ((entry (require-workbench-entry host workbench))
         (current (vector-ref entry 7)))
    (list->vector
     (map (lambda (record)
            (vector (vector-ref record 0)
                    (vector-ref record 3)
                    (and current (= current (vector-ref record 0)))))
          (vector-ref entry 6)))))

(define (transaction-group-record-by-id entry group-id)
  (let loop ((records (vector-ref entry 8)))
    (and (pair? records)
         (if (= group-id (vector-ref (car records) 0))
             (car records)
             (loop (cdr records))))))

(define (require-transaction-group-id group-id)
  (unless (and (integer? group-id) (> group-id 0))
    (error "transaction group ID must be a positive integer" group-id)))

(define (require-transaction-group-record entry group-id)
  (or (transaction-group-record-by-id entry group-id)
      (error "unknown transaction group policy state" group-id)))

(define (workbench-transaction-group-recorded! host workbench group-id)
  (require-transaction-group-id group-id)
  (let ((entry (require-workbench-entry host workbench)))
    (when (transaction-group-record-by-id entry group-id)
      (error "transaction group policy state already exists" group-id))
    (vector-set! entry 8
                 (append (vector-ref entry 8)
                         (list (vector group-id #f)))))
  group-id)

(define (workbench-transaction-group-movable? host workbench group-id redo?)
  (require-transaction-group-id group-id)
  (unless (boolean? redo?)
    (error "transaction group redo direction must be boolean" redo?))
  (let* ((entry (require-workbench-entry host workbench))
         (record (transaction-group-record-by-id entry group-id)))
    (and record (eq? (vector-ref record 1) redo?))))

(define (workbench-transaction-group-moved! host workbench group-id redo? changed?)
  (require-transaction-group-id group-id)
  (unless (boolean? redo?)
    (error "transaction group redo direction must be boolean" redo?))
  (unless (boolean? changed?)
    (error "transaction group changed state must be boolean" changed?))
  (let* ((entry (require-workbench-entry host workbench))
         (record (require-transaction-group-record entry group-id)))
    (unless (eq? (vector-ref record 1) redo?)
      (error "transaction group direction is unavailable" group-id redo?))
    (when changed?
      (vector-set! record 1 (not redo?))))
  changed?)

(define (require-jump-node node)
  (unless (and (integer? node) (> node 0))
    (error "jump node must be a positive integer" node)))

(define (jump-walk-without entries cursor removed)
  (let loop ((remaining entries)
             (index 0)
             (retained '())
             (retained-count 0)
             (retained-cursor #f))
    (if (null? remaining)
        (let ((walk (reverse retained)))
          (vector walk
                  (if (null? walk)
                      #f
                      (if retained-cursor retained-cursor 0))))
        (if (member (car remaining) removed)
            (loop (cdr remaining) (+ index 1) retained retained-count retained-cursor)
            (loop (cdr remaining)
                  (+ index 1)
                  (cons (car remaining) retained)
                  (+ retained-count 1)
                  (if (<= index cursor) retained-count retained-cursor))))))

(define (workbench-jump-record! host window node)
  (require-jump-node node)
  (let* ((state (cdr (require-window-entry-with-workbench host window)))
         (entries (vector-ref state 4))
         (cursor (vector-ref state 5)))
    (if (and (pair? entries)
             (= (list-ref entries (- (length entries) 1)) node)
             cursor
             (= cursor (- (length entries) 1)))
        #f
        (begin
          (vector-set! state 4 (append entries (list node)))
          (vector-set! state 5 (length entries))
          #t))))

(define (workbench-jump-move! host window delta)
  (unless (integer? delta)
    (error "jump movement must be an integer" delta))
  (let* ((state (cdr (require-window-entry-with-workbench host window)))
         (entries (vector-ref state 4))
         (cursor (vector-ref state 5))
         (target (and cursor (+ cursor delta))))
    (if (or (not target)
            (= delta 0)
            (< target 0)
            (>= target (length entries)))
        #f
        (begin
          (vector-set! state 5 target)
          (list-ref entries target)))))

(define (workbench-jump-current host window)
  (let* ((state (cdr (require-window-entry-with-workbench host window)))
         (cursor (vector-ref state 5)))
    (and cursor (list-ref (vector-ref state 4) cursor))))

(define (workbench-jump-forget! host workbench nodes)
  (unless (vector? nodes)
    (error "forgotten jump nodes must be a vector" nodes))
  (for-each require-jump-node (vector->list nodes))
  (let ((entry (require-workbench-entry host workbench))
        (removed (vector->list nodes)))
    (for-each
     (lambda (state)
       (let ((cursor (vector-ref state 5)))
         (when cursor
           (let ((walk (jump-walk-without (vector-ref state 4) cursor removed)))
             (vector-set! state 4 (vector-ref walk 0))
             (vector-set! state 5 (vector-ref walk 1))))))
     (vector-ref entry 4)))
  (vector-length nodes))

(define (workbench-jump-restore! host window entries cursor)
  (unless (vector? entries)
    (error "restored jump walk must be a vector" entries))
  (let ((walk (vector->list entries)))
    (for-each require-jump-node walk)
    (unless (or (and (null? walk) (not cursor))
                (and (pair? walk)
                     (integer? cursor)
                     (>= cursor 0)
                     (< cursor (length walk))))
      (error "restored jump cursor is invalid" cursor))
    (let ((state (cdr (require-window-entry-with-workbench host window))))
      (vector-set! state 4 walk)
      (vector-set! state 5 cursor)))
  window)

(define (workbench-jump-walk host window)
  (let ((state (cdr (require-window-entry-with-workbench host window))))
    (vector (list->vector (vector-ref state 4))
            (vector-ref state 5))))

(define (drop-values values count)
  (if (= count 0)
      values
      (drop-values (cdr values) (- count 1))))

(define (take-values values count)
  (if (= count 0)
      '()
      (cons (car values) (take-values (cdr values) (- count 1)))))

(define (workbench-jump-session-walk host window durable-nodes maximum-entries)
  (unless (vector? durable-nodes)
    (error "durable jump nodes must be a vector" durable-nodes))
  (for-each require-jump-node (vector->list durable-nodes))
  (unless (and (integer? maximum-entries) (> maximum-entries 0))
    (error "jump session limit must be a positive integer" maximum-entries))
  (let* ((state (cdr (require-window-entry-with-workbench host window)))
         (source (vector-ref state 4))
         (source-cursor (vector-ref state 5))
         (count (length source))
         (half (quotient maximum-entries 2))
         (first (if (and source-cursor (> count maximum-entries))
                    (min (- (max source-cursor half) half)
                         (- count maximum-entries))
                    0))
         (entries (if (> count maximum-entries)
                      (take-values (drop-values source first) maximum-entries)
                      source))
         (cursor (and source-cursor (- source-cursor first)))
         (durable (vector->list durable-nodes))
         (transient (if cursor
                        (let loop ((remaining entries) (result '()))
                          (if (null? remaining)
                              result
                              (loop (cdr remaining)
                                    (if (member (car remaining) durable)
                                        result
                                        (cons (car remaining) result)))))
                        '()))
         (filtered (if cursor
                       (jump-walk-without entries cursor transient)
                       (vector '() #f))))
    (vector (list->vector (vector-ref filtered 0))
            (vector-ref filtered 1))))

(define (require-jump-intent intent)
  (unless (and (string? intent) (> (string-length intent) 0))
    (error "jump display intent must be a non-empty string" intent)))

(define (jump-edge-kind intent)
  (cond ((string=? intent "list") "list")
        ((member intent '("definition" "declaration" "implementation")) "def")
        ((member intent '("reference" "references")) "ref")
        ((string=? intent "search") "search")
        ((string=? intent "manual") "manual")
        (else "open")))

(define (workbench-jump-track-intent? intent)
  (require-jump-intent intent)
  (not (string=? intent "replay")))

(define (workbench-jump-transition! host window intent from to)
  (require-jump-intent intent)
  (require-jump-node from)
  (require-jump-node to)
  (if (not (workbench-jump-track-intent? intent))
      #f
      (begin
        (workbench-jump-record! host window from)
        (workbench-jump-record! host window to)
        (and (not (= from to)) (jump-edge-kind intent)))))

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
                         (list (vector window #f #f #f '() #f)))))
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

;; Commands name roles with symbols while the stored form is a string. These
;; used to be host primitives that validated the window and then called back
;; into this module for the value; the state has always lived here, so the
;; round trip through C++ bought nothing (design/09-guile-first.md §0.2).
(define (window-role host window)
  (let ((role (vector-ref (workbench-window-state-or-default host window) 1)))
    (and role (string->symbol role))))

(define (set-window-role! host window role)
  (unless (or (not role) (symbol? role))
    (error "window role must be a symbol or #f" role))
  (workbench-set-window-role! host window (and role (symbol->string role))))

(define (window-pinned? host window)
  (vector-ref (workbench-window-state-or-default host window) 2))

(define (set-window-pinned! host window pinned?)
  (workbench-set-window-pinned! host window pinned?))

(define (window-created-by-policy? host window)
  (vector-ref (workbench-window-state-or-default host window) 3))

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
