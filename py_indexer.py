"""
Docstring for py_indexer_thread
"""

from dataclasses import dataclass
from pathlib import Path
import time, os, sys, getopt, stat, json, statistics, hashlib
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from concurrent import futures
from typing import Dict, List, Optional

RAN = False

@dataclass
class Job:
    path: Path
    arrival: int
    est_cost: int
    remaining: int = 1
    queue_level: int = 0


def main(Executor: futures, workers: int):
    """Run one indexing pass and return elapsed seconds."""
    global RAN
    RAN = True

    root_dir = Path(r"testFiles")
    index_file = Path("index.json")

    jobs = build_jobs(root_dir)

    # SJF ordering: smallest est_cost first, tie-break by arrival
    jobs.sort(key=lambda j: (j.est_cost, j.arrival))

    start = time.time()
    with Executor(max_workers=workers) as ex:
        results = list(ex.map(task, jobs, chunksize=200))
    end = time.time()

    elapsed = end - start

    with index_file.open("w", encoding="utf-8") as f:
        f.write(json.dumps(results) + "\n")

    return elapsed


def task(job: Job) -> Dict:
    record = index(job.path)
    record["arrival"] = job.arrival
    record["est_cost"] = job.est_cost
    record["queue_level"] = job.queue_level
    return record


def index(filepath: Path) -> Dict:
    record: Dict = {"path": str(filepath), "name": filepath.name}

    try:
        fileStats = filepath.stat()

        record["is_file"] = stat.S_ISREG(fileStats.st_mode)
        record["is_dir"] = stat.S_ISDIR(fileStats.st_mode)
        record["is_symlink"] = stat.S_ISLNK(fileStats.st_mode)

        record["size_bytes"] = fileStats.st_size
        record["mtime"] = fileStats.st_mtime
        record["atime"] = fileStats.st_atime

        record["extension"] = filepath.suffix.lower()

        # Basic permissions snapshot
        record["mode_octal"] = oct(stat.S_IMODE(fileStats.st_mode))
        record["uid"] = getattr(fileStats, "st_uid", None)
        record["gid"] = getattr(fileStats, "st_gid", None)

    except PermissionError as e:
        record["error"] = f"PermissionError: {e}"
    except FileNotFoundError as e:
        record["error"] = f"FileNotFoundError: {e}"
    except OSError as e:
        record["error"] = f"OSError: {e}"

    return record


def build_jobs(dir_root: Path) -> List[Job]:
    jobs: List[Job] = []
    tick = 0

    for dirpath, _, filenames in os.walk(dir_root):
        d = Path(dirpath)
        for fn in filenames:
            p = d / fn
            tick += 1

            try:
                size = p.stat().st_size
            except OSError:
                size = 0

            if size < 100_000:
                est = 1
            elif size < 10_000_000:
                est = 2
            else:
                est = 3

            jobs.append(Job(path=p, arrival=tick, est_cost=est, remaining=est))

    return jobs


def next_job(pool: List[Job]) -> Optional[Job]:
    if not pool:
        return None
    job = min(pool, key=lambda j: j.est_cost)
    pool.remove(job)
    return job


def cmd_find(min_bytes: int, root_dir: Path = Path("testFiles")) -> None:
    """List files in root_dir larger than min_bytes."""
    for dirpath, _, filenames in os.walk(root_dir):
        d = Path(dirpath)
        for fn in filenames:
            p = d / fn
            try:
                sz = p.stat().st_size
            except OSError:
                continue
            if sz > min_bytes:
                print(f"{p} ({sz} bytes)")


def cmd_checksum(path: Path, algo: str = "sha256") -> None:
    """Print checksum for a file using hashlib (default sha256)."""
    algo = algo.lower()

    try:
        h = hashlib.new(algo)
    except ValueError:
        supported = ", ".join(sorted(hashlib.algorithms_available))
        raise SystemExit(f"Unknown hash algorithm '{algo}'. Supported includes: {supported}")

    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)

    print(f"{algo}({path}) = {h.hexdigest()}")


def usage() -> None:
    print(
        "Usage:\n"
        "  python py_indexer_thread.py -t N             # threads\n"
        "  python py_indexer_thread.py -p N             # processes\n"
        "  python py_indexer_thread.py --test           # benchmark loop\n"
        "  python py_indexer_thread.py find X           # list files > X bytes in testFiles/\n"
        "  python py_indexer_thread.py find \">\" X       # same, if you want the '>' token\n"
        "  python py_indexer_thread.py checksum FILE    # sha256 by default\n"
        "  python py_indexer_thread.py checksum FILE --hash ALGO\n"
    )


if __name__ == "__main__":
    args = sys.argv[1:]
    options = "p:t:"
    long_options = ["hash=", "test"]
    default_workers = 4
    results_file = Path("benchmark_py.txt")

    if args:
        if args[0] == "find":
            if len(args) == 2:
                min_bytes = int(args[1])
            elif len(args) == 3 and args[1] == ">":
                min_bytes = int(args[2])
            else:
                usage()
                raise SystemExit(2)

            cmd_find(min_bytes)
            raise SystemExit(0)

        if args[0] == "checksum":
            if len(args) < 2:
                usage()
                raise SystemExit(2)

            file_path = Path(args[1])
            algo = "sha256"

            if len(args) >= 4 and args[2] == "--hash":
                algo = args[3]

            cmd_checksum(file_path, algo)
            raise SystemExit(0)

    try:
        arguments, values = getopt.getopt(args, options, long_options)

        for currArg, currVal in arguments:
            if currArg == "-p":
                workers = int(currVal) if currVal else default_workers
                main(ProcessPoolExecutor, workers)

            elif currArg == "-t":
                workers = int(currVal) if currVal else default_workers
                main(ThreadPoolExecutor, workers)

            elif currArg == "--test":
                results_file.write_text("", encoding="utf-8")

                iterations = 10
                worker_interval = 1
                worker_iterations = 12

                with results_file.open("a", encoding="utf-8") as f:
                    for i in range(worker_iterations):
                        workers = 1 + (i * worker_interval)

                        times = [main(ProcessPoolExecutor, workers) for _ in range(iterations)]
                        f.write(f"Mean time of {workers} processes = {statistics.mean(times):.5f}!\n")

                        times = [main(ThreadPoolExecutor, workers) for _ in range(iterations)]
                        f.write(f"Mean time of {workers} threads = {statistics.mean(times):.5f}!\n")

    except getopt.error as err:
        print(str(err))
        usage()

    if not RAN:
        main(ThreadPoolExecutor, default_workers)  # Default behaviour
