// The balancer from include/proxy/balancer.hpp, compiled to WebAssembly,
// placing simulated connections on simulated backends. Every routing decision,
// every ejection and every retry target comes from the C++; the backends, the
// clients and the clock are simulated here.
import createModule from "./balancer.js";

const $ = (id) => document.getElementById(id);
const reduced = matchMedia("(prefers-reduced-motion: reduce)").matches;

let wasm;
try {
  wasm = await createModule();
} catch (err) {
  $("load-error").hidden = false;
  throw err;
}

const STRATEGIES = ["round-robin", "least-connections", "source-hash"];
const NAMES = ["10.0.0.1:8080", "10.0.0.2:8080", "10.0.0.3:8080", "10.0.0.4:8080"];
const CAPACITY = 16;      // requests a backend works on at once; the rest queue
const QUEUE_LIMIT = 400;  // past this, a backend resets new connections
const BASE_MS = 10;       // mean service time of a healthy backend
const SLOW = 8;           // and of a slow one, as a multiple
const CONNECT_MS = 0.1;   // a successful connect, on average
const REFUSE_MS = 0.3;    // a refused one costs about a round trip
const MAX_ATTEMPTS = 3;
const EJECT_MS = 5000;
const CLIENTS = 500;      // distinct client addresses, for source hash

const int = (n) => Math.round(n).toLocaleString("en-US");
const expo = (mean) => -Math.log(1 - Math.random()) * mean;
const hash32 = (n) => {
  n = Math.imul(n ^ (n >>> 16), 0x45d9f3b);
  n = Math.imul(n ^ (n >>> 16), 0x45d9f3b);
  return (n ^ (n >>> 16)) >>> 0;
};

// ── The simulated world ───────────────────────────────────────────────────

// A min-heap of timed events, for the comparison run in simulated time.
class Heap {
  constructor() { this.a = []; }
  get size() { return this.a.length; }
  push(t, v) {
    const a = this.a;
    a.push([t, v]);
    for (let i = a.length - 1; i > 0; ) {
      const p = (i - 1) >> 1;
      if (a[p][0] <= a[i][0]) break;
      [a[p], a[i]] = [a[i], a[p]];
      i = p;
    }
  }
  peek() { return this.a[0]; }
  pop() {
    const a = this.a;
    const top = a[0];
    const last = a.pop();
    if (a.length) {
      a[0] = last;
      for (let i = 0; ; ) {
        let m = i;
        const l = 2 * i + 1, r = l + 1;
        if (l < a.length && a[l][0] < a[m][0]) m = l;
        if (r < a.length && a[r][0] < a[m][0]) m = r;
        if (m === i) break;
        [a[m], a[i]] = [a[i], a[m]];
        i = m;
      }
    }
    return top;
  }
}

class World {
  constructor(strategy, onDone) {
    this.handle = wasm._create(NAMES.length, strategy, 1, EJECT_MS);
    this.backends = NAMES.map((name) => ({ name, down: false, slow: false, busy: 0, queue: [], ejectedAt: -Infinity }));
    this.events = new Heap();
    this.onDone = onDone;
    this.retries = 0;
    this.errors = 0;
  }

  // A client connects at time `now`. Returns nothing; completion is an event.
  arrive(now) {
    const client = Math.floor(Math.random() * CLIENTS);
    const conn = { client, start: now, attempts: 0, tried: 0, backend: -1, connectMs: 0 };
    this.attempt(conn, now);
  }

  attempt(conn, now) {
    const w = wasm;
    while (conn.attempts < MAX_ATTEMPTS) {
      const b = w._pick(this.handle, hash32(conn.client + 1), now, conn.tried);
      if (b < 0) break;
      conn.attempts++;
      conn.tried += 2 ** b;
      if (conn.attempts > 1) this.retries++;
      const backend = this.backends[b];
      if (backend.down) {  // connection refused: charge it, move on
        const wasAvailable = w._available(this.handle, b, now);
        w._failed(this.handle, b, now);
        w._release(this.handle, b);
        if (wasAvailable && !w._available(this.handle, b, now)) backend.ejectedAt = now;
        now += REFUSE_MS;
        continue;
      }
      if (backend.queue.length >= QUEUE_LIMIT) {  // the backend's accept queue overflowed
        w._release(this.handle, b);
        conn.backend = b;
        return this.finish(conn, now, "backend_reset");
      }
      w._connected(this.handle, b);
      now += expo(CONNECT_MS);
      conn.backend = b;
      conn.connectMs = now - conn.start;
      if (backend.busy < CAPACITY) this.serve(backend, conn, now);
      else backend.queue.push(conn);
      return;
    }
    this.finish(conn, now, "connect_failed");
  }

  serve(backend, conn, now) {
    backend.busy++;
    this.events.push(now + expo(BASE_MS * (backend.slow ? SLOW : 1)), conn);
  }

  // Runs every completion due by `now`.
  advance(now) {
    while (this.events.size && this.events.peek()[0] <= now) {
      const [t, conn] = this.events.pop();
      const backend = this.backends[conn.backend];
      backend.busy--;
      wasm._release(this.handle, conn.backend);
      this.finish(conn, t, "done");
      if (backend.queue.length) this.serve(backend, backend.queue.shift(), t);
    }
  }

  finish(conn, t, reason) {
    if (reason !== "done") this.errors++;
    this.onDone(conn, t, reason);
  }

  setDown(b, down, now) {
    const backend = this.backends[b];
    backend.down = down;
    if (!down) return;
    // Its open connections are reset, like a killed process's.
    const killed = backend.queue.splice(0);
    const keep = new Heap();
    for (const [t, conn] of this.events.a) {
      if (conn.backend === b) killed.push(conn);
      else keep.push(t, conn);
    }
    this.events = keep;
    for (const conn of killed) {
      wasm._release(this.handle, b);
      this.finish(conn, now, "backend_reset");
    }
    backend.busy = 0;
  }
}

// ── Live view ─────────────────────────────────────────────────────────────

let strategy = 1;
let rate = 1200;  // new connections a second
let world;
let clock = 0;  // simulated ms, advanced with the wall clock
let recent = [];  // completed connections in the last two seconds
let tapeAt = 0;
const tape = [];

function reset() {
  const old = world;
  world = new World(strategy, record);
  if (old) old.backends.forEach((b, i) => { world.backends[i].down = b.down; world.backends[i].slow = b.slow; });
  recent = [];
  tape.length = 0;
}

function record(conn, t, reason) {
  recent.push({ t, ms: t - conn.start, backend: conn.backend, reason, attempts: conn.attempts });
  if (t - tapeAt > 200 || conn.attempts > 1 || reason !== "done") {
    tapeAt = t;
    tape.unshift({ conn, t, reason });
    if (tape.length > 9) tape.pop();
  }
}

const rows = NAMES.map((name, b) => {
  const tr = document.createElement("tr");
  tr.innerHTML = `
    <td class="name"><span class="dot"></span>${name}</td>
    <td class="state"></td>
    <td class="load"><span class="bar"><span class="busy"></span><span class="queued"></span></span><span class="n"></span></td>
    <td class="num share"></td>
    <td class="num p50"></td>
    <td class="num p99"></td>
    <td class="acts">
      <button type="button" class="slow" aria-pressed="false">Slow</button>
      <button type="button" class="kill" aria-pressed="false">Kill</button>
    </td>`;
  tr.querySelector(".slow").addEventListener("click", (e) => {
    const backend = world.backends[b];
    backend.slow = !backend.slow;
    e.currentTarget.setAttribute("aria-pressed", String(backend.slow));
  });
  tr.querySelector(".kill").addEventListener("click", (e) => {
    const down = !world.backends[b].down;
    world.setDown(b, down, clock);
    e.currentTarget.setAttribute("aria-pressed", String(down));
    e.currentTarget.textContent = down ? "Restart" : "Kill";
  });
  $("backends").append(tr);
  return tr;
});

for (const button of document.querySelectorAll("[data-strategy]")) {
  button.addEventListener("click", () => {
    strategy = Number(button.dataset.strategy);
    for (const b of document.querySelectorAll("[data-strategy]")) b.setAttribute("aria-pressed", String(b === button));
    reset();
  });
}
$("rate").addEventListener("input", (e) => { rate = Number(e.target.value); $("rate-out").textContent = int(rate); });

const pct = (sorted, q) => (sorted.length ? sorted[Math.min(sorted.length - 1, Math.floor(q * sorted.length))] : NaN);
const fmtMs = (ms) => (Number.isNaN(ms) ? "—" : ms < 10 ? ms.toFixed(1) : int(ms));

function paint() {
  recent = recent.filter((r) => clock - r.t < 2000);
  const ok = recent.filter((r) => r.reason === "done");
  const all = ok.map((r) => r.ms).sort((a, b) => a - b);
  $("t-rate").textContent = int(ok.length / 2);
  $("t-p50").textContent = fmtMs(pct(all, 0.5));
  $("t-p99").textContent = fmtMs(pct(all, 0.99));
  $("t-errors").textContent = int(world.errors);
  $("t-retries").textContent = int(world.retries);

  rows.forEach((tr, b) => {
    const backend = world.backends[b];
    const mine = ok.filter((r) => r.backend === b).map((r) => r.ms).sort((x, y) => x - y);
    const ejected = !wasm._available(world.handle, b, clock);
    const state = backend.down ? (ejected ? "down · ejected" : "down") : ejected ? "ejected" : backend.slow ? "slow" : "up";
    tr.className = backend.down ? "down" : backend.slow ? "slow" : "";
    tr.querySelector(".state").textContent = state;
    const active = wasm._active(world.handle, b);
    const scale = 3 * CAPACITY;
    tr.querySelector(".busy").style.width = `${Math.min(100, (backend.busy / scale) * 100)}%`;
    tr.querySelector(".queued").style.width = `${Math.min(100 - Math.min(100, (backend.busy / scale) * 100), (backend.queue.length / scale) * 100)}%`;
    tr.querySelector(".n").textContent = active;
    tr.querySelector(".share").textContent = ok.length ? `${Math.round((mine.length / ok.length) * 100)}%` : "—";
    tr.querySelector(".p50").textContent = fmtMs(pct(mine, 0.5));
    tr.querySelector(".p99").textContent = fmtMs(pct(mine, 0.99));
  });

  $("tape").innerHTML = tape
    .map(({ conn, t, reason }) => {
      const ip = `203.0.113.${(conn.client % 250) + 1}`;
      const backend = conn.backend >= 0 ? NAMES[conn.backend] : "";
      const cls = reason !== "done" ? "bad" : conn.attempts > 1 ? "retry" : "";
      return `<li class="${cls}">{"client":"${ip}","backend":"${backend}","attempts":${conn.attempts},` +
        `"connect_us":${Math.round(conn.connectMs * 1000)},"duration_us":${Math.round((t - conn.start) * 1000)},"close":"${reason}"}</li>`;
    })
    .join("");
}

let last = performance.now();
let nextArrival = 0;
function frame(now) {
  const dt = Math.min(100, now - last);  // a background tab should not replay a minute at once
  last = now;
  const until = clock + dt;
  while (nextArrival < until) {
    world.advance(nextArrival);
    world.arrive(nextArrival);
    nextArrival += expo(1000 / rate);
  }
  clock = until;
  world.advance(clock);
  paint();
  requestAnimationFrame(frame);
}

reset();
if (reduced) rate = 400;
$("rate").value = String(rate);
$("rate-out").textContent = int(rate);
requestAnimationFrame(frame);

// ── The comparison, in simulated time ─────────────────────────────────────

function simulate(strategyIndex, scenario, connections = 40000, perSecond = 1200) {
  const results = [];
  const w = new World(strategyIndex, (conn, t, reason) => results.push({ ms: t - conn.start, reason, backend: conn.backend, attempts: conn.attempts }));
  if (scenario === "slow") w.backends[3].slow = true;
  let t = 0;
  for (let i = 0; i < connections; i++) {
    t += expo(1000 / perSecond);
    if (scenario === "dies" && i === connections / 4) w.setDown(3, true, t);
    w.advance(t);
    w.arrive(t);
  }
  w.advance(Infinity);
  const ok = results.filter((r) => r.reason === "done").map((r) => r.ms).sort((a, b) => a - b);
  return {
    p50: pct(ok, 0.5),
    p99: pct(ok, 0.99),
    toFourth: results.filter((r) => r.backend === 3 && r.reason === "done").length / Math.max(1, ok.length),
    errors: results.length - ok.length,
    retries: w.retries,
  };
}

$("run-compare").addEventListener("click", async () => {
  const button = $("run-compare");
  button.disabled = true;
  const body = $("compare-table").querySelector("tbody");
  body.innerHTML = "";
  $("compare-table").hidden = false;
  for (const [scenario, label] of [["slow", "Backend 4 is 8× slower"], ["dies", "Backend 4 dies a quarter of the way in"]]) {
    for (const s of [0, 1, 2]) {
      $("compare-status").textContent = `Simulating ${STRATEGIES[s]}: ${label.toLowerCase()}…`;
      await new Promise((r) => setTimeout(r, 20));  // let the status paint
      const r = simulate(s, scenario);
      const tr = document.createElement("tr");
      tr.innerHTML = `<td>${label}</td><td>${STRATEGIES[s]}</td><td>${fmtMs(r.p50)} ms</td><td>${fmtMs(r.p99)} ms</td>` +
        `<td>${Math.round(r.toFourth * 100)}%</td><td>${int(r.retries)}</td><td>${int(r.errors)}</td>`;
      body.append(tr);
    }
  }
  $("compare-status").textContent = "40,000 connections per row at 1,200 a second, in simulated time.";
  button.disabled = false;
});
