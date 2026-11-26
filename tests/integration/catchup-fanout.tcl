# Shared catch-up read (P4). Until the suffix file (P2) lands no DISK catch-up
# can start, so these check the configs, the INFO counters, and that a gap past
# the backlog still ends in a full sync with no catch-up bytes read.

proc fanout_counters {r} {
    set out {}
    foreach f {catchup_groups_waiting catchup_disk_bytes_read catchup_bytes_sent
               catchup_shared_reads catchup_waiters_coalesced catchup_fallbacks} {
        lappend out [status $r $f]
    }
    return $out
}

start_server {tags {"repl external:skip"} overrides {save {}}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {Catch-up configs round trip and reject bad values} {
        assert_equal 1000 [lindex [$primary config get repl-catchup-delay-ms] 1]
        $primary config set repl-catchup-delay-ms 0
        assert_equal 0 [lindex [$primary config get repl-catchup-delay-ms] 1]
        assert_error {*} {$primary config set repl-catchup-delay-ms -1}
        assert_error {*} {$primary config set repl-catchup-delay-ms 60001}
        $primary config set repl-catchup-delay-ms 1000
    }

    test {Catch-up counters start at zero} {
        assert_equal {0 0 0 0 0 0} [fanout_counters $primary]
    }

    start_server {overrides {save {}}} {
        set r1 [srv 0 client]
        set r1_pid [srv 0 pid]
        start_server {overrides {save {}}} {
            set r2 [srv 0 client]
            set r2_pid [srv 0 pid]

            $primary config set repl-backlog-size 1mb
            $primary config set repl-diskless-sync-delay 0
            $r1 replicaof $primary_host $primary_port
            $r2 replicaof $primary_host $primary_port
            wait_for_ofs_sync $primary $r1
            wait_for_ofs_sync $primary $r2

            test {Connected replicas under writes do no catch-up work} {
                for {set i 0} {$i < 2000} {incr i} { $primary set k$i [string repeat x 100] }
                wait_for_ofs_sync $primary $r1
                wait_for_ofs_sync $primary $r2
                assert_equal {0 0 0 0 0 0} [fanout_counters $primary]
            }

            test {Two replicas past the backlog fall back to full sync with no suffix} {
                set full_before [status $primary sync_full]
                pause_process $r1_pid
                pause_process $r2_pid
                $primary client kill type replica
                for {set i 0} {$i < 3000} {incr i} { $primary set g$i [string repeat y 1000] }
                resume_process $r1_pid
                resume_process $r2_pid
                wait_for_ofs_sync $primary $r1
                wait_for_ofs_sync $primary $r2
                assert_equal [expr {$full_before + 2}] [status $primary sync_full]
                assert_equal {0 0 0 0 0 0} [fanout_counters $primary]
            }
        }
    }
}
