#!/usr/bin/env python3
from collections import Counter, deque
from pathlib import Path
import fcntl
import os
import re
import signal
import subprocess as sp
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get("GALLERY_BIN_DIR", ROOT / "bin")).resolve()
MODES = ("named", "unnamed", "shm", "fifo")
LINE = re.compile(r"(\d+) \+(\d+)ms\s+(\w+)\s+v=(\d+) p=(\d+) hall=(\d+) reserved=(\d+) near=(\d+) \| (.+)")


def command(mode):
    if mode in ("named", "unnamed"):
        return [BIN / f"gallery-{mode}"]
    return [sys.executable, ROOT / "scripts/day.py", mode]


def check_trace(data, count, pictures, capacity, limit, strategy):
    limits = ([limit] * pictures if isinstance(limit, int)
              else [int(x) for x in limit.split(",")])
    phase = ["new"] * (count + 1)
    hall, reserved = set(), set()
    entrance = deque()
    queues = [deque() for _ in range(pictures + 1)]
    viewers = [set() for _ in queues]
    routes = [[] for _ in phase]
    chosen = {}
    pending_entry, pending_picture = set(), set()
    totals = Counter()
    peaks = [0, 0]
    previous_time = -1
    for seq, line in enumerate(data.splitlines(), 1):
        m = LINE.fullmatch(line)
        assert m, line
        n, ms, event, v, p, h, r, a, note = m.groups()
        v, p, h, r, a = map(int, (v, p, h, r, a))
        assert int(n) == seq and int(ms) >= previous_time, line
        previous_time = int(ms)
        assert 0 <= v <= count and 0 <= p <= pictures, line
        totals[event] += 1
        if event in ("READY", "OPEN", "CLOSE", "SUMMARY"):
            assert (v, p) == (0, 0), line
        else:
            assert v > 0 and totals["OPEN"] == 1, line
        if event == "ARRIVE":
            assert not totals["CLOSE"] and phase[v] == "new", line
            phase[v] = "arrived"
            entrance.append(v)
            if len(hall) + len(reserved) == capacity:
                pending_entry.add(v)
        elif event == "WAIT_ENTRY":
            assert len(hall) + len(reserved) == capacity and v in entrance, line
            assert v in pending_entry, line
            pending_entry.remove(v)
        elif event == "PERMIT":
            assert phase[v] == "arrived" and v not in pending_entry and entrance.popleft() == v, line
            reserved.add(v)
            phase[v] = "admitted"
        elif event == "ENTER":
            assert phase[v] == "admitted", line
            reserved.remove(v)
            hall.add(v)
            phase[v] = "inside"
        elif event == "CLOSE":
            assert totals["ENTER"] == count and not entrance and not reserved, line
        elif event == "CHOOSE":
            assert phase[v] == "inside" and p and p not in routes[v], line
            phase[v] = "waiting"
            chosen[v] = p
            queues[p].append(v)
            if len(viewers[p]) == limits[p - 1]:
                pending_picture.add(v)
        elif event == "WAIT_PICTURE":
            assert len(viewers[p]) == limits[p - 1] and chosen[v] == p and v in queues[p], line
            assert v in pending_picture, line
            pending_picture.remove(v)
        elif event == "BEGIN":
            assert v not in pending_picture, line
            assert phase[v] == "waiting" and chosen[v] == p and queues[p].popleft() == v, line
            phase[v] = "viewing"
            viewers[p].add(v)
        elif event == "END":
            assert phase[v] == "viewing" and chosen.pop(v) == p, line
            viewers[p].remove(v)
            routes[v].append(p)
            phase[v] = "inside"
        elif event == "EXIT":
            assert phase[v] == "inside" and len(set(routes[v])) == pictures, line
            if strategy == "ordered":
                assert routes[v] == list(range(1, pictures + 1)), line
            hall.remove(v)
            phase[v] = "gone"
        elif event == "SUMMARY":
            assert f"Вышли {count} из {count}. Осмотров {count * pictures}." in note, line
            assert f"max_hall={peaks[0]} max_near={peaks[1]}" in note, line
        elif event not in ("READY", "OPEN"):
            raise AssertionError(line)
        assert (h, r, a) == (len(hall), len(reserved), len(viewers[p])), line
        assert h + r <= capacity and (p == 0 or a <= limits[p - 1]), line
        peaks = [max(peaks[0], h), max(peaks[1], a)]
    assert totals["READY"] == totals["OPEN"] == totals["CLOSE"] == totals["SUMMARY"] == 1
    assert event == "SUMMARY" and phase[1:] == ["gone"] * count and not hall and not chosen
    assert totals["END"] == count * pictures
    return routes, totals


def successful_runs(tmp):
    routes_by_case = {}
    for mode in MODES:
        for name, n, p, h, a, arr, view, strategy in [
            ("single", 1, 1, 1, 1, "0", "0", "random"),
            ("queues", 12, 3, 3, 1, "0", "15", "ordered"),
            ("day", 120, 5, 12, 3, "0:2", "1:4", "random"),
            ("limits", 12, 3, 8, "1,3,2", "0", "3:6", "ordered"),
            ("boundary", 2, 64, 2, 2, "1", "0", "random"),
        ]:
            logfile = tmp / "events.log"
            opts = ["--visitors", n, "--paintings", p, "--capacity", h, "--limit", a,
                    "--arrival", arr, "--view", view, "--strategy", strategy, "--seed", 42,
                    "--log", logfile]
            result = sp.run([*command(mode), *map(str, opts)], capture_output=True,
                            text=True, timeout=40)
            assert result.returncode == 0, (mode, name, result.stderr)
            assert result.stdout == logfile.read_text()
            routes, totals = check_trace(result.stdout, n, p, h, a, strategy)
            if name == "queues":
                assert totals["WAIT_ENTRY"] and totals["WAIT_PICTURE"]
            if name in routes_by_case:
                assert routes_by_case[name] == routes
            routes_by_case[name] = routes
            print(f"{mode:7} {name:9} EXIT={totals['EXIT']} END={totals['END']} "
                  f"WAIT_ENTRY={totals['WAIT_ENTRY']} WAIT_PICTURE={totals['WAIT_PICTURE']}")
            logfile.unlink()
            assert not list(tmp.iterdir()), list(tmp.iterdir())


def wait_for(predicate, timeout=6):
    end = time.monotonic() + timeout
    while not predicate():
        if time.monotonic() >= end:
            raise AssertionError("Истекло время ожидания события теста")
        time.sleep(0.005)


def check_cleanup(owner):
    assert not list(Path("/dev/shm").glob(f"*gallery3_{os.getuid()}_{owner}_*"))


def blocked_output_test(tmp, mode):
    # Заполненный stdout: до завершения процесса читатель не забирает ни байта.
    read_fd, write_fd = os.pipe()
    logfile = tmp / "blocked.log"
    try:
        size = fcntl.fcntl(write_fd, fcntl.F_GETPIPE_SZ)
        assert os.write(write_fd, b"." * size) == size
        with sp.Popen([*command(mode), "--visitors", "4", "--log", logfile],
                      stdout=write_fd, start_new_session=True) as proc:
            try:
                wait_for(lambda: logfile.exists() and "READY" in logfile.read_text())
                proc.send_signal(signal.SIGINT)
                assert proc.wait(timeout=4) == 130
                assert "STOP" in logfile.read_text()
            finally:
                os.close(read_fd)
                read_fd = -1
                if proc.poll() is None:
                    proc.terminate()
        print(f"{mode:7} SIGINT при заполненном stdout — итог записан в файл")
    finally:
        if read_fd >= 0:
            os.close(read_fd)
        os.close(write_fd)
        logfile.unlink(missing_ok=True)
    assert not list(tmp.iterdir()), list(tmp.iterdir())


def fork_backend_test(tmp, mode):
    # При запуске под другим именем проверяем настоящие объекты POSIX в Linux.
    alias, logfile = tmp / "museum", tmp / "backend.log"
    alias.symlink_to(command(mode)[0])
    try:
        with sp.Popen([alias, "--visitors", "1", "--view", "60000", "--log", logfile],
                      stdout=sp.DEVNULL) as proc:
            try:
                wait_for(lambda: logfile.exists() and "BEGIN" in logfile.read_text())
                prefix = f"gallery3_{os.getuid()}_{proc.pid}_"
                assert (Path("/dev/shm") / (prefix + "-1")).exists()
                assert (Path("/dev/shm") / ("sem." + prefix + "0")).exists() == (mode == "named")
            finally:
                proc.send_signal(signal.SIGINT)
                proc.wait(timeout=4)
            check_cleanup(proc.pid)
    finally:
        alias.unlink()
        logfile.unlink(missing_ok=True)


def interrupt_test(tmp, mode, target):
    independent = mode in ("shm", "fifo")
    session = tmp / "test-session"
    opts = ["--visitors", "4", "--capacity", "2", "--limit", "1", "--paintings", "1",
            "--arrival", "0", "--view", "60000"]
    children = []
    logfile = tmp / "interrupt.log"
    try:
        with logfile.open("w") as out:
            cmd = [BIN / "guard", mode, session] if independent else command(mode)
            owner = sp.Popen([*cmd, *opts], stdout=out,
                             preexec_fn=os.setpgrp if target == "group" else None)
            children.append(owner)
            if independent:
                wait_for(lambda: (session / "ready").exists())
                for i in range(4, 0, -1):
                    join_group = (lambda: os.setpgid(0, owner.pid)) if target == "group" else None
                    children.append(sp.Popen([BIN / "visitor", session, str(i)], stdout=out,
                                             preexec_fn=join_group))
            wait_for(lambda: "WAIT_PICTURE" in logfile.read_text())
            if independent:
                duplicate = sp.run([BIN / "visitor", session, "1"], capture_output=True, timeout=2)
                assert duplicate.returncode != 0
            if target == "group":
                os.killpg(owner.pid, signal.SIGINT)
            else:
                victim = children[-1] if target == "visitor" else owner
                victim.send_signal(signal.SIGINT)
            for child in children:
                assert child.wait(timeout=5) != 0
            if target == "group":
                assert owner.returncode == 130
        assert "STOP" in logfile.read_text() and "SUMMARY" not in logfile.read_text()
        check_cleanup(owner.pid)
        print(f"{mode:7} SIGINT({target}) — завершение и очистка")
    finally:
        for child in children:
            if child.poll() is None:
                child.terminate()
            child.wait(timeout=5)
        logfile.unlink(missing_ok=True)
    assert not list(tmp.iterdir()), list(tmp.iterdir())


def main():
    with tempfile.TemporaryDirectory(prefix="gallery-check-") as folder:
        tmp = Path(folder)
        os.environ["TMPDIR"] = str(tmp)
        successful_runs(tmp)
        for mode in MODES:
            blocked_output_test(tmp, mode)
            interrupt_test(tmp, mode, "owner")
            if mode in ("shm", "fifo"):
                interrupt_test(tmp, mode, "visitor")
                interrupt_test(tmp, mode, "group")
            else:
                fork_backend_test(tmp, mode)
        for args in (
            ["--visitors", "0"],
            ["--limit", "-1"],
            ["--paintings", "65"],
            ["--view", "7:3"],
            ["--arrival", "12ms"],
            ["--strategy", "x"],
            ["--seed"],
            ["--capacity", ""],
            ["--limit", "1,2"],
            ["--paintings", "3", "--limit", "1,,2"],
        ):
            proc = sp.run([BIN / "gallery-named", *args], capture_output=True, timeout=2)
            assert proc.returncode == 2, args
    print("OK — журналы, FIFO-порядок, инварианты, seed, сигналы и ошибки ввода")


if __name__ == "__main__":
    main()
