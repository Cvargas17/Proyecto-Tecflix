import concurrent.futures
import statistics
import sys
import time
import urllib.request
import urllib.error

def worker(base_url: str, requests_per_user: int):
    latencies_ms = []
    errors = 0
    for _ in range(requests_per_user):
        t0 = time.perf_counter()
        try:
            with urllib.request.urlopen(f"{base_url}/api/videos?page=1&limit=12", timeout=5) as resp:
                resp.read()
                if resp.status >= 400:
                    errors += 1
        except Exception:
            errors += 1
        latencies_ms.append((time.perf_counter() - t0) * 1000.0)
    return latencies_ms, errors

def run_benchmark(base_url: str, concurrency: int, reqs_per_user: int):
    t_start = time.perf_counter()
    all_latencies = []
    total_errors = 0

    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(worker, base_url, reqs_per_user) for _ in range(concurrency)]
        for f in concurrent.futures.as_completed(futures):
            lats, errs = f.result()
            all_latencies.extend(lats)
            total_errors += errs

    elapsed = time.perf_counter() - t_start
    total_reqs = len(all_latencies)
    all_latencies.sort()

    p50 = statistics.median(all_latencies)
    p95 = all_latencies[int(0.95 * (total_reqs - 1))]
    p99 = all_latencies[int(0.99 * (total_reqs - 1))]
    rps = total_reqs / elapsed

    print(
        f"Users={concurrency:3d} | Total={total_reqs:5d} | RPS={rps:7.1f} req/s | "
        f"p50={p50:6.2f} ms | p95={p95:6.2f} ms | p99={p99:6.2f} ms | Errors={total_errors}"
    )

if __name__ == "__main__":
    target = sys.argv[1] if len(sys.argv) > 1 else "http://localhost:8080"
    print(f"=== Load Test TecFlix -> {target} ===")
    for users in [10, 25, 50, 100, 200]:
        run_benchmark(target, concurrency=users, reqs_per_user=20)