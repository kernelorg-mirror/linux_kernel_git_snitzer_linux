# Xsight: what we asked, and whether it was answered

The checklist of the note Mike sent Ian Wigmore and Neil O'Rourke,
"xeu: driver not counting frames dropped at the port's RX FIFO; and
three other driver questions", taken from `xsight-uncounted-loss-email.txt`
as of `ecca41eba8bb` (15 October). If the sent text differs from that
file, the sent text wins and this list follows it.

Each item gives what we asked, what we gave them to go on, and what
would count as an answer. The status starts at **open**; a reply moves
it to **answered** (the question is closed by what they said),
**partial** (some of it, the rest still open) or **not addressed**
(a reply came but did not take it up). Their words go under "Xsight
said", quoted with the date, and our weighing under "Our read".

| # | item | status |
|---|---|---|
| 1 | The RX FIFO drop counters: xeu should read and list them | open |
| 2a | Is our reading of the FIFO drops right; the FIFO-to-ring rate; is 2 MiB the whole buffer | open |
| 2b | The port does not send pause frames | open |
| 3a | Interrupt deferral stops the port | open |
| 3b | Every port's ring interrupts on the same CPUs | open |
| 3c | The Toeplitz hash: seed bytes, fields, order | open |
| 4 | The six patches: will they take them, or their equivalent | open |

## 1. The RX FIFO drop counters

**Asked.** Implicit in section 2 of the note, and its subject: xeu
never calls the SDK's `xsl_rm_eu_rx_port_counters_get()`, so frames
dropped at the port's RX FIFO are counted nowhere a user can see. We
wrote a patch listing them in `ethtool -S`.

**We gave them.** The struct (`rm_rx_port_ctr`: `tc_drop_packets`,
`tc_drop_bytes`, `tc_curr_lines`, `tc_curr_packets`, `fc_curr_lines`);
12,015,394 class-0 drops on the first read after boot; a two-run
account where FIFO drops match the frames that never reached a ring
(13,038 for 11,445, 234,332 for 233,507); the patch.

**An answer would be.** Agreement that these drops belong in the
driver's statistics, and that xeu will report them (our patch or
theirs), or a reason it must not.

**Xsight said.**

Reply 1 (Ian Wigmore, "initial responses from the XSL SW dev team", received by 20 October):

> Adding port Rx FIFO stats. Yes, you are correct. xeu is missing these counters. We will add them.

**Our read.**

## 2a. The FIFO drops: is our reading right

**Asked.** Are frames dropped at the RX FIFO when a burst from the 16
connections outruns what the packet processor moves to the rings in the
time 2 MiB lasts? What rate can it sustain from that FIFO to four
rings? Is 2 MiB the whole of the buffering ahead of the rings for a
port with one traffic class?

**We gave them.** 16384 lines of 128 bytes for class 0, as
`eu_rx_fifos_init()` sizes it, about 268 frames or 46 us at this rate;
fill sampled once a second through 57 s of load: 0 in more than half,
under 1,100 lines in 90%, peaks 10,915 and 14,269; about one frame in
100,000 dropped at today's setting (4,334 and 4,717 in 30 s at 46.2 and
46.5 GB/s); with the client paced (`fq maxrate 23gbit` a flow) nothing
dropped anywhere, at 37 to 38 GB/s.

**An answer would be.** Three things, each separately: (i) yes or no
to the mechanism; (ii) a number for the FIFO-to-ring rate, or how to
measure it; (iii) whether there is more buffering than the 2 MiB (more
lines, other classes' lines, a setting) and how to use it.

**Xsight said.**

Reply 1 (Ian Wigmore, "initial responses from the XSL SW dev team", received by 20 October):

Nothing on the mechanism, the FIFO-to-ring rate or the 2 MiB. The covering note says: "The explanation of the two-port host NFS write performance dropoff to ~60GB/s rather than the expected ~95GB/s due to the extra memcpy pushing the limits of E1's DRAM bandwidth capability was really well explained." (Ours was the server's DRAM, not the E1's.)

**Our read.**

## 2b. No pause frames

**Asked.** What else has to be set for the port to send pause?

**We gave them.** `ethtool -A <port> tx on` reaches
`xsl_rm_eu_fc_set()` with xoff 12288 and xon 10240 lines; the FIFO
passed xoff, kept dropping, and the client's port counted no pause
frame received in four runs. With `rx on tx on` and a down/up, the
rings stopped tail-dropping and the FIFO dropped everything instead
(1.36 million frames in a run), still with no pause on the wire.

**An answer would be.** The missing setting or step (or a fixed
driver), or a statement that the E1 cannot send pause in this
configuration.

**Xsight said.**

Reply 1 (Ian Wigmore, "initial responses from the XSL SW dev team", received by 20 October):

> Pause frames - we will investigate the issue.

**Our read.**

## 3a. Interrupt deferral stops the port

**Asked.** Is xeu expected to work with the kernel's interrupt
deferral, and if not, what does the card offer to hold interrupts
back?

**We gave them.** `napi_defer_hard_irqs=2` and `gro_flush_timeout=50000`
set under load: the rings' frames fell from 3 million a second to a
handful within 2 s, the client lost the port, writing 0 to both did not
revive it, a down/up did; nothing logged; reproduced twice, on 7.1.13
and 7.3-rc7 (and again on build 474, 15 October).

**An answer would be.** Acknowledgement of the hang as a driver bug,
with a fix or an explanation; and the card's own interrupt
moderation, if deferral is not supported.

**Xsight said.**

Reply 1 (Ian Wigmore, "initial responses from the XSL SW dev team", received by 20 October):

> Deferring interrupts stops the port. xeu supports this by the way the NAPI handler is implemented. We will investigate the issue.

**Our read.**

## 3b. Ring interrupts of every port on the same CPUs

**Asked.** Could xeu spread ports over different CPUs (by NUMA node and
port, as `cpumask_local_spread()` does), and set only the hint
(`irq_update_affinity_hint()`) so that a placement survives?

**We gave them.** `xeu_setup_rings_irq()` gives Rx ring i CPU 2i and
Tx ring i CPU 2i+1 whatever the port, and `irq_set_affinity_hint()` sets
the affinity too, so both ports land on CPUs 0-7 and return there on
every open (down/up, `ethtool -G`); both ports on 0-7 carried 65 GB/s of
plain TCP against 83 to 90 apart.

**An answer would be.** A driver change that does both (or either),
or a reason for the current placement.

**Xsight said.**

Reply 1 (Ian Wigmore, "initial responses from the XSL SW dev team", received by 20 October):

> IRQ affinity. This is the way most drivers implement it. Our performance test uses the simple script to set the required affinity through /proc/irq/<IRQn>/smp_affinity_list

**Our read.**

## 3c. The Toeplitz hash

**Asked.** Which bytes of the 68-byte seed are used for IPv4 TCP, over
which fields and in what order?

**We gave them.** We expose the RSS table, key and function through
ethtool (on `xsl_rm_reta_set/get`, `xsl_rm_rss_hash_set/get`,
`xsl_rm_rss_hash_type_set()`) and place each flow by reading its hash
from a received packet; with Toeplitz selected the hashes change, and
a key repeating every 16 bits gives hashes repeating every 16 bits, but
they are not standard Toeplitz over the IPv4 addresses and TCP ports
with the key shown, in any field order we tried.

**An answer would be.** The exact input (bytes of the seed, fields,
their order and byte order), enough to compute a flow's ring before it
connects.

**Xsight said.**

Reply 1 (Ian Wigmore, "initial responses from the XSL SW dev team", received by 20 October):

> The Toeplitz hash - @Yoav Galon , @Guy Tzalik, please help with this

(Passed to two colleagues; no answer yet.)

**Our read.**

## 4. The six patches

**Asked.** Not a question in the note; it attaches them:

- `xeu-add-rx_ip_align-to-4-byte-align-received-IP-head.patch`
- `xeu-retry-an-RX-refill-that-came-up-short.patch`
- `xeu-make-the-RX-refill-threshold-a-module-parameter.patch`
- `xeu-expose-the-RSS-redirection-table-hash-key-and-function-to-ethtool.patch`
- `xeu-cut-RX-buffers-smaller-than-a-page-with-rx_buf_size.patch`
- `xeu-list-the-port-s-RX-FIFO-and-filter-counters-in-ethtool-S.patch`

**An answer would be.** For each: taken into xeu, taken in another
form, or declined with a reason. Until then each is ours to carry on
every xeu release.

**Xsight said.**

Reply 1 (Ian Wigmore, "initial responses from the XSL SW dev team", received by 20 October):

> 1. Adding IP alignment. xeu use the kernel defined NET_IP_ALIGN. For arm64 it is set to 0 https://elixir.bootlin.com/linux/v7.3/source/arch/arm64/include/asm/processor.h#L11. And there is a clear explanation of why. +Rejected.
> 2. Adding retry in case the Rx SB ring is empty and allocation is failed. This is a good catch. We need to handle this situation. But some drivers handle it by returning budget, so NAPI keeps polling. Will fix the +issue.
> 3. Using page frags on Rx path. I understand the motivation to add this when 16KB and 64KB pages are used. We will add this feature.
> 4. Rx refill threshold as module parameter. It will not be easy to justify this parameter while upstreaming the driver. Do you think the threshold value should vary depending on the use case?
> 5. Adding port Rx FIFO stats. Yes, you are correct. xeu is missing these counters. We will add them.
> 6. Expose RSS RETA. We can expose it.

(Their 3 is our rx_buf_size patch; their 6 covers the RSS table, not
said for the key and hash function. Item 4 asks us a question back.)

**Our read.**
