# Stock 9.1.3 baselines

Built from `disk-catchup` (stock 9.1.3) on an 8-core x86 laptop that was
also running other jobs (load average 5 to 8 during these runs). One run per
row, so read these as rough sizes, not exact numbers. Dataset 256 MB from
`DEBUG POPULATE`, 1 KB values.

## H1: gap size vs recovery

| gap | backlog | answer | bytes sent | recovery |
| --- | --- | --- | --- | --- |
| 5.5 MB | 10 MB | partial | 5.5 MB | 69 ms |
| 21.9 MB | 10 MB | full | 32.8 MB | 8.2 s |
| 54.7 MB | 10 MB | full | 63.7 MB | 7.0 s |
| 109.5 MB | 10 MB | full | 113.3 MB | 8.4 s |
| 21.9 MB | 128 MB | partial | 21.9 MB | 103 ms |
| 54.7 MB | 128 MB | partial | 54.7 MB | 242 ms |
| 109.5 MB | 128 MB | partial | 109.5 MB | 459 ms |

Once the gap passes 10 MB, stock falls back to a full sync. The full sync rows
include the default 5 s `repl-diskless-sync-delay`. The RDB is small here
because `DEBUG POPULATE` values compress well. With `--rdbcompression no` the
54.7 MB gap sent 324 MB in 8.1 s, and 3.0 s with the delay set to 0.

The 128 MB backlog rows are baseline 2 (a bigger ring). They are what DISK
catch-up should get close to, without keeping 128 MB of backlog in memory.

## H3: replicas missing the same gap

| replicas | gap | answer | bytes sent | recovery |
| --- | --- | --- | --- | --- |
| 1 | 54.7 MB | 1 full | 63.7 MB | 7.3 s |
| 3 | 54.7 MB | 3 full | 191.0 MB | 8.2 s |

## H2: steady state

`bench.sh steady 300000`, 2 replicas attached, 50 clients, 1 KB SETs:
47,946 SET/s, p50 0.655 ms, p99 1.463 ms, used_memory 349 MB, backlog 10 MB.

## Milestone sweep: write rate and outage length

`RPS=<rate×1024> bench.sh gap <rate×seconds>`, run 2026-11-26 on that laptop (load
average about 3), 10 MB backlog, 256 MB dataset, one run per row. 1 MB here is
1,048,576 bytes, the same as `repl-backlog-size 10mb`.

| rate | outage | gap | answer | bytes sent | recovery | data match |
| --- | --- | --- | --- | --- | --- | --- |
| 2 MB/s | 1.0 s | 2.1 MB | partial | 2.1 MB | 0.01 s | yes |
| 2 MB/s | 2.0 s | 4.2 MB | partial | 4.2 MB | 0.05 s | yes |
| 2 MB/s | 4.0 s | 8.4 MB | partial | 8.4 MB | 0.06 s | yes |
| 2 MB/s | 8.0 s | 16.7 MB | full | 27.2 MB | 8.03 s | yes |
| 5 MB/s | 1.0 s | 5.2 MB | partial | 5.2 MB | 0.05 s | yes |
| 5 MB/s | 2.0 s | 10.4 MB | full | 21.2 MB | 7.99 s | yes |
| 5 MB/s | 4.0 s | 20.9 MB | full | 31.2 MB | 7.90 s | yes |
| 5 MB/s | 8.0 s | 41.8 MB | full | 51.0 MB | 6.98 s | yes |
| 10 MB/s | 1.0 s | 10.4 MB | full | 21.2 MB | 7.22 s | yes |
| 10 MB/s | 2.0 s | 20.9 MB | full | 31.2 MB | 7.06 s | yes |
| 10 MB/s | 4.0 s | 41.8 MB | full | 51.0 MB | 7.01 s | yes |
| 10 MB/s | 8.0 s | 83.5 MB | full | 89.3 MB | 7.24 s | yes |

The 10 MB backlog stops being enough as soon as the gap passes 10 MB: 8.4 MB
stays partial at 2 MB/s, and 10.4 MB goes full at both 5 and 10 MB/s. That is
an outage of about 5 s at 2 MB/s, 2 s at 5 MB/s and 1 s at 10 MB/s. Every full
sync takes 7 to 8 s whatever the gap, most of it the 5 s diskless delay, and
every run's data matched the primary after recovery.

## In-sync replica latency during recovery (H3 baseline)

`INSYNC=1 bench.sh gap 50 <replicas>`: one more replica stays connected while
the others recover, and SET plus `WAIT 1` is timed every 10 ms.

| replicas | gap | answer | bytes sent | recovery | in-sync p50 | in-sync p99 | in-sync max | data match |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 52.2 MB | 1 full | 60.8 MB | 7.11 s | 0.280 ms | 40.535 ms | 41.768 ms | yes |
| 3 | 52.2 MB | 3 full | 182.4 MB | 9.16 s | 0.293 ms | 20.890 ms | 42.033 ms | yes |

On stock the recovering replicas take full syncs, and the in-sync replica's p99
stays around 20 to 40 ms, with a max near 42 ms in both runs. The DISK build
should be compared against these rows.
