# Disk catch-up (P1): hold table and decide step.
#
# A "hold" is recorded on the primary when an ONLINE replica disconnects, and
# removed when the replica comes back (MEMORY or FULL), when it expires
# (catchup-hold-ttl), or when its replication history becomes invalid.
# Until the suffix file (P2) lands, a PSYNC whose offset was trimmed from the
# backlog must still end in a FULL resync, exactly like stock Valkey.

# Offset the first replica acknowledged, as reported by the primary.
proc replica_ack_offset {primary} {
    if {[regexp {slave0:[^\r\n]*offset=([0-9]+)} [$primary info replication] -> off]} {
        return $off
    }
    return 0
}

# Write something and wait until the replica has ACKed all of it, so that
# repl_ack_off > 0 (holds are only created for replicas that ACKed).
proc wait_replica_acked {primary} {
    $primary incr catchup-counter
    wait_for_condition 50 100 {
        [replica_ack_offset $primary] > 0 &&
        [replica_ack_offset $primary] == [status $primary master_repl_offset]
    } else {
        fail "Replica did not acknowledge the primary offset"
    }
}

# Freeze the replica process (so it cannot reconnect), then drop its link.
proc disconnect_paused_replica {primary replica_pid} {
    pause_process $replica_pid
    $primary client kill type replica
    wait_for_condition 50 100 {
        [status $primary connected_slaves] == 0
    } else {
        fail "Replica link was not dropped"
    }
}

start_server {tags {"repl external:skip"} overrides {save {}}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]
    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 0

    start_server {overrides {save {}}} {
        set replica [srv 0 client]
        set replica_pid [srv 0 pid]

        $replica replicaof $primary_host $primary_port
        wait_replica_online $primary
        wait_replica_acked $primary

        test {No hold while the replica stays connected} {
            for {set i 0} {$i < 100} {incr i} { $primary set k$i v$i }
            wait_replica_acked $primary
            assert_equal 0 [status $primary catchup_holds]
            assert_equal -1 [status $primary catchup_holds_min_offset]
        }

        test {MONITOR client disconnect does not create a hold} {
            set rd [valkey_deferring_client -1]
            $rd monitor
            $rd read ;# +OK
            $rd close
            wait_for_condition 50 100 {
                [llength [regexp -all -inline {cmd=monitor} [$primary client list]]] == 0
            } else {
                fail "MONITOR client was not closed"
            }
            assert_equal 0 [status $primary catchup_holds]
        }

        test {Disconnect creates a hold, MEMORY reconnect clears it} {
            wait_replica_acked $primary
            set ack [replica_ack_offset $primary]
            set partial_before [status $primary sync_partial_ok]

            disconnect_paused_replica $primary $replica_pid
            assert_equal 1 [status $primary catchup_holds]
            assert_equal [expr {$ack + 1}] [status $primary catchup_holds_min_offset]

            resume_process $replica_pid
            wait_for_condition 50 100 {
                [status $primary sync_partial_ok] == $partial_before + 1
            } else {
                fail "Replica did not partially resync"
            }
            wait_replica_online $primary
            assert_equal 0 [status $primary catchup_holds]
        }

        test {Hold expires after catchup-hold-ttl} {
            $primary config set catchup-hold-ttl 1
            wait_replica_acked $primary

            disconnect_paused_replica $primary $replica_pid
            assert_equal 1 [status $primary catchup_holds]
            wait_for_condition 50 100 {
                [status $primary catchup_holds] == 0
            } else {
                fail "Hold did not expire"
            }

            resume_process $replica_pid
            wait_replica_online $primary
            $primary config set catchup-hold-ttl 3600
        }

        test {Trimmed offset still gives FULL resync until the suffix file lands} {
            $primary config set repl-backlog-size 16384
            wait_replica_acked $primary
            set full_before [status $primary sync_full]
            set partial_err_before [status $primary sync_partial_err]

            disconnect_paused_replica $primary $replica_pid
            assert_equal 1 [status $primary catchup_holds]

            # Write well past the backlog size so the replica's offset is trimmed.
            set val [string repeat x 100]
            for {set i 0} {$i < 3000} {incr i} { $primary set gap$i $val }
            wait_for_condition 50 100 {
                [status $primary repl_backlog_first_byte_offset] > [status $primary catchup_holds_min_offset]
            } else {
                fail "Backlog was not trimmed past the held offset"
            }
            # Without the suffix file, trimming does not remove the hold.
            assert_equal 1 [status $primary catchup_holds]

            resume_process $replica_pid
            wait_for_condition 100 100 {
                [status $primary sync_full] == $full_before + 1
            } else {
                fail "Replica did not fully resync"
            }
            assert_equal [expr {$partial_err_before + 1}] [status $primary sync_partial_err]
            wait_replica_online $primary
            assert_equal 0 [status $primary catchup_holds]

            wait_for_ofs_sync $primary $replica
            assert_equal [$primary dbsize] [$replica dbsize]
        }
    }
}

# Chained replication: A <- B <- C. When B itself needs a FULL resync from A,
# replicationAttachToNewPrimary() on B disconnects C (which creates a hold on
# B) and then frees B's backlog. B keeps the same replication ID (it is still
# on A's history), so only freeing the backlog can tell us the hold is stale.
start_server {tags {"repl external:skip"} overrides {save {}}} {
    set A [srv 0 client]
    set A_host [srv 0 host]
    set A_port [srv 0 port]
    $A config set repl-diskless-sync yes
    $A config set repl-diskless-sync-delay 0

    start_server {overrides {save {}}} {
        set B [srv 0 client]
        set B_host [srv 0 host]
        set B_port [srv 0 port]
        set B_pid [srv 0 pid]
        $B config set repl-diskless-sync yes
        $B config set repl-diskless-sync-delay 0

        start_server {overrides {save {}}} {
            set C [srv 0 client]
            set C_pid [srv 0 pid]

            $B replicaof $A_host $A_port
            wait_replica_online $A
            $C replicaof $B_host $B_port
            wait_replica_online $B

            test {Holds are cleared when a chained replica's backlog is freed} {
                # Make sure C has ACKed on B (a hold needs repl_ack_off > 0).
                $A incr catchup-counter
                wait_for_condition 50 100 {
                    [status $B master_repl_offset] == [status $A master_repl_offset] &&
                    [replica_ack_offset $B] > 0 &&
                    [replica_ack_offset $B] == [status $B master_repl_offset]
                } else {
                    fail "C did not acknowledge on B"
                }
                assert_equal 0 [status $B catchup_holds]

                $A config set repl-backlog-size 16384
                set full_before [status $A sync_full]

                # Freeze C first so it cannot reconnect to B and consume the
                # hold, then freeze B and push A past its backlog so B needs
                # a FULL resync.
                pause_process $C_pid
                pause_process $B_pid
                $A client kill type replica
                wait_for_condition 50 100 {
                    [status $A connected_slaves] == 0
                } else {
                    fail "B's link to A was not dropped"
                }
                set val [string repeat x 100]
                for {set i 0} {$i < 3000} {incr i} { $A set gap$i $val }

                resume_process $B_pid
                wait_for_condition 100 100 {
                    [status $A sync_full] == $full_before + 1 &&
                    [status $B master_link_status] eq {up}
                } else {
                    fail "B did not fully resync with A"
                }

                # B dropped C while attaching to A and freed its backlog: the
                # hold for C would point into a history B can no longer serve.
                assert_equal 0 [status $B catchup_holds]
                assert_equal -1 [status $B catchup_holds_min_offset]

                resume_process $C_pid
                wait_replica_online $B
                wait_for_ofs_sync $B $C
                assert_equal [$A dbsize] [$C dbsize]
            }
        }
    }
}
