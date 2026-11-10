#!/usr/bin/env python3
from pathlib import Path
import os
import signal
import subprocess
import sys
import tempfile
import time

BIN = Path(os.environ.get("GALLERY_BIN_DIR", Path(__file__).resolve().parents[1] / "bin")).resolve()


def main(args):
    if not args or args[0] not in ("shm", "fifo"):
        print("Запуск: python3 scripts/day.py shm|fifo [параметры вахтёра]", file=sys.stderr)
        return 0 if args == ["--help"] else 2
    if args[1:] == ["--help"]:
        return subprocess.call([BIN / "guard", "--help"])
    interrupted = 0

    def stop(signum, _frame):
        nonlocal interrupted
        interrupted = signum

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    with tempfile.TemporaryDirectory(prefix="gallery-launch-") as root:
        session = Path(root) / "day"
        children = []
        try:
            guard = subprocess.Popen([BIN / "guard", args[0], session, *args[1:]])
            children.append(guard)
            while not (session / "ready").exists():
                if interrupted:
                    return 128 + interrupted
                if guard.poll() is not None:
                    return guard.returncode
                time.sleep(0.01)
            with (session / "ready").open("rb") as ready:
                _, count, _ = map(int, ready.readline().split())
            for visitor in range(1, count + 1):
                if interrupted:
                    return 128 + interrupted
                if guard.poll() is not None:
                    return guard.returncode or 1
                children.append(subprocess.Popen([BIN / "visitor", session, str(visitor)]))
            while not interrupted:
                statuses = [child.poll() for child in children]
                if any(code not in (None, 0) for code in statuses):
                    return 1
                if all(code == 0 for code in statuses):
                    return 0
                time.sleep(0.02)
            return 128 + interrupted
        except OSError as error:
            print(f"Запуск: {error}", file=sys.stderr)
            return 1
        finally:
            for child in children:
                if child.poll() is None:
                    child.terminate()
            for child in children:
                child.wait()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
