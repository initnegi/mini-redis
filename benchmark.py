import socket
import threading
import time

HOST = "127.0.0.1"
PORT = 6379

NUM_CLIENTS = 100          # number of concurrent simulated clients
OPS_PER_CLIENT = 500       # commands each client sends

# Shared counters (protected by a lock since multiple threads update them)
results_lock = threading.Lock()
total_ops_completed = 0


def run_client(client_id, results):
    """Each thread runs this: opens its own connection, fires a mix of SET/GET commands."""
    global total_ops_completed

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.connect((HOST, PORT))

        ops_done = 0
        for i in range(OPS_PER_CLIENT):
            key = f"bench:{client_id}:{i % 50}"   # reuse ~50 keys per client to mix SET/GET realistically
            if i % 2 == 0:
                cmd = f"SET {key} value{i}\r\n"
            else:
                cmd = f"GET {key}\r\n"

            s.sendall(cmd.encode())
            response = s.recv(4096)  # wait for response before sending next command
            if not response:
                break
            ops_done += 1

        results[client_id] = ops_done

    with results_lock:
        total_ops_completed += ops_done


def main():
    print(f"Starting benchmark: {NUM_CLIENTS} concurrent clients, {OPS_PER_CLIENT} ops each")
    print(f"Target: {HOST}:{PORT}\n")

    results = {}
    threads = []

    start_time = time.perf_counter()

    for client_id in range(NUM_CLIENTS):
        t = threading.Thread(target=run_client, args=(client_id, results))
        threads.append(t)
        t.start()

    for t in threads:
        t.join()  # wait for ALL clients to finish before stopping the timer

    end_time = time.perf_counter()
    elapsed = end_time - start_time

    total_ops = sum(results.values())
    ops_per_sec = total_ops / elapsed if elapsed > 0 else 0

    print(f"Total operations completed: {total_ops}")
    print(f"Total wall-clock time:      {elapsed:.3f} sec")
    print(f"Throughput:                 {ops_per_sec:.1f} ops/sec")
    print(f"Average latency per op:     {(elapsed / total_ops * 1000):.3f} ms")


if __name__ == "__main__":
    main()
