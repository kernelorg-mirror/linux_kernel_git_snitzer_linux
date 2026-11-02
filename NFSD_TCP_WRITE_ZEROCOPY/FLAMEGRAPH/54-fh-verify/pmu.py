#!/usr/bin/python3
# pmu.py SCRIPTOUT: cycles, IPC and L2 refills by symbol, for the fh path.
import collections, sys
want = """__fh_verify fh_verify nfsd_set_fh_dentry nfsd_file_do_acquire nfsd_file_lookup_locked
nfsd_file_get nfsd_file_put exportfs_decode_fh_raw xfs_fs_fh_to_dentry xfs_nfs_get_inode xfs_iget
xfs_iget_cache_hit xfs_verify_ino xfs_group_get xfs_group_put __radix_tree_lookup d_obtain_alias
__d_obtain_alias __d_find_any_alias rqst_exp_find exp_find exp_find_key exp_get_by_name
sunrpc_cache_lookup_rcu sunrpc_cache_find_rcu cache_check_rcu cache_check full_name_hash
nfsd_setuser_and_check_port nfsd_setuser lockref_put_return dput iput fh_put
svc_tcp_rx_append_ref svc_tcp_rx_borrow_segment""".split()
c = collections.defaultdict(collections.Counter)
for l in open(sys.argv[1]):
    f = l.split()
    if len(f) < 3:
        continue
    period, ev, sym = f[0], f[1].rstrip(':').split(':')[0], f[2]
    try:
        c[ev][sym] += int(period)
    except ValueError:
        pass
T = {e: sum(v.values()) or 1 for e, v in c.items()}
print('events', {e: T[e] for e in T})
cy, ins, rf = 'cycles', 'instructions', 'l2d_cache_refill'
print(f"{'symbol':32} {'%cyc':>6} {'IPC':>5} {'refills/kI':>10} {'%refill':>7}")
print(f"{'ALL':32} {100.0:6.2f} {T[ins]/T[cy]:5.2f} {1000*T[rf]/T[ins]:10.1f} {100.0:7.2f}")
for s in sorted(want, key=lambda s: -c[cy][s]):
    if c[cy][s]:
        i = c[ins][s] or 1
        print(f"{s:32} {100*c[cy][s]/T[cy]:6.2f} {c[ins][s]/c[cy][s]:5.2f} {1000*c[rf][s]/i:10.1f} {100*c[rf][s]/T[rf]:7.2f}")
