# Round 40: the wake path

**The plan (Mike, 28 October).** What it costs nfsd to wake idle service threads and put them back to sleep, and what can be cut without losing latency at iodepth 1. The starting point is round 39's state: the tip's sunrpc, `DDE22984B5905541BB2CD35`. Runs are on tardis2 as round 39's: build 499 with `rodata=noalias`, nfsd the kernel's, xeu round 35's, the settled setting, one loan64 run at a time on port a onto the four DPRP5s.

## Why here

In run89 (round 39's counted tip), every second of the window held:

| | a second | a WRITE |
|---|---|---|
| WRITEs | 38.7k | 1 |
| threads woken | 153.5k | **3.97** |
| context switches on nfsd's CPUs | 434.7k | **11.2** |

The threads woken a WRITE were 3.5 in round 17 before its read-on and 4.34 after it. The figures are not strictly comparable, since rounds 25 and 26 changed the passes and the idle sleep since.

In run89's profile, the switch and the wake are nfsd's largest frames past idle, by cycles and by L3 misses alike:

| frame | nfsd's cycle samples | nfsd's L3 miss samples |
|---|---|---|
| `finish_task_switch` | 7.59% | 6.95% (4.3% under `schedule_idle`) |
| `_raw_spin_unlock_irqrestore` | 2.81% | 3.78% (2.3% under `wake_up_process` from `svc_pool_wake_idle_thread`) |
| `_raw_spin_lock` | 1.60% | 2.09% |
| the region count's two frames (round 39) | about 1.1% | 3.8% |

`default_idle_call` holds 27.9% of the cycle samples, since the counter runs on in an idle CPU (WFI, no deeper state on these CPUs). So `finish_task_switch` is about a tenth of nfsd's samples outside idle.

**What the profile can't say.** This kernel is not booted with pseudo-NMI. A sample that falls while interrupts are off is taken when they are turned back on, so `finish_task_switch` and `*_unlock_irqrestore` also gather the samples of the scheduler's whole interrupts-off switch and of the wake's locked section. The sum is the cost of sleeping and waking; how it splits among those frames is not known. Some of `_raw_spin_lock`'s call chains in the profile are symbolized wrongly (`svc_deferred_recv`, TLS trace calls), so they are not ranked here.

**What has been tried before:**
- **Round 17** found 7.5 wake calls and 3.5 wakes a WRITE, 4.4 of 8.7 passes through `svc_recv` finding nothing. Its read-on is kept (a thread whose read left a loaned record incomplete reads on if `XPT_DATA` is set again).
- **Round 18** skipped the wake while a running thread was about to look for work, in three forms. None was kept: the best saved nfsd 3 to 7 us a WRITE but slowed p50 at iodepth 1. That shape is not to be tried again as it was.
- **Round 25's later wake** (`svc_tcp_rx_want_eighths` 6) is kept and settled. A partial loaned record waits for more of the window before a thread is woken.
- **Round 26** is kept: a thread its pool cannot lose sleeps without the idle timer.

**What is new since then:**
- nfsd runs 512 threads in one pool on 48 CPUs (8 to 55), about 14% busy. Every wake picks the first idle thread, wherever it last ran. Its task, stack and the lines its last request touched may be on another CPU's cache, which is what `finish_task_switch`'s L3 misses would be.
- The rest of nfsd has got cheaper (−39% since round 01). The wake path's fixed cost is now a larger share.

## The steps

1. **Where the wakes come from, and what each costs** (one traced loan64 run, not counted). `xeu-rig/wakesrc.bt` as round 17 ran it: wake calls and wakes a WRITE by source (the ring's `svc_data_ready`, the hand-back after a complete or a partial record, credit returned, `svc_thread_wake_next`), passes through `svc_recv` that find nothing, and the partial hand-backs that find `XPT_DATA` set again. The counted cycles come from a fresh untraced run beside it. A second traced window gives two things:
   - the CPU a woken thread runs on against the one it last ran on;
   - the time from the wake to the thread running (`sched_wakeup` to `sched_switch`).

   Together these say whether the cost is the number of wakes or what each wake lands on.
2. **Then, by what step 1 shows, one change at a time, each an interleaved A/B on counted cycles:**
   - **Fewer wakes from one source.** If one source dominates and its woken threads mostly find nothing or take a record already being read, it gets a change that holds that source's wake.
   - **Warmer wakes.** If woken threads mostly land on a CPU other than their last, wake the most recently idle thread first, so it is likely still warm: a LIFO idle list in place of the first idle thread. Another way is fewer threads (`NFSD_THREADS` in `settled.sh`, a settled setting: a change there is Mike's, after a run that shows why).
   - **Fewer switches per wake.** Context switches run near 2.8 a wake. If step 1 finds switches that come with no wake (preemption, a lock or a mutex sleep), those are looked at first.
3. **Every candidate is judged on latency as well as cycles** (Mike, 23 October: "latency is going to matter a lot on a per IO level"; round 18 is why). The A/B is counted nfsd and ring cycles a WRITE, with the rate and the region's peak beside it. Before a change is kept, `xeu-rig/lat.sh` runs at iodepth 1, several runs a side (L0's runs differ by about 10%).

## 1. Where the wakes come from, and what each lands on (r92)

[run92.sh](run92.sh), at the current state with no module swapped, runs two loan64 runs. The first is untraced and counted. The second is traced: from 10 s into its timed load (`timed_load`) it runs 8 s of `xeu-rig/wakesrc.bt` (wakes by source and passes through `svc_recv`, as round 17's), then 8 s of the new `xeu-rig/wakeland.bt`. For each woken nfsd thread, by whether a ring CPU or an nfsd CPU woke it, `wakeland.bt` records:
- whether it runs on the CPU it last ran on, and whether that CPU was idle;
- the time from `sched_waking` to running;
- how long it had been off a CPU.

It also records every switch on nfsd's CPUs by what goes out and what comes in (nfsd, idle, other), sleep or preemption, and nfsd's sleeps outside `svc_thread_wait_for_work()` by kernel stack.

**run92** ([run92.out](run92.out), [wakesrc-r92.txt](wakesrc-r92.txt), [wakeland-r92.txt](wakeland-r92.txt)): both runs had their flows placed and no drive stepping down.
- **The untraced run (r92-1)** is the tip as run89 counted it: 40.51 GB/s, nfsd 297.4k cycles a WRITE, rings 139.3k, peak 6447, 3.98 threads woken and 11.25 context switches a WRITE.
- **The traced run** costs nfsd 313.1k cycles a WRITE and wakes 3.72 threads a WRITE. Its figures are for proportions, not cost.

**Wakes by source** (`wakesrc.bt`, 487,561 WRITEs). A source's wake call either wakes an idle thread ("woken") or finds the first idle one already running or waking ("running").

| source | woken a WRITE | running a WRITE |
|---|---|---|
| the ring's `svc_data_ready` | **1.17** | 0.91 |
| `svc_recv`'s wake of the next thread, more queued | **0.89** | 0.71 |
| the hand-back after a complete record | 0.53 | 0.44 |
| credit returned | 0.50 | 0.36 |
| `svc_data_ready` on an nfsd CPU (the backlog) | 0.03 | 0.03 |
| the hand-back after a partial record | 0.02 | 0.02 |
| all | **3.15** | 2.47 |

- **Passes through `svc_recv`:** 6.8 a WRITE, and 3.2 of them dequeue a transport.
- **Woken for nothing:** 0.51 a WRITE were woken from sleep and found nothing, as in round 17 (0.55). 2.98 a WRITE more found nothing without having slept.
- **Since round 17,** the partial record's hand-back wakes 0.02 a WRITE where it woke 0.27, thanks to round 25's later wake. The ring's own wakes are about the same.

**Where a woken thread lands** (`wakeland.bt`, 386,306 WRITEs): 3.22 nfsd threads run after a wake each WRITE. That counts every wake, not only the pool's: 0.62 are woken from a ring CPU and 2.60 from an nfsd CPU.

| | share of wakes |
|---|---|
| runs on the CPU it last ran on | **97.0%** |
| that CPU was idle | 98.3% |
| `sched_waking` to running, 4–8 us | 89% (ring's wakes), 90% (nfsd's) |
| 8–16 us | 10%, 9.5% |

- **The wakes are not landing cold.** A woken thread almost always runs where it last ran, on a CPU that was idle. Waking the newest idle thread first, or running fewer threads, would change little. `finish_task_switch`'s misses are not a migrated thread's.
- **Waking an idle CPU takes 4 to 8 us** under load, traced. The CPU is in WFI (these CPUs have no other idle state) and needs an IPI. That time is latency, not CPU, but each WRITE has several wakes in series. It is much more than `lat.bt`'s 0–2 us for a thread woken to running (L0), which measures a different span at iodepth 1. A run that times both is needed before anything is built on either.
- **The thread was off its CPU** 4 to 64 us in 59% of wakes, and 0.5 to 2 ms in 22%.

**Every switch on nfsd's CPUs** (2.84 million in the window, 7.4 a WRITE): 7.1 a WRITE are an nfsd thread going to sleep with the CPU going idle, or back. Preemption is under 0.001 a WRITE. Other tasks (kworkers and the like) make 0.27 a WRITE.

**nfsd's sleeps outside the pool's wait**, 1.27 a WRITE of its 3.57:

| where | a WRITE |
|---|---|
| `io_schedule_timeout`: the direct write waiting for the drive | **0.87** |
| `lock_sock`: another thread holds the socket | **0.15** |
| XFS log force (`xlog_wait_on_iclog`, `xlog_wait`) | 0.17 |
| `__wait_for_common` | 0.08 |
| an rwsem or mutex | 0.006 |

**What step 1 says for step 2:**
- **Not warmer wakes.** 97% already land warm.
- **The ring's wake and the next-thread wake** are 2.06 of the 3.15 wakes a WRITE. The 0.51 a WRITE woken for nothing are most likely next-thread wakes whose work a running thread took first. Round 18's way of holding those back cost latency. Linking each empty pass to the wake that caused it would say which source to change.
- **`lock_sock`**: 0.15 sleeps and wakes a WRITE spent waiting for the socket lock, a thread sending a reply or reading while another holds the socket.
- **The drive wait (0.87) and the log forces (0.17)** are the WRITE's own. The log forces come from extents allocated anew after each run's trim, and an overwrite workload would have fewer.
- **Wake-to-run latency**: whether 4 to 8 us is real untraced, and where it goes (IPI, WFI exit, the schedule).
