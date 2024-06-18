===========
NFS localio
===========

This document gives an overview of the LOCALIO auxiliary RPC protocol
added to the Linux NFS client and server (both v3 and v4) to allow a
client and server to reliably handshake to determine if they are on the
same host.  The LOCALIO auxiliary protocol's implementation, which uses
the same connection as NFS traffic, follows the pattern established by
the NFS ACL protocol extension.

The LOCALIO auxiliary protocol is needed to allow robust discovery of
clients local to their servers.  In a private implementation that
preceded use of this LOCALIO protocol, a fragile sockaddr network
address based match against all local network interfaces was attempted.
But unlike the LOCALIO protocol, the sockaddr-based matching didn't
handle use of iptables or containers.

The robust handshake between local client and server is just the
beginning, the ultimate usecase this locality makes possible is the
client is able to issue reads, writes and commits directly to the server
without having to go over the network.  This is particularly useful for
container usecases (e.g. kubernetes) where it is possible to run an IO
job local to the server.

The performance advantage realized from localio's ability to bypass
using XDR and RPC for reads, writes and commits can be extreme, e.g.:
fio for 20 secs with 24 libaio threads, 64k directio reads, qd of 8,
-  With localio:
  read: IOPS=691k, BW=42.2GiB/s (45.3GB/s)(843GiB/20002msec)
-  Without localio:
  read: IOPS=15.7k, BW=984MiB/s (1032MB/s)(19.2GiB/20013msec)

RPC
---

The LOCALIO auxiliary RPC protocol consists of a single "GETUUID" RPC
method that allows the Linux NFS client to retrieve a Linux NFS server's
uuid.  This protocol isn't part of an IETF standard, nor does it need to
be considering it is Linux-to-Linux auxiliary RPC protocol that amounts
to an implementation detail.

The GETUUID method encodes the server's uuid_t in terms of the fixed
UUID_SIZE (16 bytes).  The fixed size opaque encode and decode XDR
methods are used instead of the less efficient variable sized methods.

The RPC program number for the NFS_LOCALIO_PROGRAM is 400122 (as assigned
by IANA, see https://www.iana.org/assignments/rpc-program-numbers/ ):
Linux Kernel Organization       400122  nfslocalio

The LOCALIO protocol spec in rpcgen syntax is:

/* raw RFC 9562 UUID */
#define UUID_SIZE 16
typedef u8 uuid_t<UUID_SIZE>;

program NFS_LOCALIO_PROGRAM {
    version LOCALIO_V1 {
        void
            NULL(void) = 0;

        uuid_t
            GETUUID(void) = 1;
    } = 1;
} = 400122;

LOCALIO uses the same transport connection as NFS traffic.  As such,
LOCALIO is not registered with rpcbind.

Once an NFS client and server handshake as "local", the client will
bypass the network RPC protocol for read, write and commit operations.
Due to this XDR and RPC bypass, these operations will operate faster.

NFS Common and Server
---------------------

Localio is used by nfsd to add access to a global nfsd_uuids list in
nfs_common that is used to register and then identify local nfsd
instances.

nfsd_uuids is protected by the nfsd_mutex or RCU read lock and is
composed of nfsd_uuid_t instances that are managed as nfsd creates them
(per network namespace).

nfsd_uuid_is_local() and nfsd_uuid_lookup() are used to search all local
nfsd for the client specified nfsd uuid.

The nfsd_uuids list is the basis for localio enablement, as such it has
members that point to nfsd memory for direct use by the client
(e.g. 'net' is the server's network namespace, through it the client can
access nn->nfsd_serv with proper rcu read access).  It is this client
and server synchronization that enables advanced usage and lifetime of
objects to span from the host kernel's nfsd to per-container knfsd
instances that are connected to nfs client's running on the same local
host.

NFS Client
----------

fs/nfs/localio.c:nfs_local_probe() will retrieve a server's uuid via
LOCALIO protocol and check if the server with that uuid is known to be
local.  This ensures client and server 1: support localio 2: are local
to each other.

See fs/nfs/localio.c:nfs_local_open_fh() and
fs/nfsd/localio.c:nfsd_open_local_fh() for the interface that makes
focused use of nfsd_uuid_t struct to allow a client local to a server to
open a file pointer without needing to go over the network.

The client's fs/nfs/localio.c:nfs_local_open_fh() will call into the
server's fs/nfsd/localio.c:nfsd_open_local_fh() and carefully access
both the nfsd network namespace and the associated nn->nfsd_serv in
terms of RCU.  If nfsd_open_local_fh() finds that client no longer sees
valid nfsd objects (be it struct net or nn->nfsd_serv) it returns ENXIO
to nfs_local_open_fh() and the client will try to reestablish the
LOCALIO resources needed by calling nfs_local_probe() again.  This
recovery is needed if/when an nfsd instance running in a container were
to reboot while a localio client is connected to it.

Testing
-------

The LOCALIO auxiliary protocol and associated NFS localio read, write
and commit access have proven stable against various test scenarios but
these have not yet been formalized in any testsuite:

-  Client and server both on localhost (for both v3 and v4.2).

-  Various permutations of client and server support enablement for
   both local and remote client and server.  Testing against NFS storage
   products that don't support the LOCALIO protocol was also performed.

-  Client on host, server within a container (for both v3 and v4.2)
   The container testing was in terms of podman managed containers and
   includes container stop/restart scenario.
