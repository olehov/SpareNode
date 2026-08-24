# Connection server

`ConnectionServer` integrates the cancellable TCP listener with the bounded
connection dispatcher. It is the asynchronous connection-lifecycle boundary for
SpareNode; protocol parsing remains the responsibility of a supplied handler.

## Concurrency model

The server owns exactly one accept thread. Every accepted `TcpConnection` is
transferred into the dispatcher's bounded queue and processed by its fixed
`std::jthread` worker pool. The number of threads therefore depends on configured
server capacity, not on the number of connected clients.

`ConnectionServerConfig::multithreading_enabled` is the runtime switch populated
from `ApplicationConfig::multithreading_enabled()`. When it is `false`, the server
forces the dispatcher to one worker. When it is `true`, the explicit configured
worker count is used. The setting defaults to `false`, so additional worker
threads require an explicit opt-in. This keeps resource capacity deterministic
while providing the requested `.env` on/off control.

A slow connection can occupy one worker without blocking the accept loop or other
workers. When the bounded queue is full, the accept thread waits without polling,
which applies backpressure instead of allocating an unbounded queue.

## Startup

`ConnectionServer::start()` performs startup in this order:

1. bind and start the listener;
2. cache the effective local endpoint;
3. validate and create the bounded dispatcher; and
4. start the cancellable accept thread.

Failure at any stage releases resources already acquired by earlier stages.
`ConnectionServerStartError` distinguishes listener, dispatcher, thread-start,
and allocation failures without throwing them through the public factory.

## Shutdown

`request_stop()` first requests cancellation of the accept thread and then stops
the dispatcher. This wakes a blocked `accept()`, cancels a blocked queue
submission, releases queued connections, and requests every active handler's stop
token. The destructor joins the accept thread and dispatcher workers before their
shared state is released.

Handlers must propagate their stop token into cancellable connection operations.
The server, like the dispatcher, must not be destroyed or move-assigned from one
of its own callbacks because a managed thread cannot join itself.

## Failure isolation

Connection-handler errors and exceptions are isolated by `ConnectionDispatcher`
and reported through its `ConnectionFailureObserver`. Listener and queue-boundary
errors are reported separately through `ConnectionServerFailureObserver`.
Unexpected exceptions from the accept loop are reported as `internal_exception`.
A fatal accept-loop failure also stops the dispatcher so active handlers do not
remain alive behind a server that can no longer accept clients. Observer
exceptions are contained at their respective thread boundaries.
