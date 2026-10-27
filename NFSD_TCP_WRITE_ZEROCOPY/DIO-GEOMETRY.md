# Direct-I/O geometry and NFSD retry

This correction distinguishes an unsupported buffer layout from an ordinary
filesystem or device error without adding a more restrictive NFSD alignment
gate. The actual filesystem and block owners still decide which layouts can
be submitted. Every successful splitter predicate and positive split offset
is preserved. An evidenced supported-layout defect must be repaired, not
classified as unsupported.

## Internal result, ordinary errors

`EDIOALIGN` (532) and `BLK_STS_DIOALIGN` (20) are local internal choices, not
assigned upstream values, UAPI additions or new NFS wire statuses. Generic
`BLK_STS_INVAL` and `EINVAL` keep their meaning. Only the actual DMA/length-mask
and zero-legal-prefix refusals in ordinary unencrypted READ/WRITE can emit the
new category. Atomic, NOWAIT, zone-append, crypto and invariant failures keep
their existing classifications.

Ordinary block splitting retains the raw category. Scalar splitter and
passthrough callers normalize it to their legacy `EINVAL`. Generic block
status decoding also normalizes it; cooperating direct completions use the
raw decoder. NFSD alone selects the private kiocb participation bit for its
eligible synchronous fixed-offset direct segment. The bit is not derived
from user RWF flags and is cleared before buffered reissue. Ordinary sync,
AIO and io_uring completion and filesystem notifications expose no private
errno. Geometry is not a path/failover error.

## Completion and retry

An error aggregate cannot lose an error to success. Geometry loses to every
other nonzero error in either arrival order; multiple real errors retain the
existing owner's selection. BIO/MQ, DM, Btrfs, ioend/iomap and XFS completion
honor this priority before terminal completion. Concurrent error writers use
their existing lock or an error-path compare/exchange, not a success-path RMW.
Btrfs terminal publication also respects an original BIO's real error.

NFSD removes its generic-EINVAL retry. At most one normal buffered overwrite
is allowed after a pure geometry negative result, only on the synchronous
nonappend/nonatomic/non-NOWAIT path, after filesystem/BIO drain, unchanged
reported position, and the existing RPC writeback-error cursor check. Restore
the saved iterator and start offset exactly. A positive short direct result
is never replayed. A known real failure prohibits reissue; a later error
still fails the reply and cannot trigger a third attempt.

The first attempt may already have changed storage. Reissue is a normal
logical overwrite using the immutable request-owned payload, not rollback,
physical idempotence, zero-progress proof or a new atomicity guarantee.
Other writes and truncate can interleave under the existing rules. Account
only final returned bytes once. Preserve the existing final NFSD stable-write
path; do not infer one physical flush from one NFSD final fsync.

## Cost and verification

Successful I/O adds no independent descriptor walk, payload copy, allocation,
global lock, unconditional diagnostic or successful-I/O RMW. Native instruction,
throughput, CPU/byte and latency comparisons are still required; source review
alone establishes no measured gain or regression bound.

Production changes, KUnit access/tests and this documentation are separate
delivery groups. Tests must identify the production change they verify and
exercise actual owners rather than a duplicated model. Native qualification
must retain mixed-error orders/races, partial storage progress and drain,
iterator/position/count/page lifetime, ordinary outward errors, physical
positive legacy/loaned geometries, TCP byte/copy evidence and paired timing.
A software fixture does not qualify physical zero-copy. The ordinary release
selection is unchanged until the separate kernel integration owner completes
its qualification and selection process.
