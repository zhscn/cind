(define-module (cind ares)
  #:use-module (ice-9 format)
  #:use-module (ice-9 threads)
  #:use-module (srfi srfi-9)
  #:use-module (ares server)
  #:use-module (fibers)
  #:use-module (fibers conditions)
  #:use-module (cind command)
  #:use-module (cind development)
  #:use-module (cind host)
  #:export (ares-command-definitions
            install-ares-documentation!
            host-ares-status
            stop-host-ares!))

(define-record-type <ares-endpoint>
  (%make-ares-endpoint module lock status port port-file thread shutdown error)
  ares-endpoint?
  (module endpoint-module)
  (lock endpoint-lock)
  (status endpoint-status set-endpoint-status!)
  (port endpoint-port set-endpoint-port!)
  (port-file endpoint-port-file set-endpoint-port-file!)
  (thread endpoint-thread set-endpoint-thread!)
  (shutdown endpoint-shutdown set-endpoint-shutdown!)
  (error endpoint-error set-endpoint-error!))

(define endpoints-lock (make-mutex))
(define endpoints (make-weak-key-hash-table))

(define (make-ares-endpoint host)
  (%make-ares-endpoint (make-evaluation-module host) (make-mutex)
                       'stopped #f #f #f #f #f))

(define (endpoint-for host)
  (with-mutex endpoints-lock
    (or (hashq-ref endpoints host)
        (let ((endpoint (make-ares-endpoint host)))
          (hashq-set! endpoints host endpoint)
          endpoint))))

(define (endpoint-snapshot endpoint)
  (with-mutex (endpoint-lock endpoint)
    (vector (endpoint-status endpoint)
            (endpoint-port endpoint)
            (endpoint-port-file endpoint)
            (endpoint-error endpoint))))

(define (live-status? status)
  (and (memq status '(starting running stopping)) #t))

(define (remove-endpoint-port-file! endpoint)
  (let ((path (with-mutex (endpoint-lock endpoint)
                (endpoint-port-file endpoint))))
    (when (and path (file-exists? path))
      (false-if-exception (delete-file path)))))

(define (start-ares-endpoint! endpoint port port-file)
  (unless (and (integer? port) (<= 1 port 65535))
    (error "Ares port must be an integer from 1 through 65535" port))
  (unless (string? port-file)
    (error "Ares port file must be a string" port-file))
  (let ((start? #f))
    (with-mutex (endpoint-lock endpoint)
      (unless (live-status? (endpoint-status endpoint))
        (set! start? #t)
        (set-endpoint-status! endpoint 'starting)
        (set-endpoint-port! endpoint port)
        (set-endpoint-port-file! endpoint port-file)
        (set-endpoint-error! endpoint #f)))
    (when start?
      (let ((started (make-condition))
            (shutdown (make-condition)))
        (with-mutex (endpoint-lock endpoint)
          (set-endpoint-shutdown! endpoint shutdown))
        (let ((thread
               (call-with-new-thread
                (lambda ()
                  (catch #t
                    (lambda ()
                      (save-module-excursion
                       (lambda ()
                         (set-current-module (endpoint-module endpoint))
                         (run-fibers
                          (lambda ()
                            (let ((stop
                                   (run-nrepl-server
                                    #:port port
                                    #:started? started
                                    #:nrepl-port-path port-file
                                    #:standalone? #f)))
                              (wait shutdown)
                              (stop)))
                          #:drain? #f)))
                      (remove-endpoint-port-file! endpoint)
                      (with-mutex (endpoint-lock endpoint)
                        (set-endpoint-status! endpoint 'stopped)
                        (set-endpoint-thread! endpoint #f)
                        (set-endpoint-shutdown! endpoint #f)))
                    (lambda (key . arguments)
                      (remove-endpoint-port-file! endpoint)
                      (with-mutex (endpoint-lock endpoint)
                        (if (eq? (endpoint-status endpoint) 'stopping)
                            (set-endpoint-status! endpoint 'stopped)
                            (begin
                              (set-endpoint-status! endpoint 'failed)
                              (set-endpoint-error!
                               endpoint (format #f "~S: ~S" key arguments))))
                        (set-endpoint-thread! endpoint #f)
                        (set-endpoint-shutdown! endpoint #f))
                      (signal-condition! started)))))))
          (with-mutex (endpoint-lock endpoint)
            (when (eq? (endpoint-status endpoint) 'starting)
              (set-endpoint-thread! endpoint thread)))
          (wait started)
          (with-mutex (endpoint-lock endpoint)
            (when (eq? (endpoint-status endpoint) 'starting)
              (set-endpoint-status! endpoint 'running))))))
    (endpoint-snapshot endpoint)))

(define (stop-ares-endpoint! endpoint)
  (let ((thread #f)
        (shutdown #f))
    (with-mutex (endpoint-lock endpoint)
      (when (live-status? (endpoint-status endpoint))
        (set-endpoint-status! endpoint 'stopping)
        (set! thread (endpoint-thread endpoint))
        (set! shutdown (endpoint-shutdown endpoint))))
    (when shutdown
      (signal-condition! shutdown))
    (when thread
      (false-if-exception (join-thread thread)))
    (remove-endpoint-port-file! endpoint)
    (with-mutex (endpoint-lock endpoint)
      (unless (eq? (endpoint-status endpoint) 'failed)
        (set-endpoint-status! endpoint 'stopped))
      (set-endpoint-thread! endpoint #f)
      (set-endpoint-shutdown! endpoint #f))
    (endpoint-snapshot endpoint)))

(define (host-ares-status host)
  (let ((endpoint (with-mutex endpoints-lock (hashq-ref endpoints host))))
    (if endpoint
        (endpoint-snapshot endpoint)
        (vector 'stopped #f #f #f))))

(define (stop-host-ares! host)
  (let ((endpoint
         (with-mutex endpoints-lock
           (let ((value (hashq-ref endpoints host)))
             (hashq-remove! endpoints host)
             value))))
    (if endpoint
        (stop-ares-endpoint! endpoint)
        (vector 'stopped #f #f #f))))

(define (last-string-argument invocation)
  (let ((arguments (invocation-arguments invocation)))
    (and (> (vector-length arguments) 0)
         (let ((argument (vector-ref arguments (- (vector-length arguments) 1))))
           (and (string? argument) argument)))))

(define (default-port-file host context)
  (let ((project (context-project context)))
    (if project
        (let ((root (project-root host project)))
          (if root
              (string-append (path-as-directory host root) ".nrepl-port")
              ".nrepl-port"))
        ".nrepl-port")))

(define (random-server-port)
  (+ 49152 (random 16384 (random-state-from-platform))))

(define (status-message snapshot)
  (let ((status (vector-ref snapshot 0))
        (port (vector-ref snapshot 1))
        (port-file (vector-ref snapshot 2))
        (error (vector-ref snapshot 3)))
    (cond (error (string-append "Ares failed: " error))
          ((eq? status 'running)
           (format #f "Ares nREPL listening on 127.0.0.1:~A (~A)" port port-file))
          (else (format #f "Ares nREPL is ~A" status)))))

(define (ares-start host context invocation)
  (let* ((port-file (or (last-string-argument invocation)
                        (default-port-file host context)))
         (snapshot (start-ares-endpoint! (endpoint-for host)
                                         (random-server-port) port-file)))
    (if (eq? (vector-ref snapshot 0) 'failed)
        (command-error (status-message snapshot))
        (begin
          (set-message! host (status-message snapshot))
          (command-completed)))))

(define (ares-stop host context invocation)
  (let ((snapshot (stop-host-ares! host)))
    (set-message! host (status-message snapshot))
    (command-completed)))

(define (ares-status host context invocation)
  (set-message! host (status-message (host-ares-status host)))
  (command-completed))

(define (ares-running? host context)
  (live-status? (vector-ref (host-ares-status host) 0)))

(define (ares-command-definitions host)
  (list
   (list "scheme.ares-start"
         (lambda (context invocation) (ares-start host context invocation))
         (lambda (context) (not (ares-running? host context))))
   (list "scheme.ares-stop"
         (lambda (context invocation) (ares-stop host context invocation))
         (lambda (context) (ares-running? host context)))
   (list "scheme.ares-status"
         (lambda (context invocation) (ares-status host context invocation))
         #f)))

(define command-documentation
  '(("scheme.ares-start" .
     "Start the application Ares nREPL endpoint and publish its port file.")
    ("scheme.ares-stop" .
     "Stop the application Ares nREPL endpoint and its active sessions.")
    ("scheme.ares-status" .
     "Report the application Ares nREPL endpoint status.")))

(define (install-ares-documentation! host)
  (for-each (lambda (entry)
              (set-command-documentation! host (car entry) (cdr entry)))
            command-documentation))
