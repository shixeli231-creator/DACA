# DACA relationship-memory runtime

This is an opt-in Linux user-space prototype of DACA's remembering subsystem. It keeps its live relationship library in the daemon process's RAM, with a configurable memory budget. The Unix socket is only a communication endpoint; it is not where the cached records are stored.

The runtime has three safe reuse paths:

- Exact reuse works for any application operation when its operation name, context, semantic version, input identities, input versions, and input values all match.
- `sum_i64` supports non-contiguous, cross-record composition because integer addition is explicitly declared commutative and validated.
- `concat_utf8` supports ordered composition from saved pieces and rejects reordered inputs.

Every record includes a context string and input dependency versions. A client can invalidate a complete context or all records that depend on one changed input ID. The runtime does not guess arbitrary transformations from output values; a new relationship kind needs an explicit schema with matching and validation rules in the source.

## Run the demo

```sh
cd daca_relationship_runtime
chmod +x run_demo.sh
./run_demo.sh
```

The demo starts a daemon with a 32 MiB budget, sends requests through its Unix socket, verifies exact reuse, non-contiguous sum composition, ordered text composition, dependency invalidation, and caller-defined exact reuse. It exits nonzero if any expected result is wrong.

Compile manually:

```sh
g++ -O3 -std=c++17 -Wall -Wextra -Wpedantic -pthread \
  daca_relationship_runtime.cpp -o daca_relationship_runtime
```

Start a daemon manually:

```sh
./daca_relationship_runtime --daemon --memory-mib 128
```

In another terminal, run the built-in client demo:

```sh
./daca_relationship_runtime --demo
```

Set `DACA_SOCKET` or pass `--socket PATH` to choose a socket. By default it uses `$XDG_RUNTIME_DIR/daca-relationship.sock`, or a per-user `/tmp` fallback.

## Per-user systemd service

The included unit is deliberately a **user service**, not a root service and not a global `LD_PRELOAD` hook. Build and install the executable, then enable it for your account:

```sh
mkdir -p ~/.local/bin ~/.config/systemd/user
cp daca_relationship_runtime ~/.local/bin/
cp daca-relationship.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now daca-relationship.service
systemctl --user status daca-relationship.service
```

The unit listens at `%t/daca-relationship.sock`, which normally expands to your user runtime directory, and limits the daemon to 256 MiB. The daemon itself uses 128 MiB as its record budget.

## Using this with real software

An application needs an adapter that sends structured requests and learns only verified direct-computation outputs. Its request context must include every condition that changes correctness: application and rule version, data shape and precision where relevant, model/asset/world version, and domain-specific state.

For Java Minecraft, use a Java client or an opt-in mod/agent. Java's JIT generally does not route game math through a C++ `LD_PRELOAD` `pow()` hook. Batch work at meaningful task boundaries such as deterministic chunk/noise components, and use a small local JVM cache before IPC. Do not reuse changing game state unless the adapter supplies dependency versions and invalidates it correctly.

This prototype proves the request, record, composition, validation, invalidation, and memory-management path. It does not measure DACA chip performance or guarantee a speedup for Minecraft or ordinary desktop programs.
