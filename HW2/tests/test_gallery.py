#!/usr/bin/env python3
"""Проверка внешнего поведения: восстановление модели из журнала событий."""
import collections
import json
import os
from pathlib import Path
import random
import re
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
RESULTS = []


def parse(text):
    return [dict(re.findall(r"\b([a-z_]+)=([^\s]+)", line)) for line in text.splitlines()]


def verify(text, interrupted=False, expected_status=None):
    rows = parse(text)
    assert rows and rows[0]["event"] == "START"
    n, pcount, capacity = (int(rows[0][k]) for k in ("visitors", "paintings", "capacity"))
    limits, viewers, seen, current, chosen, plans = {}, collections.Counter(), {}, {}, {}, {}
    queue = collections.deque()
    arrived, inside, reserved, exited, cancelled, completed = set(), set(), set(), set(), set(), set()
    peaks = collections.Counter()
    picture_stats = {}
    peak_inside = peak_committed = peak_queue = 0
    entry_closed = False
    for seq, row in enumerate(rows, 1):
        assert int(row["seq"]) == seq
        event, v, p = row["event"], int(row["visitor"]), int(row["picture"])
        if event == "PICTURE_CONFIG":
            limits[p] = int(row["limit"])
        elif event == "PLAN":
            route = list(map(int, row["order"].split(",")))
            assert sorted(route) == list(range(1, pcount + 1))
            if rows[0]["strategy"] == "sequential":
                assert route == list(range(1, pcount + 1))
            plans[v] = route
        elif event == "ARRIVE":
            assert v not in arrived and not entry_closed
            arrived.add(v)
            seen[v] = set()
        elif event == "WAIT_ENTRY":
            assert v in arrived and v not in queue
            queue.append(v)
        elif event == "PERMIT":
            assert not entry_closed and queue.popleft() == v
            reserved.add(v)
        elif event == "ENTER":
            assert v in reserved
            reserved.remove(v)
            inside.add(v)
        elif event == "CHOOSE":
            assert v in inside and v not in current and p not in seen[v]
            assert p == plans[v][len(seen[v])]
            chosen[v] = p
        elif event == "WAIT_PICTURE":
            assert v in inside and chosen[v] == p
        elif event == "VIEW_BEGIN":
            assert v in inside and v not in current and p not in seen[v] and chosen[v] == p
            current[v] = p
            viewers[p] += 1
            peaks[p] = max(peaks[p], viewers[p])
            assert viewers[p] <= limits[p]
            assert int(row["viewers"]) == viewers[p]
        elif event in ("VIEW_END", "VIEW_ABORT"):
            assert current.pop(v) == p
            viewers[p] -= 1
            assert int(row["viewers"]) == viewers[p]
            if event == "VIEW_END":
                assert p not in seen[v]
                seen[v].add(p)
                assert int(row["viewed"]) == len(seen[v])
            else:
                assert interrupted
        elif event == "EXIT":
            assert v in inside and v not in current
            inside.remove(v)
            exited.add(v)
            assert int(row["viewed"]) == len(seen[v])
            if row["complete"] == "1":
                assert len(seen[v]) == pcount
                completed.add(v)
            else:
                assert interrupted
                cancelled.add(v)
        elif event == "CANCEL_ENTRY":
            assert interrupted
            if v in reserved:
                reserved.remove(v)
            else:
                queue.remove(v)
            cancelled.add(v)
        elif event == "ENTRY_CLOSED":
            assert not entry_closed
            entry_closed = True
        elif event == "PICTURE_STATS":
            picture_stats[p] = row
        assert int(row["inside"]) == len(inside), row
        assert int(row["reserved"]) == len(reserved), row
        assert int(row["queue"]) == len(queue), row
        assert len(inside) + len(reserved) <= capacity
        peak_inside = max(peak_inside, len(inside))
        peak_committed = max(peak_committed, len(inside) + len(reserved))
        peak_queue = max(peak_queue, len(queue))
    assert not inside and not reserved and not queue and not current and not +viewers
    assert entry_closed
    summary = rows[-1]
    assert summary["event"] == "SUMMARY"
    assert summary["status"] == (expected_status or ("interrupted" if interrupted else "completed"))
    assert int(summary["completed"]) == len(completed)
    assert int(summary["cancelled"]) == len(cancelled)
    assert int(summary["arrived"]) == len(arrived) == len(completed) + len(cancelled)
    assert int(summary["entered"]) == int(summary["exited"]) == len(exited)
    assert int(summary["not_arrived"]) == n - len(arrived)
    assert int(summary["inspections"]) == sum(map(len, seen.values()))
    assert int(summary["peak_inside"]) == peak_inside
    assert int(summary["peak_committed"]) == peak_committed
    assert int(summary["peak_queue"]) == peak_queue
    for p in range(1, pcount + 1):
        assert int(picture_stats[p]["views"]) == sum(p in s for s in seen.values())
        assert int(picture_stats[p]["peak"]) == peaks[p]
    if not interrupted:
        assert len(completed) == n and int(summary["inspections"]) == n * pcount
    return summary, rows


def run(binary, name, args, *, expected=0, verify_log=True):
    proc = subprocess.run([str(binary), *args], cwd=ROOT, capture_output=True, text=True, timeout=30)
    assert proc.returncode == expected, (name, proc.returncode, proc.stderr, proc.stdout[-2000:])
    summary = {}
    if verify_log:
        summary, _ = verify(proc.stdout)
    RESULTS.append({"binary": binary.name, "test": name, "result": "PASS", "summary": summary})
    return proc


def interrupt(binary, name, args, target, sig, predicate=None):
    proc = subprocess.Popen([str(binary), *args, "--log", "none"], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    prefix = []
    # Каждое событие пишется сразу через write(), без буфера stdio.
    # Внешний alarm ограничивает также ожидание строки при регрессии.
    old_handler = signal.signal(signal.SIGALRM, lambda *_: (_ for _ in ()).throw(TimeoutError(name)))
    signal.alarm(15)
    try:
        while True:
            line = proc.stdout.readline()
            assert line, (name, "process ended before target", proc.poll())
            prefix.append(line)
            if f"event={target} " in line and (predicate is None or predicate(parse(line)[0])):
                proc.send_signal(sig)
                break
        # Не смешиваем TextIOWrapper.readline() и communicate(): последнее
        # может пропустить уже прочитанные во внутренний буфер строки.
        rest = proc.stdout.read()
        err = proc.stderr.read()
        proc.wait(timeout=10)
        assert proc.returncode == 128 + sig, (name, proc.returncode, err)
        summary, _ = verify("".join(prefix) + rest, interrupted=True)
        RESULTS.append({"binary": binary.name, "test": name, "result": "PASS", "summary": summary})
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, old_handler)
        if proc.poll() is None:
            proc.kill()
            proc.communicate()


def suite(binary):
    fast = ["--arrival-min", "0", "--arrival-max", "0", "--view-min", "0", "--view-max", "0", "--log", "none"]
    run(binary, "default_200", ["--log", "none"])
    run(binary, "empty", [*fast, "--visitors", "0"])
    run(binary, "one_visitor_one_picture", [*fast, "--visitors", "1", "--paintings", "1", "--capacity", "1", "--picture-capacity", "1"])
    run(binary, "100_pictures", [*fast, "--visitors", "3", "--paintings", "100", "--seed", "4294967295"])
    run(binary, "bottleneck_100", ["--config", "examples/bottleneck.conf", "--log", "none"])
    run(binary, "capacity_one_300", [*fast, "--visitors", "300", "--capacity", "1", "--picture-capacity", "1"])
    run(binary, "capacity_exceeds_visitors", [*fast, "--visitors", "2", "--capacity", "1000", "--picture-capacity", "1000"])
    rng = random.Random(16)
    for i in range(12):
        paintings = rng.randint(1, 8)
        run(binary, f"random_config_{i+1:02}", [*fast, "--visitors", str(rng.randint(20, 100)),
            "--paintings", str(paintings), "--capacity", str(rng.randint(1, 20)),
            "--picture-capacity", ",".join(str(rng.randint(1, 5)) for _ in range(paintings)),
            "--seed", str(rng.getrandbits(32)), "--strategy", rng.choice(["random", "sequential"])])
    a = run(binary, "seed_replay_a", [*fast, "--visitors", "20", "--seed", "0"]).stdout
    b = run(binary, "seed_replay_b", [*fast, "--visitors", "20", "--seed", "0"]).stdout
    def plans(text):
        return [(r["visitor"], r["arrival_ms"], r["order"], r["durations"]) for r in parse(text) if r["event"] == "PLAN"]
    assert plans(a) == plans(b)
    with tempfile.TemporaryDirectory() as td:
        log = Path(td) / "events.log"
        proc = run(binary, "log_matches_stdout", [*fast, "--visitors", "10", "--log", str(log)])
        assert log.read_text() == proc.stdout
        proc = run(binary, "quiet_full_log", [*fast, "--visitors", "10", "--quiet", "--log", str(log)], verify_log=False)
        verify(log.read_text())
        assert all(r["event"] in ("SUMMARY", "PICTURE_STATS") for r in parse(proc.stdout))
        cfg = Path(td) / "custom.conf"
        cfg.write_text("# comment\nvisitors = 5\npaintings=2\npicture-capacity=1,3\n")
        proc = run(binary, "cli_overrides_config", [*fast, "--visitors", "7", "--config", str(cfg)])
        assert int(parse(proc.stdout)[-1]["planned"]) == 7
        for content in ("unknown=3", "capacity=0", "oops", "quiet=2", "visitors=1\x00paintings=0"):
            cfg.write_text(content)
            run(binary, f"bad_config_{content[:10]!r}", ["--config", str(cfg)], expected=2, verify_log=False)
    invalid = [
        ["--capacity", "0"], ["--capacity", "1001"], ["--visitors", "-1"], ["--visitors", "1001"],
        ["--paintings", "0"], ["--paintings", "101"], ["--picture-capacity", "0"],
        ["--picture-capacity", "1,2"], ["--picture-capacity", "1,"], ["--picture-capacity", "1,,2"],
        ["--arrival-min", "6", "--arrival-max", "5"], ["--view-max", "60001"],
        ["--seed", "4294967296"], ["--seed", "12x"], ["--strategy", "other"],
        ["--unknown"], ["--capacity"], ["garbage"], ["--config", "/nonexistent/gallery.conf"],
        ["--config", "examples/default.conf", "--config", "examples/default.conf"],
    ]
    for i, args in enumerate(invalid, 1):
        run(binary, f"invalid_{i:02}", args, expected=2, verify_log=False)
    run(binary, "log_open_error", ["--log", "/nonexistent/gallery.log"], expected=1, verify_log=False)
    run(binary, "log_write_error", [*fast, "--visitors", "2", "--log", "/dev/full"], expected=1, verify_log=False)
    interrupt(binary, "sigint_during_view", ["--arrival-min", "0", "--arrival-max", "0", "--view-min", "60000", "--view-max", "60000"], "VIEW_BEGIN", signal.SIGINT)
    interrupt(binary, "sigterm_wait_picture", ["--capacity", "20", "--picture-capacity", "1", "--strategy", "sequential", "--arrival-min", "0", "--arrival-max", "0", "--view-min", "60000", "--view-max", "60000"], "WAIT_PICTURE", signal.SIGTERM)
    interrupt(binary, "sigint_entry_queue", ["--capacity", "1", "--arrival-min", "0", "--arrival-max", "0", "--view-min", "60000", "--view-max", "60000"], "WAIT_ENTRY", signal.SIGINT,
              predicate=lambda row: int(row["inside"]) + int(row["reserved"]) == 1)
    interrupt(binary, "sigterm_before_arrival", ["--arrival-min", "60000", "--arrival-max", "60000"], "START", signal.SIGTERM)


if __name__ == "__main__":
    BINARIES = [Path(p).resolve() for p in sys.argv[1:]] or [ROOT / "gallery", ROOT / "gallery_cv"]
    started = time.monotonic()
    for binary in BINARIES:
        suite(binary)
        print(f"PASS: {binary.name}: {sum(r['binary'] == binary.name for r in RESULTS)} checks", flush=True)
    filename = "test-results.json"
    if any("asan" in p.name for p in BINARIES):
        filename = "sanitizer-results.json"
    elif any("tsan" in p.name for p in BINARIES):
        filename = "tsan-results.json"
    dest = ROOT / "results" / filename
    dest.parent.mkdir(exist_ok=True)
    dest.write_text(json.dumps({"passed": len(RESULTS), "seconds": round(time.monotonic()-started, 3), "tests": RESULTS}, ensure_ascii=False, indent=2) + "\n")
    print(f"ALL PASS: {len(RESULTS)} checks; results: {dest.relative_to(ROOT)}")
