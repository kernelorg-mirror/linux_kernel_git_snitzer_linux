import sys, collections
sys.path.insert(0,'/dev/shm'); from areas133 import load
ch=load(sys.argv[1]); n=len(ch); iops=float(sys.argv[2])
us=lambda k: 100*k/n*0.01*64/iops
skip={'_raw_spin_lock','_raw_spin_lock_irqsave','_raw_spin_lock_bh','_raw_spin_unlock_irqrestore','_raw_spin_unlock_irq','_raw_spin_unlock_bh','arch_counter_get_cntpct','ktime_get','down_read_trylock','__up_read','percpu_counter_add_batch','queued_spin_lock_slowpath','native_queued_spin_lock_slowpath','ktime_get_real_ts64','ktime_get_mono_fast_ns','read_seqcount_begin','local_clock','sched_clock','arch_timer_read_counter'}
for leaf in sys.argv[3:]:
    c=collections.Counter()
    for x in ch:
        if x[0]!=leaf: continue
        p=[f for f in x[1:] if f not in skip][:2]
        c[' < '.join(p)]+=1
    tot=sum(c.values()); print(f"== {leaf}: {us(tot):.3f} us")
    for k,v in c.most_common(6): print(f"   {us(v):.3f}  {k}")
