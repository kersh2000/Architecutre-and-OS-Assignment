"""
Docstring for py_indexer_thread
"""

from dataclasses import dataclass
from pathlib import Path
import time, os, sys, getopt, stat, json, statistics
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from concurrent import futures
from typing import Dict

RAN = False

@dataclass
class Job:
    path: Path
    arrival: int
    est_cost: int
    remaining: int = 1
    queue_level: int = 0

def main(Executor: futures, workers: int):
    global RAN
    RAN = True

    root_dir = Path(r"testFiles")
    index_file = Path("index.json")

    # print(f"Multiprocessing" if Executor is ProcessPoolExecutor else "Multithreading" with {workers} workers...")

    jobs = build_jobs(root_dir)

    start = time.time()
    with Executor(max_workers=workers) as ex:
        results = list(ex.map(task, jobs, chunksize=200))

    end = time.time()
    elapsed = end - start

    # print(f"Total files indexed = {len(results)}")
    # print(f"Time: {end - start:.6f} seconds")

    with index_file.open("w", encoding="utf-8") as f:
        f.write(json.dumps(results) + '\n')
    
    return elapsed


def task(job):
    record = index(job.path)

    record["arrival"] = job.arrival
    record["est_cost"] = job.est_cost
    record["queue_level"] = job.queue_level

    return record

def index(filepath: Path):
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

def build_jobs(dir_root):
    jobs = []
    tick = 0

    for dirpath, _, filenames in os.walk(dir_root):
        d = Path(dirpath)
        for fn in filenames:
            p = d / fn
            tick += 1

            # Simple estimate: larger files => higher cost (bucketed)
            try:
                size = p.stat().st_size
            except OSError:
                size = 0

            if size < 100_000:         # < 100 KB
                est = 1
            elif size < 10_000_000:    # < 10 MB
                est = 2
            else:
                est = 3

            jobs.append(Job(path=p, arrival=tick, est_cost=est, remaining=est))

    return jobs

def next_job(pool):
    if not pool:
        return None
    
    job = min(pool, key=lambda j: j.est_cost)
    pool.remove(job)
    return job


if __name__ == '__main__':
    args = sys.argv[1:]
    options = "p:t:"
    long_options = ["hash", "test"]
    default_workers = 4
    results_file = Path("benchmark_py.txt")

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
                with results_file.open("w", encoding="utf-8") as f:
                    f.write("")
                iterations = 10
                worker_interval = 1
                worker_iterations = 12
                with results_file.open("a", encoding="utf-8") as f:
                    for i in range(worker_iterations):
                        workers = 1 + (i * worker_interval)
                        times = []
                        for j in range(iterations):
                            times.append(main(ProcessPoolExecutor, workers))
                        f.write(f"Mean time of {workers} processes = {statistics.mean(times):.5f}!\n")
                        times = []
                        for j in range(iterations):
                            times.append(main(ThreadPoolExecutor, workers))
                        f.write(f"Mean time of {workers} threads = {statistics.mean(times):.5f}!\n")

    except getopt.error as err:
        print(str(err))
    
    if not RAN: 
        main(ThreadPoolExecutor, default_workers)    # Default behaviour