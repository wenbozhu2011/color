# Color verification — build & run

Proves the Color protocol's safety and liveness by running the client and
server over a simulated lossy network (drop / duplicate / delay / reorder) and
checking the results across many random seeds.

## Requirements

- CMake ≥ 3.16
- A C++17 compiler (g++ or clang++)

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

```sh
./build/verification/color_verify
```

Expected output ends with `ALL RUNS PASSED`. The exit code is non-zero if any
run fails.

Or run the bundled test suite:

```sh
ctest --test-dir build --output-on-failure
```

## Options (all optional)

```
--seeds N        number of random seeds to run       (default 100)
--rate f         mean new requests per tick (Poisson) (default 2.0)
--parallel P     max requests in flight at once        (default 8)
--drop f         probability a message is dropped      (default 0.30)
--dup f          probability a message is duplicated   (default 0.10)
--verbose        print a per-message event trace
```

Examples:

```sh
./build/verification/color_verify --seeds 500          # more seeds
./build/verification/color_verify --drop 0.8 --dup 0.5 # harsher network
./build/verification/color_verify --seeds 1 --steps 6 --verbose  # watch events
```

For what is being checked and why, see `plan.md`.
