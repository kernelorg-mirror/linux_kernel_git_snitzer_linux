#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0
"""results-table.py - one host's test-plan results in a form that can be
set beside another page size's.

  results-table.py extract RUNDIR > results/<page>.tsv
      Reduce a run-testplan.sh results directory to "key<TAB>value" lines:
      what was run, every stage's result, and every cell of every budget
      phase on both kinds of storage.  Region use is also given in MiB,
      since a page count means nothing across page sizes.

  results-table.py compare results/*.tsv > comparison.md
      Markdown tables, one column group per page size.  A cell or stage a
      host has not run shows as "-".

Run "extract" on each host when its plan is complete, commit the .tsv, and
regenerate RESULTS.md's tables with "compare".
"""
import glob
import os
import re
import sys

STORAGES = (("prp", "budget", "needs whole pages (nvme-loop)"),
            ("sgl", "budget-sgl", "takes arbitrary pieces (NVMe/TCP)"))
CELL_METRICS = ("nconn", "budget", "rcvbuf", "MBps", "fio_rc", "drops", "alloc_fail",
                "used_pages", "rto", "retrans", "admitted", "denied", "paused",
                "overrun", "wr_direct", "wr_dontcache", "wr_buffered")
X_METRICS = ("budget", "clients", "R_MBps", "N_MBps", "R_rc", "N_rc", "drops",
             "alloc_fail", "used_pages", "admitted", "denied")
N_METRICS = ("init", "default", "B", "clients", "R_MBps", "B_MBps", "R_rc", "B_rc",
             "init_admitted", "default_admitted", "B_admitted", "no_budget", "used_pages")


def table(path):
    if not os.path.exists(path):
        return []
    rows = [l.split() for l in open(path) if l.strip()]
    return [dict(zip(rows[0], r)) for r in rows[1:]]


def extract(run):
    out = []

    def put(key, val):
        out.append("%s\t%s" % (key, val))

    status = {}
    boots = []
    for l in open(os.path.join(run, "status.tsv")).read().splitlines()[1:]:
        f = l.split("\t")
        status[f[0]] = (f[1], f[4])
        if f[3] not in boots:
            boots.append(f[3])
    rig = {}
    for st, d, _ in STORAGES:
        for p in sorted(glob.glob(os.path.join(run, d, "stress-*", "rig.txt"))):
            rig[st] = dict(kv.split("=", 1) for kv in open(p).read().split())
    any_rig = rig.get("prp") or rig.get("sgl") or {}
    ps = int(any_rig.get("page_size", os.sysconf("SC_PAGESIZE")))
    put("meta/page_size", ps)
    put("meta/kernel", any_rig.get("kernel", os.uname().release))
    put("meta/boots", len(boots))
    put("meta/last_stage_at", max(v[1] for v in status.values()))
    man = "/var/lib/zc-transplant/manifest"
    if os.path.exists(man):
        for l in open(man):
            if l.startswith(("describe=", "version=")):
                put("meta/" + l.split("=", 1)[0], l.split("=", 1)[1].strip())
    for st in rig:
        put("meta/%s/virt_boundary_mask" % st, rig[st].get("virt_boundary_mask", "?"))
        put("meta/%s/rx_ip_align" % st, rig[st].get("rx_ip_align", "?"))
    geo = os.path.join(run, "budget", "geometry.txt")
    if os.path.exists(geo):
        put("meta/geometry", open(geo).read().strip())
    for s, (res, _) in status.items():
        put("stage/" + s, res)

    klog = os.path.join(run, "kunit", "stage.log")
    if os.path.exists(klog):
        for m in re.finditer(r"# ([a-z0-9-]+): pass:(\d+) fail:(\d+) skip:(\d+) total:(\d+)", open(klog).read()):
            put("kunit/" + m.group(1), "%s/%s pass, %s fail, %s skip" % (m.group(2), m.group(5), m.group(3), m.group(4)))
    for rg in ("tcp", "loop"):
        log = os.path.join(run, "localio-" + rg, "stage.log")
        if os.path.exists(log):
            t = open(log).read()
            for m in re.finditer(r"^rs=(\d+)\s+WRITEs=(\d+)\s+reads/WRITE=([0-9.]+)", t, re.M):
                put("localio/%s/rs%s/reads_per_write" % (rg, m.group(1)), m.group(3))
            for m in re.finditer(r"(boundary|short-read|stable) exit=(\d+)", t):
                put("localio/%s/%s_exit" % (rg, m.group(1)), m.group(2))

    def mib(pages):
        return "%.2f" % (int(pages) * ps / 1048576.0)

    for st, d, _ in STORAGES:
        for pdir in sorted(glob.glob(os.path.join(run, d, "stress-*"))):
            ph = os.path.basename(pdir)[len("stress-"):]
            fin = os.path.join(pdir, "region-final.txt")
            if os.path.exists(fin):
                kv = open(fin).read().split()
                if "pages" in kv:
                    put("region/%s/%s/pages" % (st, ph), kv[kv.index("pages") + 1])
                    put("region/%s/%s/MiB" % (st, ph), mib(kv[kv.index("pages") + 1]))
            fails = 0
            for f in glob.glob(os.path.join(pdir, "*", "fio.out")) + glob.glob(os.path.join(pdir, "*", "*", "fio.out")):
                fails += open(f, errors="replace").read().count("verify:")
            put("verify_failures/%s/%s" % (st, ph), fails)
            for kind, fname, metrics in (("cell", "cells.tsv", CELL_METRICS),
                                         ("x", "xcells.tsv", X_METRICS),
                                         ("n", "ncells.tsv", N_METRICS)):
                for r in table(os.path.join(pdir, fname)):
                    base = "%s/%s/%s/%s" % (kind, st, ph, r["cell"])
                    for m in metrics:
                        if m in r:
                            put(base + "/" + m, r[m])
                    if "used_pages" in r:
                        put(base + "/used_MiB", mib(r["used_pages"]))
    print("\n".join(out))


def load(path):
    d = {}
    order = []
    for l in open(path):
        if "\t" in l:
            k, v = l.rstrip("\n").split("\t", 1)
            d[k] = v
            order.append(k)
    return d, order


def written(d, base):
    wd = int(d.get(base + "/wr_direct", 0) or 0)
    wb = int(d.get(base + "/wr_dontcache", 0) or 0) + int(d.get(base + "/wr_buffered", 0) or 0)
    if wd and not wb:
        return "direct"
    if wb and not wd:
        return "buffered"
    if wd and wb:
        return "both"
    return "-"


def compare(paths):
    runs = sorted((load(p) for p in paths), key=lambda r: int(r[0]["meta/page_size"]))
    names = ["%d KiB" % (int(d["meta/page_size"]) // 1024) for d, _ in runs]

    def union(prefix, depth):
        seen = []
        for d, order in runs:
            for k in order:
                if k.startswith(prefix):
                    part = k[len(prefix):].split("/")[depth]
                    if part not in seen:
                        seen.append(part)
        return seen

    def row(cols):
        print("| " + " | ".join(str(c) for c in cols) + " |")

    def head(cols):
        row(cols)
        row(["---"] * len(cols))

    print("### What each column is\n")
    head([""] + names)
    for label, key in (("kernel", "meta/kernel"), ("built from", "meta/describe"), ("boots used", "meta/boots"),
                       ("last stage finished", "meta/last_stage_at"), ("page budgets", "meta/geometry"),
                       ("nvme-loop: virt_boundary_mask", "meta/prp/virt_boundary_mask"),
                       ("NVMe/TCP: virt_boundary_mask", "meta/sgl/virt_boundary_mask"),
                       ("NVMe/TCP: veth rx_ip_align", "meta/sgl/rx_ip_align")):
        row([label] + [d.get(key, "-") for d, _ in runs])

    print("\n### The plan, stage by stage\n")
    head(["stage"] + names)
    for s in union("stage/", 0):
        row(["`%s`" % s] + [d.get("stage/" + s, "-") for d, _ in runs])
    for s in union("kunit/", 0):
        row(["KUnit `%s`" % s] + [d.get("kunit/" + s, "-") for d, _ in runs])
    for s in union("localio/", 0):
        for k in union("localio/%s/" % s, 0):
            leaf = [x for x in runs[0][1] + sum((o for _, o in runs[1:]), []) if x.startswith("localio/%s/%s" % (s, k))]
            for full in dict.fromkeys(leaf):
                row(["LOCALIO %s" % full[len("localio/"):]] + [d.get(full, "-") for d, _ in runs])
    for st, _, _ in STORAGES:
        for ph in union("verify_failures/%s/" % st, 0):
            row(["fio verify failures, %s phase %s" % (st, ph)] + [d.get("verify_failures/%s/%s" % (st, ph), "-") for d, _ in runs])

    print("\n### Copy against loan\n")
    print("Each loan cell (`-on`, `edge-at-`) beside its twin that copies (`-copy`): MB/s, and the")
    print("most of the region either held, in MiB. \"sized\" pairs write files that already have their size.\n")
    pairs = (("A", "seq-sized-copy", "seq-sized-on"), ("A", "mixed-sized-copy", "mixed-sized-on"),
             ("A", "step-copy", "step-on"), ("A", "step-copy-n16", "step-on-n16"),
             ("A", "seq-copy", "seq-on"), ("A", "mixed-copy", "mixed-on"), ("B", "edge-copy-n1", "edge-at-n1"))
    for st, _, what in STORAGES:
        print("Storage that %s:\n" % what)
        head(["pair (phase)"] + ["%s: copy / loan MB/s" % n for n in names] + ["%s: region MiB copy / loan" % n for n in names] + ["loan written"])
        for ph, c, l in pairs:
            a, b, w = [], [], "-"
            for d, _ in runs:
                kc, kl = "cell/%s/%s/%s" % (st, ph, c), "cell/%s/%s/%s" % (st, ph, l)
                a.append("%s / %s" % (d.get(kc + "/MBps", "-"), d.get(kl + "/MBps", "-")))
                b.append("%s / %s" % (d.get(kc + "/used_MiB", "-"), d.get(kl + "/used_MiB", "-")))
                if kl + "/MBps" in d:
                    w = written(d, kl)
            row(["`%s` / `%s` (%s)" % (c, l, ph)] + a + b + [w])
        print()

    print("### Every cell\n")
    print("MB/s · packets dropped · most of the region held (MiB) · how nfsd issued the WRITEs.\n")
    for st, _, what in STORAGES:
        for ph in union("cell/%s/" % st, 0):
            cells = union("cell/%s/%s/" % (st, ph), 0)
            if not cells:
                continue
            print("Phase %s, storage that %s:\n" % (ph, what))
            head(["cell", "conn", "budget set"] + names)
            for c in cells:
                base = "cell/%s/%s/%s" % (st, ph, c)
                conn = bud = "-"
                vals = []
                for d, _ in runs:
                    if base + "/MBps" not in d:
                        vals.append("-")
                        continue
                    conn = d.get(base + "/nconn", conn)
                    bud = "yes" if d.get(base + "/budget", "0") != "0" else "no"
                    rc = d.get(base + "/fio_rc", "0")
                    vals.append("%s · %s · %s · %s%s" % (d[base + "/MBps"], d.get(base + "/drops", "-"),
                                                        d.get(base + "/used_MiB", "-"), written(d, base),
                                                        "" if rc == "0" else " · **fio rc %s**" % rc))
                row(["`%s`" % c, conn, bud] + vals)
            print()
    for kind, title, cols in (("x", "Phases X and D: a region client (R) beside a client that is not (N)",
                               (("R_MBps", "R MB/s"), ("N_MBps", "N MB/s"), ("drops", "drops"), ("alloc_fail", "alloc fail"), ("used_MiB", "region MiB"))),
                              ("n", "Phase N: a budget per network namespace",
                               (("R_MBps", "R MB/s"), ("B_MBps", "B MB/s"), ("no_budget", "copied (no budget)"), ("used_MiB", "region MiB")))):
        printed = False
        for st, _, what in STORAGES:
            for ph in union("%s/%s/" % (kind, st), 0):
                cells = union("%s/%s/%s/" % (kind, st, ph), 0)
                if not cells:
                    continue
                if not printed:
                    print("### %s\n" % title)
                    print(" · ".join(c[1] for c in cols) + ".\n")
                    printed = True
                print("Phase %s, storage that %s:\n" % (ph, what))
                head(["cell"] + names)
                for c in cells:
                    base = "%s/%s/%s/%s" % (kind, st, ph, c)
                    row(["`%s`" % c] + [" · ".join(d.get(base + "/" + k, "-") for k, _ in cols) if any(
                        (base + "/" + k) in d for k, _ in cols) else "-" for d, _ in runs])
                print()


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "extract":
        extract(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "compare":
        compare(sys.argv[2:])
    else:
        sys.stderr.write(__doc__)
        sys.exit(2)
