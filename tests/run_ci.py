#!/usr/bin/env python3
"""CI autograder for the HFT course project (runs in GitHub Actions).

Detects which phase components are present in the student's repo and grades the
objective parts:
  * Phase 3 — SPSC ring buffer  (spsc_ring.hpp) : correctness + concurrency + TSan
  * (offline) latency replay     if scripts/latency_replay.py + a tape are present

Writes:
  * report.json         — machine-readable {score, max_score, tests[], metrics{}}
  * a Markdown summary to $GITHUB_STEP_SUMMARY (shown on the Actions run page)

Runs on the STUDENT's own runner, so no untrusted code touches the instructor.
Grade collection is done separately by scripts/collect_grades.py.
"""
import glob, json, os, subprocess, sys

REPO = os.environ.get("GITHUB_WORKSPACE", os.getcwd())
CXX  = os.environ.get("CXX", "g++")
tests, metrics = [], {}

def add(name, ok, pts, output):
    tests.append({"name": name, "score": pts if ok else 0, "max_score": pts,
                  "status": "pass" if ok else "fail", "output": output})

def sh(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, **kw)
    except subprocess.TimeoutExpired as e:
        import types
        return types.SimpleNamespace(returncode=124, stdout=e.stdout or "",
                                     stderr="timed out (likely an unimplemented/incorrect stub)")

def find(pattern):
    hits = glob.glob(os.path.join(REPO, "**", pattern), recursive=True)
    return hits[0] if hits else None

def parse_results(stdout):
    out = {}
    for line in stdout.splitlines():
        if line.startswith("RESULT|"):
            _, key, verdict, msg = line.split("|", 3)
            out[key] = (verdict == "pass", msg)
    return out

TDIR = os.path.dirname(os.path.abspath(__file__))

# ----- Phase 3: SPSC ring buffer -------------------------------------------------
hdr = find("spsc_ring.hpp")
if hdr:
    inc = os.path.dirname(hdr)
    flags = ["-std=c++17", "-O2", "-pthread", "-I", inc]
    P = {"basic":10,"empty_pop":10,"full":10,"fifo":15,"wrap":15,"concurrent":20,"tsan":10}
    NAME = {"basic":"SPSC basic push/pop/empty","empty_pop":"SPSC pop on empty",
            "full":"SPSC full/capacity","fifo":"SPSC FIFO order","wrap":"SPSC wrap-around",
            "concurrent":"SPSC concurrent correctness","tsan":"SPSC ThreadSanitizer (no races)"}
    # single-threaded correctness
    b1 = os.path.join(REPO, "_spsc_corr")
    c1 = sh([CXX, *flags, os.path.join(TDIR, "spsc_correctness.cpp"), "-o", b1])
    if c1.returncode != 0:
        add("SPSC header compiles", False, 10, c1.stderr[-3000:])
        for k in P: add(NAME[k], False, P[k], "not run — build failed")
    else:
        add("SPSC header compiles", True, 10, "OK")
        pr = parse_results(sh([b1], timeout=120).stdout)
        for k in ["basic","empty_pop","full","fifo","wrap"]:
            ok, msg = pr.get(k, (False, "did not run")); add(NAME[k], ok, P[k], msg)
        # concurrency (normal build)
        b2 = os.path.join(REPO, "_spsc_conc")
        c2 = sh([CXX, *flags, os.path.join(TDIR, "spsc_concurrency.cpp"), "-o", b2])
        if c2.returncode != 0:
            add(NAME["concurrent"], False, P["concurrent"], c2.stderr[-2000:])
        else:
            r2 = sh([b2], timeout=180); pr2 = parse_results(r2.stdout)
            ok, msg = pr2.get("concurrent", (False, "did not run"))
            thr = [l for l in r2.stdout.splitlines() if l.startswith("THROUGHPUT")]
            if thr: metrics["spsc_ops_per_sec"] = int(thr[0].split()[1])
            add(NAME["concurrent"], ok, P["concurrent"], msg)
        # ThreadSanitizer
        b3 = os.path.join(REPO, "_spsc_tsan")
        c3 = sh([CXX, "-std=c++17","-O1","-g","-pthread","-fsanitize=thread",
                 "-I", inc, os.path.join(TDIR,"spsc_concurrency.cpp"), "-o", b3])
        if c3.returncode != 0:
            add(NAME["tsan"], False, P["tsan"], "TSan build failed:\n"+c3.stderr[-1500:])
        else:
            r3 = sh([b3], timeout=240, env=dict(os.environ, SPSC_N="200000"))
            race = "ThreadSanitizer" in (r3.stderr + r3.stdout)
            ok = r3.returncode == 0 and not race
            add(NAME["tsan"], ok, P["tsan"],
                "no data races" if ok else "data race / failure:\n"+r3.stderr[-2000:])
else:
    add("SPSC ring buffer (spsc_ring.hpp)", False, 0,
        "spsc_ring.hpp not found — skipped (only graded once you add it in Phase 3).")

# ----- HFT challenge autograders (auto-detected per repo) ------------------------
CHALLENGES = [
    ("pool.hpp", "pool_test.cpp", {
        "pool_basic": ("Pool: basic alloc", 5), "pool_full": ("Pool: capacity limit", 5),
        "pool_reuse": ("Pool: slot reuse", 5), "pool_pattern": ("Pool: churn pattern", 10)}),
    ("order_book.hpp", "book_test.cpp", {
        "book_bbo": ("Book: best bid/ask + cancel", 12), "symmap": ("SymMap: put/get/absent", 8)}),
    ("fix_parser.hpp", "fix_test.cpp", {
        "fix_parse": ("FIX: parse NewOrder", 15)}),
    ("u64toa.hpp", "u64toa_test.cpp", {
        "u64toa_edges": ("u64toa: edge cases", 8), "u64toa_fuzz": ("u64toa: fuzz vs std", 12)}),
    ("rolling_counter.hpp", "rolling_counter_test.cpp", {
        "rc_window": ("RollingCounter: window count", 10),
        "rc_mixed": ("RollingCounter: expiry as clock advances", 10)}),
    ("shm_ring.hpp", "shm_ring_test.cpp", {
        "shm_spsc": ("Shared-memory SPSC across processes (fork + mmap)", 20)}),
]
def run_challenge(header, driver, mapping):
    hdr = find(header)
    if not hdr:
        return
    inc = os.path.dirname(hdr)
    b = os.path.join(REPO, "_" + driver)
    c = sh([CXX, "-std=c++17", "-O2", "-pthread", "-I", inc, "-I", TDIR,
            os.path.join(TDIR, driver), "-o", b])
    if c.returncode != 0:
        add(header + " compiles", False, 10, c.stderr[-2500:])
        for k, (nm, pts) in mapping.items():
            add(nm, False, pts, "not run — build failed")
        return
    r = sh([b], timeout=180)
    pr = parse_results(r.stdout)
    for k, (nm, pts) in mapping.items():
        ok, msg = pr.get(k, (False, "did not run"))
        add(nm, ok, pts, msg)
    for line in r.stdout.splitlines():
        if line.startswith("METRIC|"):
            _, mk, mv = line.split("|", 2)
            try:
                metrics[mk] = float(mv)
            except ValueError:
                pass
for _h, _d, _m in CHALLENGES:
    run_challenge(_h, _d, _m)

# ----- optional: offline latency replay (reported metric, not pass/fail) ---------
replay = find("latency_replay.py"); report = find("latency_report.py")
cml    = os.path.join(REPO, "hft", "cpp_client", "CMakeLists.txt")
if replay and os.path.exists(cml):
    bd = os.path.join(REPO, "hft", "cpp_client", "build")
    ok_build = sh(["cmake","-S",os.path.dirname(cml),"-B",bd]).returncode == 0 and \
               sh(["cmake","--build",bd]).returncode == 0
    metrics["cpp_client_builds"] = ok_build
    add("C++ client builds (cmake)", ok_build, 10,
        "built build/hft_bot" if ok_build else "cmake build failed")
    # (Latency numbers depend on a committed tape; left as a reported metric.)

# ----- write outputs -------------------------------------------------------------
score = sum(t["score"] for t in tests)
maxs  = sum(t["max_score"] for t in tests)
json.dump({"score": score, "max_score": maxs, "tests": tests, "metrics": metrics},
          open(os.path.join(REPO, "report.json"), "w"), indent=2)

summ = os.environ.get("GITHUB_STEP_SUMMARY")
lines = ["## Autograder — %d / %d\n" % (score, maxs),
         "| Test | Score | Notes |","|---|---|---|"]
for t in tests:
    note = t["output"].splitlines()[0][:80] if t["output"] else ""
    lines.append("| %s | %d/%d | %s |" % (t["name"], t["score"], t["max_score"], note))
if metrics: lines.append("\n**Metrics:** `%s`" % json.dumps(metrics))
text = "\n".join(lines)
if summ:
    open(summ, "a").write(text + "\n")
print(text)
print("\nreport.json written: %d/%d" % (score, maxs))
