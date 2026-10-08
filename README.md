# TCP Proxy

A multithreaded Layer 4 TCP proxy and load balancer for Linux, in C++20 with no
dependencies outside the standard library. Each worker thread runs its own
epoll event loop. Connections are spread across backends by round robin,
least connections or source hash. A backend that refuses a connect is taken
out of rotation, and that client is retried on another backend before it has
lost anything. Every connection leaves one line of JSON telemetry when it
closes.

**[Live demo →](https://raj-rathod-tcp-proxy.vercel.app)** A browser has no
sockets, so the demo compiles the proxy's balancer to WebAssembly and gives it
simulated backends: slow one down or kill one and watch the routing react.

```
cmake -S . -B build && cmake --build build && ctest --test-dir build

build/echo-server --listen 127.0.0.1:9001 &
build/echo-server --listen 127.0.0.1:9002 &
build/tcp-proxy --listen 0.0.0.0:8080 --backend 127.0.0.1:9001 --backend 127.0.0.1:9002 \
                --strategy least-connections --log -
build/loadgen --target 127.0.0.1:8080 --mode rr --connections 64 --seconds 5
```

Or with a config file, which also allows several listeners, each routed to its
own pool: see [`proxy.conf.example`](proxy.conf.example).

```
{"start":"2026-10-08T05:16:10.019Z","worker":0,"route":"0.0.0.0:8080","client":"127.0.0.1:55465",
 "backend":"127.0.0.1:9001","attempts":1,"connect_us":102,"duration_us":15274,
 "bytes_up":4000000,"bytes_down":4000000,"reads":504,"writes":505,"close":"done"}
```

---

## Results

All on loopback in a Linux 6.12 VM (Docker Desktop) on an Apple M4 Pro, with
the backends, the proxy and the load generator pinned to separate CPUs, at
most eight busy at once, since the host has eight performance cores. Every
number is the median of three five-second runs.
[`scripts/bench.sh`](scripts/bench.sh) reproduces them all, and
[`results/`](results/) holds the raw output.

### Where the time goes

| 64-byte echo | requests/s | p50 | p90 | p99 |
|---|---:|---:|---:|---:|
| 1 connection, direct | 35,989 | 26.6 µs | 31.4 µs | 43.9 µs |
| 1 connection, through the proxy | 17,577 | 54.1 µs | 63.4 µs | 95.1 µs |
| 256 connections, direct | 971,697 | 239 µs | 383 µs | 547 µs |
| 256 connections, 1 worker | 232,599 | 1,014 µs | 1,552 µs | 1,836 µs |
| 256 connections, 2 workers | 267,648 | 948 µs | 1,133 µs | 1,262 µs |
| 256 connections, 3 workers | 436,609 | 585 µs | 677 µs | 769 µs |

The proxy adds about 28 µs to a round trip: two more hops through loopback.

One worker relays about 250,000 requests a second and spends **3.9 µs of CPU
on each, 0.4 µs of it in user space and 3.5 µs in the kernel** (from
`/proc/<pid>/stat` across the run). Under `strace` the steady state is exactly
what the design intends: one `read` and one `send` per message per direction,
no `epoll_ctl` calls at all, since interest only changes when a buffer fills
or drains, and about 255 ready sockets per `epoll_wait`, so the event loop
costs almost nothing per request. What remains is the TCP stack. A faster
proxy for small messages needs more cores or fewer system calls, not
tighter user-space code.

Throughput rises with workers, but unevenly here. Two workers spent twice
the CPU per request that one did, and a count of context switches showed they
were never idle. That leaves two explanations I could not tell apart in this
setup: contention in the kernel, or the VM placing vCPUs on slower cores, since
it shares eight performance cores between all three programs. I would not read
a scaling factor off this table.

### Bulk throughput is set by system calls per byte

| buffer per direction | throughput | system calls per MB |
|---|---:|---:|
| 4 KiB | 4.5 Gbit/s | 501 |
| **16 KiB** (default) | **24.0 Gbit/s** | 125 |
| 64 KiB | 24.9 Gbit/s | 31 |
| 256 KiB | 32.7 Gbit/s | 8 |

Eight connections streaming 64 KiB writes through three workers, with
system calls counted from the proxy's own telemetry (`reads` and `writes` per
connection). Going from 4 KiB to 16 KiB cuts the calls by four and
multiplies throughput by five. Past that, copying dominates and each step
buys less. The default is 16 KiB because the buffer is allocated twice for
every open connection: 32 KiB each, against 512 KiB at 256 KiB.
`splice(2)` would remove the copy as well; it is the next thing to try.

### One slow backend

| strategy | requests/s | p50 | p90 | p99 |
|---|---:|---:|---:|---:|
| round robin | 6,376 | 0.76 ms | 21.7 ms | 24.5 ms |
| **least connections** | **94,039** | 0.21 ms | 0.32 ms | 20.6 ms |

Three backends, one replying 20 ms late (`echo-server --delay-ms 20`), and a
new connection for every request, 48 in flight. Round robin gives the slow
backend a third of the requests regardless, so every client spends most of its
time waiting on it. Least connections sees the slow backend's open
connections pile up and sends nearly everything to the other two. That is 15
times the throughput. It still sends some, which is the 20 ms p99.

### A backend dies

| retries | ejection | failed requests | failed connects | requests/s |
|---|---|---:|---:|---:|
| none | off | 273,434 | 273,434 | 84,822 |
| 2 | off | 6 | 221,436 | 97,609 |
| 2 | 5 s | **10** | **11** | 102,294 |

One of three backends is killed 3 s into a 10 s run, with a new connection
per request. Without retries, every connection sent to the dead backend fails.
With retries, a refused connect is tried on another backend. The client's
first bytes are still in the proxy's buffer at that point, so nothing is
lost, but the dead backend is still tried first a third of the time. Ejection
takes it out after its first failure, and after that only one probe every
five seconds is spent on it. The few failures left are requests that were
mid-flight on the backend when it died: their bytes had already been
forwarded, and a Layer 4 proxy cannot replay a stream it has passed on.

Opening a fresh connection for every request, through three workers, the
proxy sustained **103,494 connections a second** with a p99 of 0.92 ms. I have
left out a direct baseline for that test: in this VM it showed
multi-millisecond client-side connect stalls that I did not track down, so I
do not trust it as a comparison.

---

## Design

```
include/proxy/
  poller.hpp     epoll (Linux) and kqueue (macOS) behind one level-triggered interface
  net.hpp        addresses, listening, non-blocking connects, accept4
  buffer.hpp     one direction of a connection: a flat array with read and write offsets
  balancer.hpp   strategies and passive health checks; no sockets, so it also builds to WebAssembly
  config.hpp     the configuration file
  telemetry.hpp  the per-connection JSON record
  proxy.hpp      workers, connections, the event loop
tools/
  echo_server.cpp  a backend for load tests, optionally slow (--delay-ms) or a sink (--sink)
  loadgen.cpp      closed-loop load: request/response, a connection per request, or streams
```

**One event loop per worker, nothing shared on the data path.** Every
listening socket is registered with every worker's epoll set with
`EPOLLEXCLUSIVE`, so a new connection wakes one worker rather than all. That
worker owns the connection for its whole life, so reads, writes and buffers
take no locks. The state shared between workers is the balancer for each pool,
which is atomic counters, one cache line per backend, and some totals.

**Backpressure falls out of interest management.** Each connection has two
buffers, client-to-backend and backend-to-client, and a socket is only
watched for reading while the buffer it fills has room. A slow reader
therefore stops the proxy reading from the writer, and TCP flow control
carries that back to the sender. Memory per connection is bounded at twice the
buffer size, however fast either side is.

**Writes are eager.** After a read, the proxy writes the bytes on immediately,
without waiting for epoll to report the other socket writable, which it almost
always is. Write interest is only registered when a write comes up short.
This is why the steady state above has no `epoll_ctl` calls and no wasted
reads.

**Half-close is relayed, not collapsed.** When one side finishes sending, the
proxy delivers what is buffered and then passes the EOF on with
`shutdown(SHUT_WR)`, leaving the other direction open. A client that sends a
request and closes its write side still gets its reply. The connection is
closed when both directions have finished.

**Retries happen before anything is lost.** The client's socket is read
while the backend connect is in flight, and those bytes stay in the buffer
until the connect succeeds. A refused, unreachable or timed-out connect can
therefore be retried on a different backend (`retries`, default 2), up to
every backend in the pool, with the client none the wiser. A connect failure
also counts against the backend. After `eject_after` consecutive failures
(default 1) it is skipped for `eject_for_s` seconds (default 5). If every
backend is ejected, one is tried anyway: refusing the client is certain to
fail, and a retry might not.

**The strategies.** Round robin rotates over the backends that are eligible
right now. Rotating over all of them and skipping the ejected ones would
send an ejected backend's whole share to its neighbour; a unit test caught
that. Least connections picks the backend with the fewest open connections,
breaking ties from a rotating start so that an idle pool still spreads
evenly. Source hash uses rendezvous hashing on the client's IP: each client
goes to the backend with the highest `hash(client, backend)`. When a backend
leaves, only its own clients move.

**Timeouts are a sweep.** Ten times a second each worker walks its connections
for connect and idle timeouts. That is O(connections) per sweep, which is
nothing at tens of thousands of connections. A timer wheel would be the change
if a worker ever held hundreds of thousands of idle ones.

**Shutdown drains.** SIGINT or SIGTERM stops accepting, gives open connections
`drain_s` seconds to finish, then closes the rest and logs them as
`shutdown`. Running out of file descriptors pauses accepting until the next
sweep, instead of spinning on a listener that stays readable.

**Telemetry is batched.** Each worker appends a connection's record to a
buffer when it closes and writes the whole buffer once per loop iteration, so
logging costs one `write` for many connections rather than one each.

## Tests

`ctest` runs 36 tests. The unit tests cover the balancer, including
round-robin fairness while a backend is ejected, least connections routing
around a backend that never finishes, rendezvous hashing moving only the
failed backend's clients, ejection and recovery, and concurrent picks from
eight threads; plus the buffer, the config parser's errors and the telemetry
format. The end-to-end tests run the real proxy on loopback in front of small
blocking test servers that share no code with it:
- 4 MiB relayed byte for byte in both directions;
- 64 clients at once;
- half-close in each direction;
- retry past a refusing backend;
- a client closed when no backend answers;
- round-robin and least-connections distribution;
- idle timeout;
- drain on stop;
- the telemetry line itself.

CI runs them on Linux and macOS, with GCC and Clang, in Debug and Release,
and once more under AddressSanitizer and UndefinedBehaviorSanitizer, followed
by a short load test through the built proxy.

## License

MIT
