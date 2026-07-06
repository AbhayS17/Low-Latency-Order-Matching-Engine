# High-Frequency Limit Order Book (LOB)

A low-latency, deterministic matching engine built in modern C++. Designed to simulate real-world market microstructure, this engine handles limit and market order execution, partial fills, and liquidity dry-up scenarios under heavy concurrent loads.

## Performance Benchmarks
Tested with 1,000,000 randomized orders (heavy concentration around mid-price to force spread crossing).

* **Peak Throughput:** 8.9+ million orders/sec
* **Average Latency:** 112 nanoseconds/order

> **Note on I/O operations:** These benchmarks were recorded with terminal I/O disabled to measure the pure algorithmic and memory performance of the engine. To observe the real-time matching logic, partial fills, and trade executions, you can uncomment the `std::cout` statements inside the matching loops. 
> 
> *(Note: Enabling standard terminal output will introduce system-level I/O bottlenecks, artificially reducing throughput to ~175,000 orders/sec).*

## System Architecture
* **The Control Tower (Instant Cancellations):** Utilizes a `std::unordered_map` paired with a custom Doubly Linked List to achieve $O(1)$ order cancellations, bypassing standard array shifting bottlenecks.
* **Price-Time Priority (The Train Tracks):** Price levels are maintained in a Red-Black Tree (`std::map`), ensuring $O(\log N)$ access to the best bid/ask, while individual orders queue in $O(1)$ time at the tail of the list.
* **Memory Safety:** Strictly enforces memory safety and eliminates cyclic leaks using `std::shared_ptr` and `std::weak_ptr` for node coupling. 

## Build and Run
```bash
g++ -O3 main.cpp -o engine
./engine