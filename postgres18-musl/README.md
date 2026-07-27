# postgres18-musl

Stock **PostgreSQL 18** (branch `REL_18_STABLE`) built from source with the
**musl** C toolchain as an OSv position-independent executable, booting on OSv
and serving SQL over TCP.

This is a demo: it boots a small baked cluster and answers `psql` queries over
the guest's forwarded port. It is deliberately single-backend-safe (see
[Status](#status)).

## What it does

1. `build.sh` fetches the PostgreSQL 18 source, builds it with `musl-gcc`,
   `initdb`s a small demo cluster containing a `demo` database with a
   `greetings` table, and lays the install tree + baked cluster out for the
   image manifest.
2. At boot, `module.py` mounts a writable ramfs over `/data`, copies the
   read-only baked cluster from `/data-seed` (on rofs) into it with the tiny
   `seed_copy` helper, then starts `postgres -D /data`.
3. PostgreSQL listens on TCP `0.0.0.0:5432` and serves queries.

## Build & run

From the top-level OSv directory (this app lives under `apps/`):

```bash
# Build the OSv image with this app.  conf_fork=1 pulls in OSv's fork/COW
# cross-address-space coherence (PostgreSQL's postmaster forks a backend per
# connection).  fs=ramfs makes the root (and thus the writable /data) a ramfs.
./scripts/build -j$(nproc) conf_fork=1 fs=ramfs image=postgres18-musl

# Boot under KVM, forwarding the guest's 5432 to the host (fork needs >=2 vCPUs):
./scripts/run.py -c2 -nvV --forward tcp::5432-:5432

# From the host, query the demo over the forwarded port:
psql -h 127.0.0.1 -p 5432 -U postgres -d demo -c 'select * from greetings;'
```

> **OSv kernel requirement.** PostgreSQL forks a backend per connection, so this
> demo needs the OSv fork/COW cross-address-space coherence work (`conf_fork=1`,
> plus the DSM/POSIX-shm-registry, ramfs, net-stack, mbuf and thread-stack
> identity-heap fixes). On an OSv tree without those, the postmaster boots but a
> forked backend hits `could not open shared memory segment` (DSM ENOENT) and no
> query is served. Those fixes are a separate OSv kernel effort (fork-arena /
> PR #1455 line of work); this app is the userspace half of the demo.

Expected output:

```
 id |    who     |          msg
----+------------+------------------------
  1 | osv        | hello from a unikernel
  2 | postgres18 | stock musl build
  3 | demo       | this row came over TCP
(3 rows)
```

## OSv deviations from stock PostgreSQL

Every deviation below is an **OSv-workaround**, not an improvement to
PostgreSQL. Each exists only because a current OSv capability gap forces it,
and each is tracked to be **erased** as those gaps close. Nothing here changes
PostgreSQL's on-disk format or SQL behavior.

**Build flags** (`build.sh`, step 3):

| Deviation | Why | Erase when |
|---|---|---|
| `CC=musl-gcc` | OSv's libc is musl | — (intrinsic to the musl target) |
| `--without-icu/zlib/readline/lz4/zstd/libxml` | those libs are not baked into the demo image | those libs are packaged for OSv |
| `-DWAIT_USE_SELF_PIPE` | OSv's epoll/signalfd latch path is incomplete; use PostgreSQL's portable self-pipe latch | OSv latch path is complete |
| `LDFLAGS_EX=-pie` | OSv runs applications as PIEs | — (intrinsic to OSv) |

**Source neuters** (`build.sh`, step 2 -- two one-line `if (0 && ...)` guards):

| Deviation | Why | Erase when |
|---|---|---|
| `check_root()` in `main.c` | OSv runs the app as uid 0; there is no unprivileged user to drop to | OSv models an unprivileged user |
| `checkDataDir()` perms in `miscinit.c` | OSv filesystems do not report Unix-style directory modes | OSv fs reports Unix modes |

**Cluster config** (`build.sh`, step 5, baked into `postgresql.conf`/`pg_hba.conf`):

| Deviation | Why | Erase when |
|---|---|---|
| `initdb --locale=C` | musl provides only the C locale | musl locale support lands |
| `io_method = sync` | OSv's async-I/O (io_uring/worker) path is incomplete | OSv AIO path is complete |
| `unix_socket_directories = ''` | OSv has no AF_UNIX | OSv AF_UNIX support lands |
| `listen_addresses = '*'` + `pg_hba trust` | demo reachability over the forwarded TCP port | it's a demo (ephemeral ramfs cluster) |

## Status

- **Works: single-backend query serving.** The demo boots PostgreSQL to
  "database system is ready to accept connections" and serves real SQL
  (`SELECT`, DDL, DML, aggregates) over TCP. Sequential `psql` connections are
  reliable.
- **WIP: sustained multi-backend concurrency.** PostgreSQL's postmaster forks a
  backend per connection, and OSv's `fork()`/COW cross-address-space coherence
  is still being hardened (a separate OSv kernel effort). A demo doing
  sequential queries is fine; hammering it with many concurrent connections can
  trip the fork/reap lifecycle wall. This app is intentionally scoped to the
  single-backend demo.

## Files

- `Makefile` -- osv-apps entry point; runs `build.sh`.
- `build.sh` -- fetch + build PostgreSQL, build `seed_copy`, `initdb` the demo
  cluster, emit `usr.manifest`.
- `seed_copy.c` -- boot-time recursive rofs→ramfs copy helper (libc only).
- `module.py` -- boot command (mount ramfs, seed_copy, run postgres).
- `usr.manifest` -- generated by `build.sh`; maps the build output into the image.
