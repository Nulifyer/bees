# bees Command Line Options

## Load management options

* `--thread-count COUNT` or `-c`

 Specify maximum number of worker threads.  Overrides `--thread-factor`
 (`-C`), default/autodetected values, and the hardcoded thread limit.

* `--thread-factor FACTOR` or `-C`

 Specify ratio of worker threads to detected CPU cores.  Overridden by
 `--thread-count` (`-c`).

 Default is 1.0, i.e. 1 worker thread per detected CPU.  Use values
 below 1.0 to leave some cores idle, or above 1.0 if there are more
 disks than CPUs in the filesystem.

* `--loadavg-target LOADAVG` or `-g`

 Specify load average target for dynamic worker threads.  Default is
 to run the maximum number of worker threads all the time.

 Worker threads will be started or stopped subject to the upper limit
 imposed by `--thread-factor`, `--thread-min` and `--thread-count`
 until the load average is within +/- 0.5 of `LOADAVG`.

* `--thread-min COUNT` or `-G`

 Specify minimum number of dynamic worker threads.  This can be used
 to force a minimum number of threads to continue running while using
 `--loadavg-target` to manage load.

 Default is 0, i.e. all bees worker threads will stop when the system
 load exceeds the target.

 Has no effect unless `--loadavg-target` is used to specify a target load.

* `--throttle-factor FACTOR`

 In order to avoid saturating btrfs deferred work queues, bees tracks
 the time that operations with delayed effect (dedupe and tmpfile copy)
 and operations with long run times (`LOGICAL_INO`) run.  If an operation
 finishes before the average run time for that operation, bees will
 sleep for the remainder of the average run time, so that operations
 are submitted to btrfs at a rate similar to the rate that btrfs can
 complete them.

 The `FACTOR` is multiplied by the average run time for each operation
 to calculate the target delay time.

 `FACTOR` 0 is the default, which adds no delays.  bees will attempt
 to saturate btrfs delayed work queues as quickly as possible, which
 may impact other processes on the same filesystem, or even slow down
 bees itself.

 `FACTOR` 1.0 will attempt to keep btrfs delayed work queues filled at
 a steady average rate.

 `FACTOR` more than 1.0 will add delays longer than the average
 run time (e.g. 10.0 will delay all operations that take less than 10x
 the average run time).  High values of `FACTOR` may be desirable when
 using bees with other applications on the same filesystem.

 The maximum delay per operation is 60 seconds.

* `--no-mlock`

 Do not lock the hash table in memory.

 By default the whole hash table is `mlock`ed once it has been read, so it
is never swapped out or reclaimed, also while bees has nothing to do.
With this option the hash table is ordinary anonymous memory: under
memory pressure (or a cgroup limit such as systemd's `MemoryHigh`) the
kernel may move parts of it to swap, and bees slows down while it reads
them back.  Use it where giving memory back to other programs matters
more than bees' speed, e.g. on a desktop or laptop with compressed swap.

## Filesystem tree traversal options

* `--scan-mode MODE` or `-m`

 Specify extent scanning algorithm.
 **EXPERIMENTAL** feature that may go away.

  * Mode 0: lockstep
  * Mode 1: independent
  * Mode 2: sequential
  * Mode 3: recent
  * Mode 4: extent

 For details of the different scanning modes and the default value of
 this option, see [bees configuration](config.md).

* `--settle-delay SECONDS`

 Do not scan new data until it has been left alone for `SECONDS`.
 Default is 0: scan data as soon as its transaction is committed.

 Without this option bees reads, hashes and dedupes build output, package
extraction and caches that are deleted or rewritten minutes later, and
it dedupes files that are still being written, which makes the kernel
flush and lock them.  With a settle delay:

  * bees scans only up to the newest transid that it saw committed at
    least `SECONDS` ago.  Data that is deleted or overwritten before it
    is that old is never looked up, opened or read.
  * In scan mode 4 (extent), an extent that is old enough is still put
    aside when one of its files was changed (ctime or mtime) less than
    `SECONDS` ago, e.g. a log that is being appended to, a download or
    a VM image.  bees looks at it again when the file has been left
    alone for `SECONDS`.  A file that never stops changing is scanned
    anyway after 10 times `SECONDS`, and no more than 1000 extents per
    size tier wait like this; any more are scanned without waiting.
    The other scan modes only have the first rule.

 Nothing is skipped because of the delay, only postponed: the crawl
position saved in `beescrawl.dat` never moves past data that is still
waiting, so bees picks it up after a restart or a crash.  The times
are not saved, so after every start bees waits `SECONDS` before it
scans anything.  The space that duplicates use is freed `SECONDS`
later than without the option.

 The delay costs no wakeups on an idle filesystem.  bees polls for new
transactions as usual (every 30 seconds or less often), so data can
wait up to one polling interval longer than `SECONDS`.  A delay
shorter than the filesystem's commit interval (30 seconds by default)
is of little use.

 A file that was scanned earlier can still be opened as the _source_
 of a dedupe while it is being changed.

## Extent handling options

* `--keep-prealloc`

 Leave preallocated (`PREALLOC`, e.g. from `fallocate`) extents alone.

 By default, bees replaces each preallocated extent inside EOF with a
hole.  Reads still return zeros, but the space reservation is gone, so
an application that preallocated space to guarantee it can later write
there (for example a thick-provisioned VM disk image, or a database)
may get `ENOSPC` when the filesystem is full.

 With this option, bees skips preallocated extents entirely: it does not
punch holes in them, hash them or dedupe them.  Extents that contain
written zeros are not preallocated extents, and are still handled as
usual.

## Workarounds

* `--workaround-btrfs-send` or `-a`

 _This option is obsolete and should not be used any more._

 Pretend that read-only snapshots are empty and silently discard any
request to dedupe files referenced through them.  This is a workaround
for [problems with old kernels running `btrfs send` and `btrfs send
-p`](btrfs-kernel.md) which make these btrfs features unusable with bees.

 This option was used to avoid breaking `btrfs send` on old kernels.
 The affected kernels are now too old to be recommended for use with bees.

 bees now waits for `btrfs send` to finish.  There is no need for an
 option to enable this.

 **Note:** There is a _significant_ space tradeoff when using this option:
it is likely no space will be recovered--and possibly significant extra
space used--until the read-only snapshots are deleted.

## Logging options

* `--timestamps` or `-t`

 Enable timestamps in log output.

* `--no-timestamps` or `-T`

 Disable timestamps in log output.

* `--absolute-paths` or `-p`

 Paths in log output will be absolute.

* `--strip-paths` or `-P`

 Paths in log output will have the working directory at bees startup stripped.

* `--verbose` or `-v`

 Set log verbosity (0 = no output, 8 = all output, default 8).

* `--status-interval SECONDS`

 Write the status file `$BEESSTATUS` at most every `SECONDS` (default 1).
 0 disables the status file even if `$BEESSTATUS` is set.

 The file is rewritten when an event counter, the work queue or the
progress table changed.  While nothing changes, bees checks less and
less often and rewrites the file once a minute (or every `SECONDS`, if
that is longer), so the thread ages in the file can be that much out
of date when bees is idle or stuck.
