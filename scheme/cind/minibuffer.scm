(define-module (cind minibuffer)
  #:use-module (cind state)
  #:use-module (ice-9 optargs)
  #:use-module (srfi srfi-13)
  #:use-module (cind command)
  #:use-module (cind host)
  #:export (read-from-minibuffer
            completing-read
            rank-completion-candidates
            rank-provider-result
            make-bounded-history-policy
            configure-minibuffer-history-policy!
            minibuffer-history
            set-minibuffer-history!
            minibuffer-history-state
            interaction-started!
            interaction-policy-state
            interaction-status
            interaction-selection
            interaction-provider
            set-interaction-provider!
            refresh-interaction!
            minibuffer-input-changed!
            clear-minibuffer-navigation!
            record-minibuffer-history!
            move-minibuffer-candidate!
            move-minibuffer-history!
            submit-interaction!
            cancel-interaction!))

;; #f means "no active interaction" / "not navigating history", which the
;; initializers preserve (design/09-guile-first.md §4).
(define-policy-slot! 'minibuffer-history)
(define-state-slot! 'minibuffer-histories (lambda () (make-hash-table)))
(define-state-slot! 'minibuffer-navigation (lambda () #f))
(define-state-slot! 'minibuffer-selection (lambda () #f))
(define-state-slot! 'interaction (lambda () #f))

(define (interaction-started! host state)
  (unless (and (vector? state)
               (= (vector-length state) 11)
               (memq (vector-ref state 0) '(text picker))
               (string? (vector-ref state 1))
               (string? (vector-ref state 2))
               (string? (vector-ref state 3))
               (string? (vector-ref state 4))
               (string? (vector-ref state 5))
               (boolean? (vector-ref state 6))
               (string? (vector-ref state 7))
               (string? (vector-ref state 8))
               (vector? (vector-ref state 9))
               (and (vector? (vector-ref state 10))
                    (= (vector-length (vector-ref state 10)) 3)))
    (error "invalid interaction policy state" state))
  (state-set! host 'interaction state)
  (state-clear! host 'minibuffer-selection)
  state)

(define (interaction-policy-state host)
  (let ((state (state-ref host 'interaction))
        (mechanism (interaction-mechanism-status host)))
    (if (and state (vector-ref mechanism 0))
        (vector (vector-ref state 0)
                (vector-ref state 1)
                (vector-ref state 2)
                (vector-ref state 3)
                (vector-ref state 4)
                (vector-ref state 5)
                (vector-ref state 6)
                (vector-ref state 7))
        (begin
          (state-clear! host 'interaction)
          #f))))

(define (interaction-selection-for host revision count)
  (let ((state (state-ref host 'minibuffer-selection)))
    (cond
     ((zero? count)
      (state-clear! host 'minibuffer-selection)
      #f)
     ((or (not state) (not (= revision (vector-ref state 0))))
      (state-set! host 'minibuffer-selection (vector revision 0))
      0)
     ((>= (vector-ref state 1) count)
      (vector-set! state 1 0)
      0)
     (else (vector-ref state 1)))))

(define (interaction-status host)
  (let ((mechanism (interaction-mechanism-status host))
        (state (state-ref host 'interaction)))
    (if (vector-ref mechanism 0)
        (begin
          (unless state
            (error "active interaction has no Guile policy state"))
          (let* ((picker? (eq? (vector-ref state 0) 'picker))
                 (history (vector-ref state 5))
                 (count (vector-ref mechanism 1))
                 (selected (and picker?
                                (interaction-selection-for
                                 host (vector-ref mechanism 4) count))))
            (vector #t picker? (not (zero? (string-length history)))
                    (and (not (zero? (string-length history))) history)
                    selected count
                    (vector-ref mechanism 2)
                    (vector-ref mechanism 3))))
        (begin
          (state-clear! host 'interaction)
          (state-clear! host 'minibuffer-selection)
          (vector #f #f #f #f #f 0 #f #f)))))

(define (interaction-selection host)
  (vector-ref (interaction-status host) 4))

(define (interaction-provider host)
  (let ((state (state-ref host 'interaction)))
    (and state (vector-ref state 7))))

(define (refresh-interaction! host)
  (let ((state (state-ref host 'interaction)))
    (when state
      (refresh-interaction-mechanism! host (vector-ref state 7)))))

(define (set-interaction-provider! host provider)
  (unless (string? provider)
    (error "interaction provider must be a string" provider))
  (let ((state (state-ref host 'interaction)))
    (unless (and state (eq? (vector-ref state 0) 'picker))
      (error "no picker interaction is active"))
    (let ((previous (vector-ref state 7)))
      (vector-set! state 7 provider)
      (catch #t
        (lambda () (refresh-interaction-mechanism! host provider))
        (lambda (key . arguments)
          (vector-set! state 7 previous)
          (apply throw key arguments))))))

(define (set-interaction-selection! host index)
  (let* ((mechanism (interaction-mechanism-status host))
         (state (state-ref host 'interaction))
         (count (vector-ref mechanism 1)))
    (and (vector-ref mechanism 0)
         state
         (eq? (vector-ref state 0) 'picker)
         (< index count)
         (begin
           (state-set! host 'minibuffer-selection
                       (vector (vector-ref mechanism 4) index))
           #t))))

(define (host-histories host)
  (state-ref host 'minibuffer-histories))

(define (minibuffer-history host name)
  (unless (string? name)
    (error "minibuffer history name must be a string" name))
  (or (hash-ref (host-histories host) name) #()))

(define (set-minibuffer-history! host name entries)
  (unless (and (string? name) (> (string-length name) 0))
    (error "minibuffer history name must be a non-empty string" name))
  (unless (string-vector? entries)
    (error "minibuffer history entries must be a string vector" entries))
  (hash-set! (host-histories host) name entries))

(define (clear-minibuffer-navigation! host)
  (state-clear! host 'minibuffer-navigation))

(define (minibuffer-navigation host buffer)
  (let ((navigation (state-ref host 'minibuffer-navigation)))
    (and navigation (equal? buffer (vector-ref navigation 0)) navigation)))

(define (minibuffer-history-state host buffer name)
  (let ((navigation (minibuffer-navigation host buffer)))
    (vector (vector-length (minibuffer-history host name))
            (and navigation (vector-ref navigation 1))
            (if navigation (vector-ref navigation 2) ""))))

(define (minibuffer-input-changed! host buffer revision)
  (let ((navigation (minibuffer-navigation host buffer)))
    (when (and navigation (not (= revision (vector-ref navigation 3))))
      (clear-minibuffer-navigation! host))
    (refresh-interaction! host)))

(define (make-bounded-history-policy maximum)
  (unless (and (integer? maximum) (>= maximum 0))
    (error "history maximum must be a non-negative integer" maximum))
  (lambda (entries value)
    (unless (vector? entries)
      (error "history entries must be a vector" entries))
    (unless (string? value)
      (error "history value must be a string" value))
    (let* ((count (vector-length entries))
           (duplicate? (and (> count 0)
                            (string=? (vector-ref entries (- count 1)) value)))
           (values (if duplicate?
                       (vector->list entries)
                       (append (vector->list entries) (list value))))
           (overflow (max 0 (- (length values) maximum))))
      (list->vector (list-tail values overflow)))))

(define (configure-minibuffer-history-policy! host procedure)
  (policy-set! host 'minibuffer-history procedure))

(define (string-vector? value)
  (and (vector? value)
       (let loop ((index 0))
         (or (= index (vector-length value))
             (and (string? (vector-ref value index))
                  (loop (+ index 1)))))))

(define (record-minibuffer-history! host name value)
  (unless (or (not name) (string? name))
    (error "minibuffer history name must be #f or a string" name))
  (unless (string? value)
    (error "minibuffer history value must be a string" value))
  (clear-minibuffer-navigation! host)
  (when (and name
             (> (string-length name) 0)
             (> (string-length value) 0))
    (let ((policy (policy-ref host 'minibuffer-history)))
      (let ((entries (policy (minibuffer-history host name) value)))
        (unless (string-vector? entries)
          (error "minibuffer history policy must return a string vector" entries))
        (set-minibuffer-history! host name entries)))))

(define (move-minibuffer-candidate! host delta)
  (let* ((status (interaction-status host))
         (selected (vector-ref status 4))
         (count (vector-ref status 5)))
    (and selected
         (> count 0)
         (not (zero? delta))
         (set-interaction-selection!
          host (modulo (+ selected delta) count)))))

(define (submit-interaction! host)
  (let* ((state (state-ref host 'interaction))
         (selected (interaction-selection host)))
    (unless state
      (error "no interaction policy state is active"))
    (let ((value (submit-interaction-mechanism!
                  host selected (vector-ref state 6)))
          (arguments (vector-ref state 9))
          (target (vector-ref state 10)))
      (state-clear! host 'interaction)
      (state-clear! host 'minibuffer-selection)
      (vector (vector-ref state 8)
              (list->vector (append (vector->list arguments) (list value)))
              target
              (let ((history (vector-ref state 5)))
                (and (not (zero? (string-length history))) history))))))

(define (cancel-interaction! host)
  (let ((cancelled? (cancel-interaction-mechanism! host)))
    (state-clear! host 'interaction)
    (state-clear! host 'minibuffer-selection)
    cancelled?))

(define (move-minibuffer-history! host context delta)
  (let* ((status (interaction-status host))
         (name (vector-ref status 3))
         (navigation (minibuffer-navigation host (context-buffer context)))
         (index (and navigation (vector-ref navigation 1)))
         (stored-draft (if navigation (vector-ref navigation 2) ""))
         (entries (and name (minibuffer-history host name)))
         (count (if entries (vector-length entries) 0))
         (current (buffer-text host (context-buffer context))))
    (cond
     ((or (not name) (zero? delta)) #f)
     ((< delta 0)
      (let ((target (if index
                        (and (> index 0) (- index 1))
                        (and (> count 0) (- count 1))))
            (draft (if index stored-draft current)))
        (and target
             (let ((revision
                    (replace-interaction-input! host (vector-ref entries target))))
               (state-set! host 'minibuffer-navigation
                           (vector (context-buffer context) target draft revision))
               #t))))
     ((not index) #f)
     ((< (+ index 1) count)
      (let ((target (+ index 1)))
        (let ((revision
               (replace-interaction-input! host (vector-ref entries target))))
          (state-set! host 'minibuffer-navigation
                      (vector (context-buffer context) target stored-draft revision))
          #t)))
     (else
      (let ((revision (replace-interaction-input! host stored-draft)))
        (state-set! host 'minibuffer-navigation
                    (vector (context-buffer context) #f stored-draft revision))
        #t)))))

(define (candidate-filter-text candidate)
  (let ((filter-text (vector-ref candidate 3)))
    (if (zero? (string-length filter-text))
        (vector-ref candidate 1)
        filter-text)))

(define (candidate-score candidate terms)
  (let ((haystack (string-downcase (candidate-filter-text candidate))))
    (let loop ((remaining terms)
               (score (string-length haystack)))
      (if (null? remaining)
          score
          (let ((position (string-contains haystack (car remaining))))
            (and position (loop (cdr remaining) (+ score position))))))))

(define (rank-completion-candidates candidates query)
  (unless (vector? candidates)
    (error "completion candidates must be a vector" candidates))
  (let ((terms (string-tokenize (string-downcase query))))
    (let loop ((index 0)
               (ranked '()))
      (if (= index (vector-length candidates))
          (list->vector
           (map (lambda (entry) (vector-ref entry 2))
                (sort ranked
                      (lambda (left right)
                        (or (< (vector-ref left 0) (vector-ref right 0))
                            (and (= (vector-ref left 0) (vector-ref right 0))
                                 (< (vector-ref left 1) (vector-ref right 1))))))))
          (let* ((candidate (vector-ref candidates index))
                 (score (candidate-score candidate terms)))
            (loop (+ index 1)
                  (if score
                      (cons (vector score index candidate) ranked)
                      ranked)))))))

(define (rank-provider-result result query)
  (if (and (vector? result)
           (= (vector-length result) 3)
           (eq? (vector-ref result 0) 'async-provider))
      (interaction-provider-task
       (vector-ref result 1)
       (let ((transform (vector-ref result 2)))
         (lambda (async-result)
           (rank-completion-candidates (transform async-result) query))))
      (rank-completion-candidates result query)))

(define (require-string procedure field value)
  (unless (string? value)
    (error (string-append procedure ": " field " must be a string") value)))

(define (require-symbol procedure field value)
  (unless (symbol? value)
    (error (string-append procedure ": " field " must be a symbol") value)))

(define (require-arguments procedure arguments)
  (unless (list? arguments)
    (error (string-append procedure ": arguments must be a proper list") arguments)))

(define* (read-from-minibuffer prompt accept-command
                               #:key
                               (initial-input "")
                               (history "")
                               (keymap 'interaction.text)
                               (input-state 'emacs)
                               (buffer-name " *minibuffer*")
                               (arguments '()))
  (require-string "read-from-minibuffer" "prompt" prompt)
  (require-string "read-from-minibuffer" "accept-command" accept-command)
  (require-string "read-from-minibuffer" "initial-input" initial-input)
  (require-string "read-from-minibuffer" "history" history)
  (require-symbol "read-from-minibuffer" "keymap" keymap)
  (require-symbol "read-from-minibuffer" "input-state" input-state)
  (require-string "read-from-minibuffer" "buffer-name" buffer-name)
  (when (zero? (string-length buffer-name))
    (error "read-from-minibuffer: buffer-name must not be empty"))
  (require-arguments "read-from-minibuffer" arguments)
  (apply interaction 'text keymap input-state buffer-name prompt initial-input history "" #t
         accept-command arguments))

(define* (completing-read prompt provider accept-command
                          #:key
                          (initial-input "")
                          (history "")
                          (allow-custom-input? #f)
                          (keymap 'interaction.picker)
                          (input-state 'emacs)
                          (buffer-name " *minibuffer*")
                          (arguments '()))
  (require-string "completing-read" "prompt" prompt)
  (require-string "completing-read" "provider" provider)
  (require-string "completing-read" "accept-command" accept-command)
  (require-string "completing-read" "initial-input" initial-input)
  (require-string "completing-read" "history" history)
  (require-symbol "completing-read" "keymap" keymap)
  (require-symbol "completing-read" "input-state" input-state)
  (require-string "completing-read" "buffer-name" buffer-name)
  (when (zero? (string-length buffer-name))
    (error "completing-read: buffer-name must not be empty"))
  (require-arguments "completing-read" arguments)
  (apply interaction 'picker keymap input-state buffer-name prompt initial-input history provider
         (and allow-custom-input? #t) accept-command arguments))
