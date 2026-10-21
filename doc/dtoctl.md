% DTOCTL(1) Dtoctl User Manuals
% Honggyu Kim <honggyu.kim@sk.com>
% 2026

NAME
====
dtoctl - run a program with DSA Transparent Offload (DTO) preloaded


SYNOPSIS
========
dtoctl [_options_] COMMAND [_command-options_]

dtoctl \--dry-run [_options_] [COMMAND [_command-options_]]

dtoctl \--stats-run[=_DIR_] [\--repeat=_N_] [\--perf[=_PERF_]] [_options_] COMMAND [_command-options_]


DESCRIPTION
===========
The **dtoctl** tool runs `COMMAND` with the DSA Transparent Offload library
(**libdto.so**) preloaded, so that its `memcpy`, `memmove`, `memset`, and
`memcmp` calls are transparently offloaded to Intel DSA (Data Streaming
Accelerator).

Without **dtoctl**, users must set **LD_PRELOAD** to point at **libdto.so** and
export a set of **DTO_\*** environment variables to control DTO behavior.
**dtoctl** replaces that manual setup: it checks its command line options,
converts them into the corresponding **DTO_\*** environment variables, prepends
**libdto.so** to **LD_PRELOAD**, and then executes `COMMAND`.  Nothing is left
behind in the calling shell.

Only the options that are explicitly given are applied.  Options that are not
given leave any pre-existing **DTO_\*** environment variables (or the DTO
built-in defaults) untouched, and an explicitly given option overrides a
previously exported variable of the same name.  Use `-i` to ignore the
inherited **DTO_\*** variables instead.

Option values are validated before `COMMAND` runs, and an invalid value is an
error.  DTO itself silently falls back to a default when a variable holds a
value it does not understand.

`COMMAND` and every process it starts inherit the settings, so
`dtoctl make` runs each compiler under DTO as well.


LIBRARY RESOLUTION
==================
The path of **libdto.so** to preload is resolved in the following order:

1. The `-l`, `--library` option, if given.
2. The **DTO_LIBRARY** environment variable, if set and not empty.  This
   variable is read by **dtoctl** only, not by **libdto.so**.
3. For a CMake build, the **libdto.so** installed with **dtoctl**, found
   relative to the **dtoctl** executable (`<bindir>/../lib/libdto.so`), so
   that an installation moved with `cmake --install --prefix` finds its own
   library.
4. The compiled-in default path (`/usr/local/lib/libdto.so` for a default
   CMake build, or `/usr/lib64/libdto.so` for the Makefile build).

The resolved path is made absolute, so it keeps working when `COMMAND` changes
directory, and **dtoctl** fails with exit status 125 if it cannot read the
library.  (**ld.so** would only print a warning and run `COMMAND` without DTO.)
Run `dtoctl --dry-run` to see which library was chosen and why.

The library is prepended to any existing **LD_PRELOAD** value.  Any other
**libdto.so** entry already in **LD_PRELOAD** is removed, with a warning when it
names a different file, so that one process never loads two copies of DTO.


OPTIONS
=======

General
-------
-l _PATH_, \--library=_PATH_
:   Path to **libdto.so** to prepend to **LD_PRELOAD**.  Overrides
    **DTO_LIBRARY** and the installed library (see LIBRARY RESOLUTION).

-i, \--ignore-env
:   Unset every **DTO_\*** variable inherited from the environment, including
    **DTO_LIBRARY**, so that only the options given on this command line and
    the DTO built-in defaults apply.  This is the way to undo an exported
    variable that the `--no-*` options cannot, such as
    **DTO_USESTDC_CALLS=1** or **DTO_DSA_MEMCPY=0**.

\--dry-run
:   Print the resolved library and where it came from, the resulting
    **LD_PRELOAD**, every **DTO_\*** variable that `COMMAND` would see (each
    marked `option` or `inherited`), and the command line, then exit without
    running anything.  `COMMAND` may be omitted.

Offload tuning
--------------
-w _METHOD_, \--wait-method=_METHOD_
:   How to wait for DSA completion: _yield_, _busypoll_, _umwait_, or _tpause_.
    _umwait_ and _tpause_ need a CPU with WAITPKG support; DTO falls back
    otherwise.  Sets **DTO_WAIT_METHOD**.  Default: _busypoll_.

-b _BYTES_, \--min-bytes=_BYTES_
:   Minimum operation size offloaded to DSA.  Smaller operations run on the CPU.
    _BYTES_ may have a _K_, _M_, or _G_ suffix (powers of 1024), and is passed to
    DTO as a plain byte count.  With auto tuning on, this is the starting
    value.  Sets **DTO_MIN_BYTES**.  Default: 64K.

-c _FRACTION_, \--cpu-fraction=_FRACTION_
:   Fraction (0.0 <= f < 1.0) of each operation performed on the CPU in parallel
    to DSA.  DTO uses two decimal places.  With auto tuning on, this is the
    starting value.  Sets **DTO_CPU_SIZE_FRACTION**.  Default: 0.0.

-n _MODE_, \--numa-aware=_MODE_
:   NUMA awareness: _0_ disable, _1_ buffer-centric, _2_ cpu-centric.  Has no
    effect if DTO was built without NUMA support.  Sets **DTO_IS_NUMA_AWARE**.
    Default: _0_.

-q _LIST_, \--wq-list=_LIST_
:   Semicolon-separated list of DSA work queues to use, e.g. "wq0.0;wq2.0".
    Names must match those under /dev/dsa/, and the list can be at most 255
    characters.  Required if DTO was built without accel-config support.
    Sets **DTO_WQ_LIST**.  Default: auto-discover all available WQs.

\--umwait-delay=_CYCLES_
:   Delay in cycles for the umwait instruction, a positive number.  The
    maximum is in /sys/devices/system/cpu/umwait_control/max_time.  Sets
    **DTO_UMWAIT_DELAY**.  Default: 100000.

\--overlapping-memmove=_WHERE_
:   Where to run memmove with overlapping buffers: _cpu_ or _dsa_.
    Sets **DTO_OVERLAPPING_MEMMOVE_ACTION** (cpu -> 0, dsa -> 1).  Default: _cpu_.

Disable DSA offload for specific operations
-------------------------------------------
By default every one of these operations is offloaded to DSA; each option below
turns a single one back into its standard C library call on the CPU.

\--no-memcpy
:   Use the system memcpy instead of DSA.  Sets **DTO_DSA_MEMCPY=0**.

\--no-memmove
:   Use the system memmove instead of DSA.  Sets **DTO_DSA_MEMMOVE=0**.

\--no-memset
:   Use the system memset instead of DSA.  Sets **DTO_DSA_MEMSET=0**.

\--no-memcmp
:   Use the system memcmp instead of DSA.  Sets **DTO_DSA_MEMCMP=0**.

Other toggles
-------------
\--no-cache-control
:   Clear the DSA cache control flag to avoid cache pollution.
    Sets **DTO_DSA_CC=0**.  Default: cache control is on.

\--no-auto-adjust
:   Disable auto tuning of cpu-fraction and min-bytes.
    Sets **DTO_AUTO_ADJUST_KNOBS=0**.  Default: auto tuning is on.

\--stdc-only
:   Use only the standard C memory functions, no DSA offload.
    Sets **DTO_USESTDC_CALLS=1**.  Default: DSA offload is enabled.

Statistics and logging
----------------------
\--stats
:   Enable stats collection: calls, bytes and time for each operation, by
    path (CPU, DSA, DSA failed) and by size.  Costs about 40 ns per call.
    Sets **DTO_COLLECT_STATS=1**.  Default: off.

\--stats-file=_PATH_
:   Write the stats to _PATH_ instead of standard output; each `%p` in _PATH_
    becomes the process ID.  The file is appended to.  Implies `--stats`,
    since DTO ignores the file otherwise.  Sets **DTO_STATS_FILE** and
    **DTO_COLLECT_STATS=1**.  Default: standard output.

\--stats-format=_FORMAT_
:   _text_ for tables, or _json_ for JSON Lines (one record per line).
    Implies `--stats`.  Sets **DTO_STATS_FORMAT**.  Default: _text_.

\--stats-interval=_MS_
:   Also write the stats for every _MS_ milliseconds, from a background
    thread, as well as the summary at exit.  Implies `--stats`.  Sets
    **DTO_STATS_INTERVAL_MS**.  Default: summary only.

\--log-file=_PATH_
:   Redirect DTO output to _PATH_._program_._pid_, one file per process.
    Sets **DTO_LOG_FILE**.  Default: standard output.

\--log-level=_LEVEL_
:   Verbosity of DTO logging: _0_, _1_, or _2_.  Sets **DTO_LOG_LEVEL**.
    Default: _0_.

Stats run
---------
\--stats-run[=_DIR_]
:   Measure how much DSA speeds `COMMAND` up.  Run it CPU-only
    (**DTO_USESTDC_CALLS=1**), then with DSA (**DTO_USESTDC_CALLS=0**),
    collecting JSON stats from each run into _DIR_, then summarize them with
    **dto-stats-report** (see STATS RUN).  _DIR_ must be new or empty; the
    default is a new `dto-stats-<date>-<time>` in the current directory.
    Give _DIR_ with `=`: `--stats-run=out`.  The other options apply to every
    run; `--stdc-only`, `--stats-file` and `--stats-format` cannot be used,
    since each run sets them.

\--repeat=_N_
:   With `--stats-run`, run each way _N_ times, alternating; the report takes
    the median times.  Default: 1.

\--perf[=_PERF_]
:   With `--stats-run`, also run each way once more under
    `PERF record -e cpu-clock`, to show where the CPU time went.  _PERF_ is
    the **perf** to run.  Default: **perf** from **PATH**.

-?, \--help
:   Print help message and list of options with description.

\--usage
:   Print usage string.

-V, \--version
:   Print the version.


EXIT STATUS
===========
On success **dtoctl** does not return: `COMMAND` replaces it, so the exit
status is that of `COMMAND`.  Otherwise:

64
:   Invalid option or option value, or no `COMMAND`.

125
:   **dtoctl** itself failed, for example because the library cannot be read.

126
:   `COMMAND` was found but cannot be run.

127
:   `COMMAND` was not found.

`--dry-run`, `--help`, and `--version` exit with 0.

With `--stats-run`, a run that fails stops the rest, and **dtoctl** exits with
its status (128 + the signal number if it was killed); otherwise the exit
status is that of **dto-stats-report**.


STATS RUN
=========
`--stats-run` answers two questions: how much faster is `COMMAND` with DSA,
and how much faster could it be?

Each run's DTO stats give the time spent in the intercepted calls, and
**dtoctl** records each run's wall time and CPU time (user + system, for
`COMMAND` and all its children).  **dto-stats-report** then works out:

f
:   the share of the CPU-only run's CPU time spent in mem\* calls;

s
:   how much faster the mem\* calls were with DSA, per byte;

predicted speedup
:   by Amdahl's law, 1 / ((1 - f) + f / s), set against the measured
    speedup in wall time;

limit
:   1 / (1 - f), the speedup if the mem\* calls took no time at all.

It also lists each operation and the sizes with the most time, CPU-only and
with DSA, and notes where DSA lost, where the runs differed, and why no call
may have reached DSA.  With `--perf` it divides each run's CPU time into
copies, memset, memcmp, CRC, kernel copies, DTO itself and other code, from
the profile, which also catches mem\* calls DTO cannot intercept.

Amdahl's estimate assumes `COMMAND` is busy on the CPU; one that waits for
I/O or for other threads gains less in wall time.  A benchmark that runs for
a fixed time does more work when faster: the report says so when the runs
copied different amounts, and compares speed per byte.

The directory holds `runs.jsonl` (the command, settings, and each run's times
and exit status), `<run>.jsonl` (DTO's stats for each run, from every process
it started), and with `--perf`, `<mode>-perf.data`.  Run
**dto-stats-report** _DIR_ again at any time, with `--json` for the numbers
as JSON.  DTO writes the stats files readable by their owner only, so a run
made with **sudo** needs **sudo** to report on.


NOTES
=====
DSA work queues under /dev/dsa/ are often accessible to root only.  Put
**sudo** in front of **dtoctl**, not between **dtoctl** and `COMMAND`:

    $ sudo dtoctl -w busypoll ./prog     # works
    $ dtoctl -w busypoll sudo ./prog     # sudo drops LD_PRELOAD and DTO_*

By default **sudo** also resets the environment, so **DTO_\*** variables
exported in the calling shell do not reach `sudo dtoctl`.  Pass the settings as
**dtoctl** options instead.


EXAMPLES
========
Run a program with DTO using the installed **libdto.so**:

    $ dtoctl ./prog

The equivalent of the manual setup

    $ export LD_PRELOAD=/usr/lib64/libdto.so
    $ export DTO_WAIT_METHOD=busypoll
    $ export DTO_CPU_SIZE_FRACTION=0.33
    $ export DTO_AUTO_ADJUST_KNOBS=1
    $ ./prog

becomes a single command:

    $ dtoctl -w busypoll -c 0.33 ./prog

Latency reduction mode (auto tuning on, busy polling):

    $ dtoctl -w busypoll ./prog

Power reduction mode (offload everything to DSA, wait with umwait):

    $ dtoctl -w umwait -c 0.0 --no-auto-adjust ./prog

Avoid cache pollution (offload everything, clear cache control):

    $ dtoctl -w yield -c 0.0 --no-auto-adjust --no-cache-control ./prog

Use a locally built library and a specific set of work queues, and collect
stats into a file:

    $ dtoctl -l ./libdto.so.1.0 -q "wq0.0;wq2.0;wq4.0;wq6.0" \
          --stats-file=/tmp/dto-stats.txt ./prog

Measure the speedup from DSA, three runs each way, then profile:

    $ sudo dtoctl --stats-run=prog-stats --repeat=3 --perf ./prog

Check what would run, ignoring anything exported in the shell:

    $ dtoctl --dry-run -i -w umwait -b 16K ./prog
    library: /usr/lib64/libdto.so.1.0 (compiled-in default)
    LD_PRELOAD=/usr/lib64/libdto.so.1.0
    DTO_WAIT_METHOD=umwait  # option
    DTO_MIN_BYTES=16384  # option
    command: ./prog


SEE ALSO
========
**ld.so**(8), **accel-config**(1), **perf-record**(1)
