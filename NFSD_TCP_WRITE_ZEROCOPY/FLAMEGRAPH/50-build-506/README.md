# Round 50: the 1 MiB load on build 506

**The ask (Mike, 2 November):** run `settled.sh`, do a proper 1 MiB performance test, and commit what the best issue to fix next is.

tardis2, build 506, sunrpc `5CFC092E63911EC723D2A63`, nfsd `3F0C341E86E266D5FDF2ADF`, xeu `CA0931345E46C929B11D01F`, no pseudo-NMI, no KUnit suite loaded. `run128.sh` runs four untraced loan runs, two of `lat.sh`, one profile; logs in `$L/t2-r128/`. Copy is not run.

**This boot's drives:** only seven NVMe devices are on PCI, so two SN8100s are gone (they went before the boot of the crash in `TESTING.md`, and no PCI rescan was tried). The four DPRP5s are `nvme2` to `nvme5`, mounted at `/hs/nvme4` to `7` as every script expects. `data_drives` was fixed for it.
