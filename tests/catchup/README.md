# Catch-up benchmarks (H1 to H3)

`bench.sh` starts a primary and replicas on local ports, loads a dataset with
`DEBUG POPULATE`, and measures one scenario. Each run starts fresh servers in a
temp folder and removes them at the end.

```sh
make -C src valkey-server valkey-cli valkey-benchmark
export VALKEY_SRC=$PWD/src
tests/catchup/bench.sh gap 50        # H1: one replica misses 50 MB of writes
tests/catchup/bench.sh gap 50 3      # H3: three replicas miss the same 50 MB
tests/catchup/bench.sh steady        # H2: SET throughput and latency, 2 replicas attached
```

`gap` stops the replica processes, drops their connections from the primary,
writes about `<gap-mb>` of SETs, then resumes the replicas and times how long it
takes until every replica's offset matches the primary's. It prints one CSV
row:

| column | meaning |
| --- | --- |
| `gap_bytes` | replication stream bytes written while the replicas were away |
| `full_syncs`, `partial_syncs` | how the primary answered the reconnects |
| `repl_out_bytes` | bytes the primary sent to replicas during recovery |
| `disk_bytes_read` | suffix bytes read for catch-up (blank on builds without the fanout INFO fields from #4) |
| `recovery_ms` | resume to fully caught up, including the replica's reconnect delay of up to 1 s |

Settings come from the environment:

| variable | default | meaning |
| --- | --- | --- |
| `VALKEY_SRC` | required | folder with the three binaries |
| `BACKLOG` | `10mb` | primary `repl-backlog-size` |
| `DATASET_MB` | `256` | dataset size, which is what a full sync sends |
| `VALUE` | `1024` | value size in bytes |
| `PRIMARY_ARGS` | empty | extra primary config, for example `--repl-catchup-max-mbps 50` |
| `REPLICAS` | `2` | replicas attached during `steady` |
| `BASE_PORT` | `21600` | primary port; replicas use the next ports |

## Baselines

Run each hypothesis against the three baselines from the proposal:

1. Stock 9.1.3 with the 10 MB backlog: build `disk-catchup` and point
   `VALKEY_SRC` at it.
2. Stock with the backlog raised to the disk cap: same build with
   `BACKLOG=<cap>`.
3. Our build with the suffix off (full sync whenever the ring misses).

`DEBUG POPULATE` values compress well, so a full sync sends a much smaller RDB
than the dataset size. Add `PRIMARY_ARGS="--rdbcompression no"` for the
worst case. Full syncs also wait the default 5 s `repl-diskless-sync-delay`
unless `PRIMARY_ARGS` sets it.

H1 compares `repl_out_bytes` and `recovery_ms` against `gap_bytes` as the gap
grows past 10 MB. H2 compares `steady` throughput and p50/p99 latency, and
checks that `used_memory` matches stock and `suffix_bytes_read` stays 0. H3
compares `disk_bytes_read` with one copy of the gap as `replicas` grows.

Stock results on one laptop are in `results-stock.md`.
