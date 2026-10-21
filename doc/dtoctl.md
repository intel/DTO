% DTOCTL(1) Dtoctl User Manuals
% Honggyu Kim <honggyu.kim@sk.com>
% 2026

NAME
====
dtoctl - run a program with DSA Transparent Offload (DTO) preloaded


SYNOPSIS
========
dtoctl [_options_] COMMAND [_command-options_]


DESCRIPTION
===========
The **dtoctl** tool runs `COMMAND` with the DSA Transparent Offload library
(**libdto.so**) preloaded, so that its `memcpy`, `memmove`, `memset`, and
`memcmp` calls are transparently offloaded to Intel DSA (Data Streaming
Accelerator).

Without **dtoctl**, users must set **LD_PRELOAD** to point at **libdto.so** and
export a set of **DTO_\*** environment variables to control DTO behavior.
**dtoctl** replaces that manual setup: it converts its command line options into
the corresponding **DTO_\*** environment variables, prepends **libdto.so** to
**LD_PRELOAD**, and then executes `COMMAND`.

Only the options that are explicitly given are applied.  Options that are not
given leave any pre-existing **DTO_\*** environment variables (or the DTO
built-in defaults) untouched, and an explicitly given option overrides a
previously exported variable of the same name.


LIBRARY RESOLUTION
==================
The path of **libdto.so** to preload is resolved in the following order:

1. The `-l`, `--library` option, if given.
2. The **DTO_LIBRARY** environment variable, if set.
3. The compiled-in default path (`/usr/local/lib/libdto.so` for a default
   CMake build, or `/usr/lib64/libdto.so` for the Makefile build).

The resolved path is prepended to any existing **LD_PRELOAD** value.


OPTIONS
=======

Library
-------
-l _PATH_, \--library=_PATH_
:   Path to **libdto.so** to prepend to **LD_PRELOAD**.  Overrides
    **DTO_LIBRARY** and the compiled-in default.
    Default: the compiled-in library path (see LIBRARY RESOLUTION).

Offload tuning
--------------
-w _METHOD_, \--wait-method=_METHOD_
:   How to wait for DSA completion: _yield_, _busypoll_, _umwait_, or _tpause_.
    Sets **DTO_WAIT_METHOD**.  Default: _busypoll_.

-b _BYTES_, \--min-bytes=_BYTES_
:   Minimum operation size offloaded to DSA.  Smaller operations run on the CPU.
    Sets **DTO_MIN_BYTES**.  Default: 65536.

-c _FRACTION_, \--cpu-fraction=_FRACTION_
:   Fraction (0.0 <= f < 1.0) of each operation performed on the CPU in parallel
    to DSA.  Sets **DTO_CPU_SIZE_FRACTION**.  Default: 0.0.

-n _MODE_, \--numa-aware=_MODE_
:   NUMA awareness: _0_ disable, _1_ buffer-centric, _2_ cpu-centric.
    Sets **DTO_IS_NUMA_AWARE**.  Default: _0_.

-q _LIST_, \--wq-list=_LIST_
:   Semicolon-separated list of DSA work queues to use, e.g. "wq0.0;wq2.0".
    Names must match those under /dev/dsa/.  Sets **DTO_WQ_LIST**.
    Default: auto-discover all available WQs.

\--umwait-delay=_CYCLES_
:   Delay in cycles for the umwait instruction.  Sets **DTO_UMWAIT_DELAY**.
    Default: 100000.

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
:   Enable stats collection.  For debugging/profiling only, as it slows down the
    workload.  Sets **DTO_COLLECT_STATS=1**.  Default: off.

\--stats-file=_PATH_
:   Write the stats histogram to _PATH_ instead of standard output.
    Sets **DTO_STATS_FILE**.  Default: standard output.

\--log-file=_PATH_
:   Redirect DTO output to _PATH_ (the file name is suffixed by the pid).
    Sets **DTO_LOG_FILE**.  Default: standard output.

\--log-level=_LEVEL_
:   Verbosity of DTO logging: _0_, _1_, or _2_.  Sets **DTO_LOG_LEVEL**.
    Default: _0_.

-?, \--help
:   Print help message and list of options with description.

\--usage
:   Print usage string.


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
stats:

    $ dtoctl -l ./libdto.so.1.0 -q "wq0.0;wq2.0;wq4.0;wq6.0" --stats ./prog


SEE ALSO
========
**ld.so**(8)
