# ServiceWatch

Bounded concurrent TCP availability checks in C++17. Observe IPv4/IPv6 services with consecutive-failure and recovery thresholds, deterministic configuration-order JSON Lines, and explicit exit codes.

## Build and run

Requires CMake 3.20+, a C++17 compiler and Python 3 for tests.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
./build/servicewatch --config examples/local.csv --rounds 3
```

On Windows with Visual Studio use `build/Release/servicewatch.exe`; Ninja builds use `build/servicewatch.exe`.

Configuration is `name,numeric-IP,port`, one target per line. Blank lines and lines beginning with # are ignored. DNS names and scoped IPv6 addresses are deliberately unsupported. Only check services you operate or have permission to test.

`--help` lists bounds. Defaults: 4 workers, 1000 ms connection timeout, 1 round, 1000 ms delay between completed rounds, 2 consecutive failures to DOWN and 1 success to UP. Initial state is UNKNOWN. Counters saturate at their thresholds. A transient failure can retain UP; inspect `reachable` for the latest observation.

Exit 0 means all final states UP; 1 means at least one UNKNOWN/DOWN; 2 means invalid options/configuration; 3 means runtime or I/O failure. Output from earlier completed rounds remains valid if a later round fails. Always inspect exit status. Output errors can leave partial lines.

## Engineering

- RAII closes sockets and balances Winsock initialization.
- Nonblocking connect uses a steady-clock deadline, Windows select or POSIX poll, then SO_ERROR verification.
- At most 16 workers and 64 configured targets; bounded 64 KiB configuration and 256-byte lines.
- Workers write separate result slots; state transitions and output happen only after joining. Worker exceptions propagate after all threads are joined.
- TCP connect success does not prove HTTP, TLS, database, or application health. No credentials or payload are sent.

This portfolio tool is not a persistent monitoring service. No DNS, daemon mode, alert delivery, persistent state, HTTP probes, or production uptime guarantee. Scheduling delays and OS overhead can exceed a configured timeout. A new process starts UNKNOWN. JSON output includes target addresses; review before sharing.

See [verification](docs/VERIFICATION.md). Learning notes are maintained separately from this repository.
