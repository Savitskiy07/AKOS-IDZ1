#!/usr/bin/env python3
"""Расширенный аудит: конкуренция, сигналы, системные отказы и границы."""
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import tempfile
import time
import test_gallery as core

ROOT = core.ROOT
FAST = ["--arrival-min", "0", "--arrival-max", "0", "--view-min", "0", "--view-max", "0", "--log", "none"]


def save(name, binary, **detail):
    core.RESULTS.append({"binary": binary.name, "test": name, "result": "PASS", **detail})


def audit(binary, condvars):
    rng = random.Random(1616)
    for i in range(30):
        paintings = rng.randint(1, 10)
        proc = core.run(binary, f"contended_{i+1:02}", [*FAST,
            "--visitors", str(rng.randint(20, 100)), "--paintings", str(paintings),
            "--capacity", str(rng.randint(1, 20)), "--picture-capacity", str(rng.randint(1, 3)),
            "--arrival-max", "1", "--view-min", "1", "--view-max", "2",
            "--seed", str(rng.getrandbits(32)), "--strategy", rng.choice(["random", "sequential"])])
        for row in core.parse(proc.stdout):
            if row["event"] == "PLAN":
                assert 0 <= int(row["arrival_ms"]) <= 1
                assert all(1 <= int(t) <= 2 for t in row["durations"].split(","))

    # Пограничный допустимый N; быстрая работа ограничивает число живых потоков.
    core.run(binary, "visitors_1000", [*FAST, "--visitors", "1000", "--paintings", "1"])
    core.run(binary, "individual_limits_100", [*FAST, "--visitors", "2", "--paintings", "100",
             "--picture-capacity", ",".join(str(i + 1) for i in range(100))])
    for i in range(12):
        target = ["PERMIT", "VIEW_BEGIN", "WAIT_PICTURE"][i % 3]
        sig = signal.SIGINT if i % 2 == 0 else signal.SIGTERM
        core.interrupt(binary, f"interrupt_stress_{i+1:02}_{target}",
            ["--visitors", "100", "--capacity", "5", "--paintings", "3", "--picture-capacity", "1",
             "--arrival-min", "0", "--arrival-max", "0", "--view-min", "60000", "--view-max", "60000",
             "--strategy", "sequential"], target, sig)

    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        fault = td / "gallery_fault"
        command = ["gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
        if condvars:
            command.append("-DUSE_CONDVAR")
        subprocess.run(command + ["-fanalyzer", "src/main.c", "src/config.c", "src/sync.c", "-pthread",
                       "-o", str(td / "gallery_analyzed")], cwd=ROOT, check=True, capture_output=True, text=True)
        save("gcc_static_analyzer", binary)
        command += ["src/main.c", "src/config.c", "src/sync.c", "tests/fault_injection.c", "-pthread",
                    "-Wl,--wrap=pthread_create", "-Wl,--wrap=calloc",
                    "-Wl,--wrap=" + ("pthread_cond_init" if condvars else "sem_init"), "-o", str(fault)]
        subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
        # Первый и второй вызовы создают поток сигналов и вахтёра;
        # последующие вызывают ошибку до/после появления посетителей.
        faults = [("GALLERY_FAIL_THREAD", i) for i in (1, 2, 3, 8, 25)]
        faults += [("GALLERY_FAIL_SYNC", i) for i in (1, 3, 10)]
        faults += [("GALLERY_FAIL_ALLOC", 1)]
        for key, count in faults:
            env = os.environ.copy()
            env[key] = str(count)
            proc = subprocess.run([str(fault), *FAST, "--visitors", "50", "--arrival-max", "1", "--view-max", "2"],
                                  cwd=ROOT, capture_output=True, text=True, env=env, timeout=15)
            assert proc.returncode == 1, (key, count, proc.returncode, proc.stderr)
            if key == "GALLERY_FAIL_THREAD" and count >= 3:
                core.verify(proc.stdout, interrupted=True, expected_status="error")
                assert "event=ERROR " in proc.stdout
            else:
                assert proc.stderr.strip(), (key, count, "no diagnostic")
            save(f"fault_{key}_{count}", binary)

        # Проверяем, что закрытый stdout не убивает процесс через SIGPIPE,
        # а полный журнал сохраняется и освобождение мест завершается.
        log = td / "broken-pipe.log"
        read_fd, write_fd = os.pipe()
        os.close(read_fd)
        try:
            proc = subprocess.run([str(binary), *FAST, "--visitors", "20", "--log", str(log)],
                                  cwd=ROOT, stdout=write_fd, stderr=subprocess.PIPE, text=True, timeout=15)
        finally:
            os.close(write_fd)
        assert proc.returncode == 1
        core.verify(log.read_text(), expected_status="io_error")
        save("closed_stdout", binary)

        # Пустой файл конфигурации, CRLF, отсутствие финального перевода строки.
        config = td / "valid.conf"
        for name, content in [("empty", ""), ("crlf", "visitors=2\r\npaintings=1\r\n"), ("no_newline", "visitors=2")]:
            config.write_bytes(content.encode())
            core.run(binary, f"config_{name}", [*FAST, "--config", str(config)])

    symbols = subprocess.run(["nm", "-u", str(binary)], capture_output=True, text=True, check=True).stdout
    if condvars:
        assert "sem_" not in symbols and "pthread_cond_timedwait" in symbols
    else:
        assert "sem_timedwait" in symbols and "sem_post" in symbols
    save("synchronization_backend_symbols", binary)


if __name__ == "__main__":
    started = time.monotonic()
    for binary, condvars in [(ROOT / "gallery", False), (ROOT / "gallery_cv", True)]:
        audit(binary, condvars)
        print(f"PASS audit: {binary.name}", flush=True)
    result = {"passed": len(core.RESULTS), "seconds": round(time.monotonic() - started, 3), "tests": core.RESULTS}
    (ROOT / "results" / "audit-results.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
    print(f"ALL PASS audit: {len(core.RESULTS)} checks")
