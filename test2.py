import threading
import time # Import the time module
 
def cpu_heavy():
    count = 0
    while count < 100000000:
        count += 1
 
# Record the start time
start_time = time.perf_counter()
 
t1 = threading.Thread(target=cpu_heavy)
t2 = threading.Thread(target=cpu_heavy)
 
# Start the threads
t1.start()
t2.start()
 
# Wait for both threads to finish
t1.join()
t2.join()
 
# Record the end time
end_time = time.perf_counter()
 
# Calculate and print the elapsed time
elapsed_time = end_time - start_time
print(f"Time taken to run: {elapsed_time:0.4f} seconds")