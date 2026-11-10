#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parents[1]
LINE = re.compile(
    r"^(\d+) t_ms=(\d+) event=(\w+) visitor=(\d+) painting=(\d+) "
    r"inside=(\d+) reserved=(\d+) queue=(\d+) viewers=(\d+) \| (.*)$"
)


def fields(text):
    return dict(re.findall(r"(\w+)=([^\s]+)", text))


def verify_log(text, *, normal=True):
    """Независимо восстановить модель только по публичным событиям."""
    state, seen, current = {}, defaultdict(set), {}
    entrance = deque()
    picture_queues = defaultdict(deque)
    viewers = Counter()
    totals = Counter()
    choices = defaultdict(list)
    chosen = {}
    arrival_delays = {}
    events = Counter()
    config = summary = None
    peak_inside = peak_occupied = peak_viewers = 0
    last_time = -1
    closed = False
    stopping = False
    last_sequence = 0
    for line in text.splitlines():
        match = LINE.fullmatch(line)
        assert match, f"Повреждённая строка: {line[:150]}"
        sequence, elapsed, event, visitor, painting, inside, reserved, queue, count, detail = match.groups()
        sequence, elapsed, visitor, painting, inside, reserved, queue, count = map(
            int, (sequence, elapsed, visitor, painting, inside, reserved, queue, count)
        )
        assert sequence == last_sequence + 1
        assert elapsed >= last_time
        last_sequence, last_time = sequence, elapsed
        events[event] += 1
        data = fields(detail)
        if event == "CONFIG":
            assert config is None
            config = data
        assert config is not None
        capacity, paintings, limit = (int(config[k]) for k in
                                     ("capacity", "paintings", "picture_capacity"))
        if event == "ARRIVE":
            assert visitor not in state
            state[visitor] = "arrived"
            arrival_delays[visitor] = int(data["arrival_delay_ms"])
        elif event == "WAIT_ENTRY":
            assert state[visitor] == "arrived" and not closed
            state[visitor] = "waiting"
            entrance.append(visitor)
        elif event == "ALLOW":
            assert not stopping and not closed
            assert entrance.popleft() == visitor, "Вахтёр нарушил FIFO"
            assert state[visitor] == "waiting"
            state[visitor] = "reserved"
        elif event == "ENTER":
            assert state[visitor] == "reserved" and not stopping
            state[visitor] = "inside"
        elif event == "CHOOSE":
            assert state[visitor] == "inside"
            assert visitor not in current and painting not in seen[visitor]
            assert visitor not in chosen
            assert 1 <= painting <= paintings
            chosen[visitor] = painting
            choices[visitor].append((painting, int(data["view_ms"])))
            picture_queues[painting].append(visitor)
        elif event == "WAIT_PAINTING":
            assert chosen[visitor] == painting and visitor not in current
        elif event == "VIEW_START":
            assert state[visitor] == "inside" and not stopping
            assert chosen.pop(visitor) == painting
            assert picture_queues[painting].popleft() == visitor, "Нарушен FIFO у картины"
            assert visitor not in current and painting not in seen[visitor]
            current[visitor] = painting
            viewers[painting] += 1
        elif event in ("VIEW_END", "VIEW_ABORT"):
            assert current.pop(visitor) == painting
            viewers[painting] -= 1
            if event == "VIEW_END":
                assert painting not in seen[visitor]
                seen[visitor].add(painting)
                totals[painting] += 1
            else:
                assert stopping
        elif event == "EXIT":
            assert state[visitor] == "inside" and visitor not in current
            complete = data["completed"] == "1"
            assert complete == (len(seen[visitor]) == paintings)
            if not complete:
                assert stopping
            state[visitor] = "completed" if complete else "interrupted"
            if visitor in chosen:
                target = chosen.pop(visitor)
                picture_queues[target].remove(visitor)
        elif event == "CANCEL":
            assert stopping
            assert state[visitor] in ("arrived", "waiting", "reserved")
            if state[visitor] == "waiting":
                assert entrance.popleft() == visitor
            state[visitor] = "cancelled"
        elif event == "STOP":
            stopping = True
        elif event == "ENTRY_CLOSED":
            assert not closed
            closed = True
            if not stopping:
                assert not entrance
        elif event == "SUMMARY":
            assert summary is None
            summary = data
        elif event == "PICTURE_SUMMARY":
            assert int(data["completed_views"]) == totals[painting]
            assert int(data["peak"]) <= limit
        elif event not in ("CONFIG", "ARRIVALS_DONE", "GUARD_DONE", "ERROR"):
            raise AssertionError(f"Неизвестное событие: {event}")

        # Проверяем фактические состояния, а затем сверяем опубликованные счётчики.
        actual_inside = sum(value == "inside" for value in state.values())
        actual_reserved = sum(value == "reserved" for value in state.values())
        assert actual_inside + actual_reserved <= capacity, "Переполнение галереи"
        assert inside == actual_inside and reserved == actual_reserved
        assert queue == len(entrance)
        assert all(0 <= value <= limit for value in viewers.values())
        assert count == (viewers[painting] if painting else 0)
        peak_inside = max(peak_inside, inside)
        peak_occupied = max(peak_occupied, inside + reserved)
        peak_viewers = max(peak_viewers, sum(viewers.values()))

    assert config is not None and summary is not None
    assert not current and not entrance and not chosen
    assert all(not queue for queue in picture_queues.values())
    assert not any(viewers.values())
    counts = Counter(state.values())
    assert len(state) == int(summary["arrived"]) == int(summary["created"])
    for key in ("completed", "interrupted", "cancelled"):
        assert counts[key] == int(summary[key])
    assert sum(counts[k] for k in ("completed", "interrupted", "cancelled")) == len(state)
    assert int(summary["views"]) == sum(totals.values())
    assert int(summary["peak_inside"]) == peak_inside
    assert int(summary["peak_occupied"]) == peak_occupied
    assert int(summary["peak_viewers"]) == peak_viewers
    assert int(summary["unstarted"]) + len(state) == int(config["visitors"])
    assert events["PICTURE_SUMMARY"] == int(config["paintings"])
    if normal:
        assert closed and summary["reason"] == "complete"
        assert counts["completed"] == int(config["visitors"])
        assert int(summary["views"]) == int(config["visitors"]) * int(config["paintings"])
    if config["strategy"] == "ordered":
        for route in choices.values():
            assert [p for p, _ in route] == list(range(1, len(route) + 1))
    return dict(summary=summary, config=config, choices=dict(choices),
                arrival_delays=arrival_delays, events=dict(events))


class Suite:
    def __init__(self, binary, temporary):
        self.binary = str(binary)
        self.temporary = Path(temporary)
        self.results = []

    def record(self, name, callback):
        started = time.monotonic()
        result = callback()
        self.results.append(dict(name=name, status="passed",
                                 seconds=round(time.monotonic() - started, 4),
                                 summary=result.get("summary") if isinstance(result, dict) else None))
        print(f"PASS {name}", flush=True)
        return result

    def run(self, extra=(), *, code=0, normal=True, stdin=None, env=None):
        log = self.temporary / "run.log"
        defaults = [
            "--visitors", "12", "--capacity", "4", "--paintings", "4",
            "--picture-capacity", "2", "--arrival-min-ms", "0", "--arrival-max-ms", "0",
            "--view-min-ms", "0", "--view-max-ms", "1",
        ] if "--config" not in extra else []
        command = [
            self.binary, *defaults, "--quiet", "--log", str(log),
            *map(str, extra),
        ]
        proc = subprocess.run(command, input=stdin, text=True, capture_output=True,
                              timeout=30, cwd=ROOT, env=env)
        assert proc.returncode == code, (command, proc.returncode, proc.stderr, proc.stdout[-4000:])
        if code == 2:
            assert proc.stderr
            return {}
        return verify_log(log.read_text(), normal=normal)

    def interrupted(self, number, *, before_arrival=False):
        log = self.temporary / "signal.log"
        command = [
            self.binary, "--visitors", "40", "--capacity", "2", "--paintings", "3",
            "--picture-capacity", "1", "--strategy", "ordered",
            "--arrival-min-ms", "60000" if before_arrival else "0",
            "--arrival-max-ms", "60000" if before_arrival else "0",
            "--view-min-ms", "60000", "--view-max-ms", "60000",
            "--quiet", "--log", str(log),
        ]
        if log.exists():
            log.unlink()
        proc = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                text = log.read_text() if log.exists() else ""
                if before_arrival:
                    ready = "event=CONFIG " in text
                else:
                    ready = all(f"event={event} " in text for event in
                                ("ARRIVALS_DONE", "VIEW_START", "WAIT_PAINTING", "WAIT_ENTRY"))
                if ready:
                    break
                assert proc.poll() is None, proc.communicate()[1]
                time.sleep(0.01)
            else:
                raise AssertionError("Не достигнуто состояние для проверки сигнала")
            proc.send_signal(number)
            _, stderr = proc.communicate(timeout=5)
            assert proc.returncode == 128 + number, (proc.returncode, stderr)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
        result = verify_log(log.read_text(), normal=False)
        assert result["summary"]["reason"] == ("sigint" if number == signal.SIGINT else "sigterm")
        if before_arrival:
            assert int(result["summary"]["created"]) == 0
        else:
            assert int(result["summary"]["interrupted"]) > 0
            assert int(result["summary"]["cancelled"]) > 0
        return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", nargs="?", default="./gallery")
    parser.add_argument("--skip-faults", action="store_true",
                        help="Оставлен для совместимости со старым запуском тестов.")
    parser.add_argument("--report", type=Path)
    args = parser.parse_args()
    binary = Path(args.binary).resolve()
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="gallery-tests-") as temporary:
        suite = Suite(binary, temporary)
        for name, extra in [
            ("empty_day", ["--visitors", 0]),
            ("single_visitor", ["--visitors", 1, "--capacity", 1, "--paintings", 1]),
            ("single_slot_100", ["--visitors", 100, "--capacity", 1, "--picture-capacity", 1]),
            ("random_day_200", ["--config", "examples/day.conf"]),
            ("ordered_contention_100", ["--config", "examples/contention.conf",
                                         "--visitors", 100, "--picture-capacity", 1,
                                         "--strategy", "ordered"]),
            ("random_day_300", ["--visitors", 300, "--seed", 2026]),
            ("64_paintings", ["--visitors", 4, "--paintings", 64]),
            ("capacity_larger_than_visitors", ["--visitors", 2, "--capacity", 1000,
                                               "--picture-capacity", 1000]),
            ("seed_zero", ["--seed", 0]),
            ("seed_max", ["--seed", 4294967295]),
            ("zero_delays", ["--visitors", 100, "--view-max-ms", 0]),
        ]:
            suite.record(name, lambda extra=extra: suite.run(extra))

        def simultaneous_intervals():
            result = suite.run(["--visitors", 4, "--capacity", 4, "--paintings", 1,
                                "--picture-capacity", 2, "--view-min-ms", 20,
                                "--view-max-ms", 20])
            assert int(result["summary"]["peak_viewers"]) == 2, \
                "Дискретная модель не сохранила перекрывающиеся интервалы осмотра"
            return result
        suite.record("simultaneous_viewing_intervals", simultaneous_intervals)

        def reproducible():
            options = ["--seed", 9182, "--visitors", 25, "--view-max-ms", 3]
            first = suite.run(options)
            second = suite.run(options)
            assert first["choices"] == second["choices"]
            assert first["arrival_delays"] == second["arrival_delays"]
            return first
        suite.record("seed_reproduces_plans", reproducible)
        for seed in range(8):
            suite.record(f"stress_seed_{seed}", lambda seed=seed: suite.run(
                ["--visitors", 100, "--paintings", 6, "--capacity", 7,
                 "--picture-capacity", 1, "--seed", seed]))

        invalid = [
            ["--capacity", 0], ["--paintings", 0], ["--paintings", 65],
            ["--picture-capacity", 0], ["--visitors", -1], ["--visitors", 1001],
            ["--seed", 4294967296], ["--seed", "-1"], ["--capacity", "2junk"],
            ["--capacity", "999999999999999999999999999999999999"],
            ["--arrival-min-ms", 2, "--arrival-max-ms", 1],
            ["--view-min-ms", 2, "--view-max-ms", 1],
            ["--view-max-ms", 60001], ["--strategy", "wrong"], ["--unknown"],
            ["extra"], ["--visitors"], ["--config", "missing.conf"],
            ["--log", ""],
        ]
        for index, options in enumerate(invalid):
            suite.record(f"invalid_cli_{index + 1:02d}",
                         lambda options=options: suite.run(options, code=2))

        config_file = Path(temporary) / "input.conf"
        for index, content in enumerate([
            "capacity=2\ncapacity=3\n", "unknown=1\n", "capacity 2\n",
            "capacity=" + "3" * 5000 + "\n", "quiet=3\n",
        ]):
            config_file.write_text(content)
            suite.record(f"invalid_config_{index + 1}",
                         lambda: suite.run(["--config", config_file], code=2))
        config_file.write_text("visitors=1\ncapacity=1\n")

        def priority():
            result = suite.run(["--visitors", 7, "--config", config_file])
            assert int(result["summary"]["completed"]) == 7
            return result
        suite.record("cli_priority_independent_of_position", priority)
        suite.record("duplicate_config_argument", lambda: suite.run(
            ["--config", config_file, "--config", config_file], code=2))
        suite.record("interactive_defaults", lambda: suite.run(["--interactive"], stdin="\n" * 10))
        suite.record("interactive_retry", lambda: suite.run(
            ["--interactive"], stdin="wrong\n2\n" + "\n" * 9))
        suite.record("interactive_eof", lambda: suite.run(["--interactive"], code=2, stdin=""))

        suite.record("sigint_during_view_and_queues", lambda: suite.interrupted(signal.SIGINT))
        suite.record("sigterm_during_view_and_queues", lambda: suite.interrupted(signal.SIGTERM))
        suite.record("sigint_during_long_arrival", lambda: suite.interrupted(
            signal.SIGINT, before_arrival=True))

        def invalid_log():
            proc = subprocess.run([str(binary), "--log", str(Path(temporary) / "absent/a.log")],
                                  capture_output=True, timeout=5)
            assert proc.returncode == 1 and proc.stderr
        suite.record("log_open_error", invalid_log)

        def same_file():
            before = config_file.read_bytes()
            alias = Path(temporary) / "alias.conf"
            os.link(config_file, alias)
            proc = subprocess.run([str(binary), "--config", str(config_file), "--log", str(alias)],
                                  capture_output=True, timeout=5)
            assert proc.returncode == 1 and proc.stderr
            assert config_file.read_bytes() == before, "Исходная конфигурация была повреждена"
        suite.record("config_log_same_inode_protected", same_file)

        def full_device():
            proc = subprocess.run([str(binary), "--log", "/dev/full", "--visitors", "0"],
                                  capture_output=True, timeout=5)
            assert proc.returncode == 1 and b"SUMMARY" in proc.stdout and proc.stderr
        suite.record("log_write_error", full_device)

        def no_log():
            proc = subprocess.run([str(binary), "--log", "-", "--visitors", "0"],
                                  text=True, capture_output=True, timeout=5)
            assert proc.returncode == 0, proc.stderr
            return verify_log(proc.stdout)
        suite.record("console_only", no_log)

        def broken_pipe():
            log = Path(temporary) / "broken_pipe.log"
            proc = subprocess.Popen([str(binary), "--log", str(log), "--visitors", "100",
                                     "--arrival-min-ms", "1", "--arrival-max-ms", "1"],
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            proc.stdout.close()
            proc.stdout = None
            _, stderr = proc.communicate(timeout=5)
            code = proc.returncode
            assert code == 1 and stderr
            assert "event=SUMMARY " in log.read_text()
        suite.record("broken_stdout_pipe", broken_pipe)

        report = dict(
            date_utc=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            platform=platform.platform(), binary=binary.name,
            compiler=subprocess.check_output(["gcc", "--version"], text=True).splitlines()[0],
            total=len(suite.results), passed=len(suite.results),
            seconds=round(time.monotonic() - started, 3), results=suite.results,
            source_sha256={str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                           for p in sorted((ROOT / "src").glob("*.c"))},
        )
        if args.report:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
        print(f"ALL PASSED: {report['passed']} tests; {report['seconds']} seconds.")


if __name__ == "__main__":
    main()
