# Bounded connection dispatcher

`sparenode::network::ConnectionDispatcher` transfers accepted TCP connections
through a fixed worker pool. It provides the scheduling boundary used by the
future server accept loop without assigning a permanent thread to every client.

## Resource model

Creation requires two explicit positive limits:

- `worker_count` fixes the number of long-lived `std::jthread` workers;
- `pending_connection_limit` fixes the number of accepted connections that may
  wait for a worker.

The pending queue is allocated once during dispatcher creation and implemented
as a ring buffer. A successful `submit()` therefore does not grow the queue or
allocate storage based on connection load. Each queued or active
`TcpConnection` always has one owner.

## Submission policy

`submit()` transfers an open connection only after a queue slot is available.
When the queue is full, the producer waits without polling. The wait ends when:

1. a worker frees capacity and the connection is accepted;
2. the caller's `std::stop_token` is requested and `cancelled` is returned; or
3. dispatcher shutdown begins and `stopped` is returned.

A stop request that already exists when `submit()` starts always returns
`cancelled`. On every failed submission, the by-value connection is destroyed
by the call and its socket is closed through RAII.

## Handler contract

The configured `ConnectionHandler` receives exclusive ownership of one
connection and the owning worker's stop token. The same handler object may be
called concurrently by several workers, so any captured shared state must be
synchronized.

Normal transport failures are returned as `Result<void, NetworkError>`. The
dispatcher isolates the failure from other clients and forwards it to the
optional `ConnectionFailureObserver`. Unexpected handler exceptions are also
contained and reported as `handler_exception`; observer exceptions are
contained at the worker boundary. An observer may run concurrently on several
workers and should avoid long blocking operations.

## Shutdown

`request_stop()` is idempotent and non-blocking. It performs four actions:

1. prevents all later submissions;
2. wakes producers blocked by a full queue;
3. closes pending connections through RAII; and
4. requests every worker token so cancellable connection operations can return.

The destructor calls `request_stop()` and then joins all workers. Consequently,
a handler must observe its stop token during blocking work; otherwise destruction
must wait for that handler to return. The dispatcher must not be destroyed from
inside one of its own callbacks because a worker cannot join itself.
