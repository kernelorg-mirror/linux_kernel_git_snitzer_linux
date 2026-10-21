# Handover: a summary artifact for the Xsight meeting (21 October)

For a Claude session preparing what Mike will put in front of Xsight
(Ian Wigmore, Don Wallwork, Neil O'Rourke) **in a two-hour meeting at
their office today, 21 October**. The job: **a new claude.ai artifact,
a reduced summary** of the two artifacts below, with the high-level
overview of the reply drafted to Xsight on top. Mike reviews it before
the meeting; it is shown, not sent.

Written by the tardis1 session that ran XSIGHT-FOLLOWUP.md's X1 to X6
and drafted the reply (X7).

## Inputs, in the order to read them

1. **The reply draft**: `xsight-reply-2.txt` here. Not sent yet; Mike
   sends it. Its first screen is the overview; sections 1 to 6 carry
   the evidence. It is complete: X1 to X6 are all in it.
2. **The two artifacts to reduce** (both Mike's, last updated 14
   October; read each with the Artifact tool, `action: "read"`, not a
   web fetch). Their sources are in this tree under `artifacts/`:
   - **"NFSD Receive Region Budget"**,
     https://claude.ai/artifact/3UQkMVoWW8p9A5ZASPnm3g, source
     `artifacts/region-budget.html` (its `droids.gif` is published
     beside it and is not in the tree). Copy mode: what one port and
     two ports carry (47.4 and 60.4 GB/s inside the drives' fast
     region), what was pinned for them, the receive memory kept in the
     32 MiB cache, what did not help, and that SGL drives take every
     loaned WRITE direct at less than half the copy's CPU.
   - **"NFSD Loan Performance"**,
     https://claude.ai/artifact/1hAkQK1xDiqPr1efdv4ZDb, source
     `artifacts/loan-performance.html`. The receive-page loan: how a
     loaned WRITE moves, that at 60.4 GB/s copy mode is at the limit
     of memory, and what a loan saves in memory traffic, measured on a
     stand-in for SGL storage.
   Mike named these two (21 October).
3. **The kernel's inventory: "v7.3-7 Not Upstream"**,
   https://claude.ai/artifact/Q146xk6UJS7sJSEsfyPDDi (updated 20
   October): which commits the v7.3-7 release's topic branches carry
   that no NFS, NFSD or SUNRPC maintainer has staged, with a summary of
   each topic. It is what to point at when Xsight asks what of the
   receive path is ours and not yet upstream (the page loan, the
   receive region). **It may need refreshing first**: the v7.3-7
   rebase (20 October) and what has landed since may have changed what
   it lists, so check the report's scope and commit list against the
   release's branches before quoting its counts (and whether it covers
   `kernel-7.3/main.NFSD_TCP_WRITE_ZEROCOPY-v7.3-7`, which gained
   `b783bf8ec202` on 21 October). How to regenerate and republish to
   the same URL is
   `hs/NOT_UPSTREAM_REPORTS.md` in kernel-rpmtree ("generate a
   NOT_UPSTREAM report for v7.3-7"); the older inventories (v6.12.*,
   v7.1.13-*) are other releases and not for this meeting.
4. **Background, if a point needs it**: `xsight-email-checklist.md`
   (what we asked, what they answered, quoted, and our reading),
   `xsight-uncounted-loss-email.txt` (the first note as sent), and
   `xeu-rig/XSIGHT-FOLLOWUP.md` (each measurement's write-up, X1 to X6).

## The overview, as the draft has it

- **The card is not the limit.** One port carries line rate (49.8
  GB/s plain TCP), two ports 98 to 99.6 GB/s; NFS on two ports stops
  near 60 GB/s because the server copies each write and the copy's
  memory traffic is the ceiling. Ian's note called it "E1's DRAM"; it
  is the server's. (Correct this gently, early.)
- **rx_ip_align was rejected on a misreading.** It is about where the
  payload lands for the NVMe DMA (2 mod 4 without it, so no loaned
  page can go direct), not the CPU's header loads. Ask: can the E1
  shift only a frame's first buffer (a receive pad, header split)?
- **FIFO drops are the XDMA engines stalling on memory** (X2): with
  receivers discarding, the FIFO never drops at line rate; with the
  host's memory loaded hard (8 copy threads, ~65 GB/s) it drops half a
  million frames in 20 s with no copy at all. Ask: what latency the
  engines tolerate; any DMA priority or more FIFO.
- **Interrupt placement** (X4): a placement is lost on every port open
  because close frees the IRQs and disables MSI-X, not only because of
  the hint. Our patch records and restores it; before and after
  measured. Pushed in kernel-rpmtree as xeu Release 8.
- **rx_refill_thresh** (X5): it should follow the ring size, not the
  use case. On rings of 128, 32 drops thousands of frames a port in
  10 s and 8 drops 1 to 15; on rings of 256 they are level. Suggest
  something like `min(32, size / 8)` and we drop our patch.
- **The hash** (X1): Don's 13-byte input does not reproduce the card's
  hashes at any key offset or byte order; we need the exact
  computation. Table entry = hash bits 13 to 19, confirmed.
- **Pause and deferral** (X6): both reproduce with two scripts, plain
  TCP, no NFS. Deferral: Rx interrupts stop at once and never return;
  only a down and up recovers. Pause: none reaches the wire; with rx
  and tx on, the FIFO is held at xoff and drops there.

## What changed since the artifacts (14 October)

Reduce them with these in mind, or the summary will contradict the
reply:

- **15 October**: on Mike's word six things were dropped, among them
  the linger, the keep, the SO_RCVLOWAT mark, several thread pools per
  NUMA node and veth's `rx_ip_align`. Anything in the artifacts that
  credits those is history, not the current setting. The current
  setting is `xeu-rig/settled.sh`.
- **Builds 479 and 491** (7.3 and v7.3-7) measure in the same range:
  two ports 56.9 to 58.6 GB/s clean (479), 56.8 to 58.1 (491).
- **tardis2** has SGL drives (two DapuStor DP800s): there every loaned
  WRITE is written direct (`xeu-rig/README.md`, "tardis2", 17
  October), which the artifacts could only predict with a stand-in.

## For the new artifact

- **One page Mike can talk through in two hours with Xsight**: what we
  measured, what we need from them, what we give them (patches,
  scripts). Lead with the card carrying line rate, then the asks. Keep
  numbers to the few that carry each point; link nothing internal.
- **It will be shown to people outside Hammerspace.** No host names
  beyond "server" and "client", no internal paths, URLs, people or
  build numbers that mean nothing to them; no Hammerspace or Xsight
  branding or logos. Their replies may be quoted only as the
  checklist quotes them.
- **Mike's, private by default**: publish it as a new artifact (do not
  change the two above), give Mike the link, and let him decide whom
  to share it with.
- Follow the Artifact tool's quickstart for a new page (`intent:
  "slides"` if a deck suits a two-hour meeting better than a page; Mike
  can say).

## X3, the strongest single result for the meeting

rx_ip_align on drives that take SGLs, the only thing deciding direct
or not (`xeu-rig/XSIGHT-FOLLOWUP.md`, X3; the draft's section 1): at 0
every loaned WRITE is refused for alignment and copied (0 direct,
281,600 and 281,920 refused), at 2 every one goes to the drive
direct (280,416 and 280,000, none refused); nfsd's CPUs 12% against 1
to 2%. The rate is the drives' either way. This is the evidence that
the patch is about the storage DMA, which Xsight rejected as a CPU
matter.
