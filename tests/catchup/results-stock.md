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
