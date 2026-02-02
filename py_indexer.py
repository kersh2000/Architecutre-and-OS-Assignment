"""
Docstring for py_indexer_thread
"""

import time, os, sys, getopt
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
from concurrent import futures

RAN = False

def main(Executor: futures, workers: int):
    global RAN
    RAN = True

    exType = "Multiprocessing" if Executor is ProcessPoolExecutor else "Multithreading"
    print(f"{exType} with {workers} workers...")

    numbers = [10000000, 20000000, 30000000, 40000000]

    start = time.time()
    with Executor(max_workers=workers) as ex:
        results = list(ex.map(task, numbers))

    end = time.time()

    print("Results: ", results)
    print(f"Time: {end - start:.2f} seconds")

def task(n):
    return sum(i * i for i in range (n))


if __name__ == '__main__':
    args = sys.argv[1:]
    options = "p:t:"
    long_options = ["hash"]
    default_workers = 4

    try:
        arguments, values = getopt.getopt(args, options, long_options)
        for currArg, currVal in arguments: 
            if currArg == "-p":
                workers = int(currVal) if currVal else default_workers
                main(ProcessPoolExecutor, workers)
            elif currArg == "-t":
                workers = int(currVal) if currVal else default_workers
                main(ThreadPoolExecutor, workers)

    except getopt.error as err:
        print(str(err))
    
    if not RAN: 
        main(ThreadPoolExecutor, default_workers)    # Default behaviour