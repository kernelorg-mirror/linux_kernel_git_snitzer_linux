# LOCALIO tests (tracked copies)

Copies of the files `run-sgl-localio-ab.sh` needs, which otherwise live in
other projects' untracked directories of the original working tree, so a
fresh clone of this branch can run the LOCALIO A/B stage:

| file | from | change |
|---|---|---|
| `start-nfsd.sh` | tree root | none |
| `localio-boundary-test.sh`, `localio-short-read-test.sh`, `localio-stable-test.sh`, `odirect_pread.c` | `NFS_LOCALIO_DONTCACHE/` | `iorhard` and `resident` are built here from source when missing |
| `iorhard.c`, `resident.c` | `NFSD_DONTCACHE_FALLBACK/` | none |

Binaries (`iorhard`, `resident`, `odirect_pread`) are built beside the
scripts on first use (`gcc -O2 [-pthread]`) and are not tracked.  The
originals remain canonical for their own projects; refresh these copies if
those change.
