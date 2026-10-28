# Intel® DSA Transparent Offload Library
DSA Transparent Offload (DTO) is a shared library for applications to transparently (i.e., without modification) use DSA (Data Streaming Accelerator).
The library dynamically links to applications using "-ldto" linker options (requires recompilation).
The library can also be linked using LD_PRELOAD environment variable (doesn't require recompilication).
The library intercepts standard memcpy, memmove, memset, and memcmp standard API calls from the application
and transparently uses DSA to perform those operations using DSA's memory move, fill, and compare operations. DTO is limited to
synchronous offload model since these APIs have synchronous semantics.

DTO library works with DSA's Shared Work Queues (SWQs). DTO also works with multiple DSAs and uses them in round robin manner.
During initialization, DTO library can either auto-discover all configured SWQs (potentially on multiple DSAs), or a list of specific SWQs that is 
specified using an environment variable DTO_WQ_LIST.

DTO library falls back to using standard APIs on CPU under following scenarios:
   a. DTO library is not able to find any DSA instances (e.g., not configured/provisioned)
   b. Memory operation size is smaller than a offload threshold (configurable using an environment variable DTO_MIN_BYTES)
   c. Not able to do work submission (e.g., reached ENQ retry threshold) due to WQ full
   d. DSA encounters a page-fault and completes partially (resulting in rest of the operation being completed using standard library on CPU)

To improve throughput for synchronous offload, DTO uses "pseudo asynchronous" execution using following steps.
1) After intercepting the API call, DTO splits the API job into two parts; 1) CPU job and 2) DSA job. For example, a 64 KB memcpy may
   be split into 20 KB CPU job and 44 KB DSA job. The split fraction can be configured using an environment variable DTO_CPU_SIZE_FRACTION. 
2) DTO submits the DSA portion of the job to DSA. 
   If DTO_IS_NUMA_AWARE=1 DTO uses work queues of DSA device located on the same numa node as 
   buffer (memcpy/memmove - dest buffer, memcmp - ptr2) delivered to method - buffer-centric numa awareness.
   If DTO_IS_NUMA_AWARE=2 DTO uses work queues of DSA device located on the same numa node as 
   calling thread cpu - cpu-centric numa awareness.
3) In parallel, DTO performs the CPU portion of the job using std library on CPU.
4) DTO waits for DSA to complete (if it hasn't completed already). The wait method can be configured using an environment variable DTO_WAIT_METHOD.
   The wait method can be one of the following: yield, busypoll, umwait, or tpause. The default is busypoll.

For some workloads, complete offloading to the DSA device can result in improved performance and reduce power consumption via the UMWAIT (or TPAUSE) instruction.
In this case, DTO_CPU_SIZE_FRACTION can be set to 0.0, which means that the CPU job is 0 bytes and the entire job is offloaded to DSA and AUTO_ADJUST_KNOBS is set to 0.
Then during the wait step, DTO will use the UMWAIT (or TPAUSE) instruction to wait for the DSA job to complete. UMWAIT / TPAUSE are instructions that allow the CPU to enter a low-power state while waiting for the job to complete. For UMWAIT, please refer to this [enabling guide](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/monitor-umonitor-performance-guidance.html) for more details.



DTO also implements a heuristic to auto tune dsa_min_bytes and cpu_size_fraction parameters based on current DSA load. For example, if DSA is heavily loaded,
DTO tries to reduce the DSA load by increasing cpu_size_fraction and dsa_min_bytes. Conversely, if DSA is lightly loaded, DTO tries to increase the DSA load by
decreasing cpu_size_fraction and dsa_min_bytes. The goal of the heuristic is to minimize the wait time in step 4 above while maximizing throughput. The auto-tuning 
can be enabled or disabled using an environment variable DTO_AUTO_ADJUST_KNOBS.

DTO can also be used to learn certain application characterics by building histogram of various API types and sizes. The histogram can be built using an environment variable DTO_COLLECT_STATS.

```bash
dto.c: DSA Transparent Offload shared library
dto-test.c: Sample multi-threaded test application
test.sh: Sample test script to showcase how to use DTO with dto-test app (using both "-ldto" and "LD_PRELOAD" methods)
dto-4-dsa.conf:  An example json config file for configuring DSAs

Following environment variables control the behavior of DTO library:
	DTO_USESTDC_CALLS=0/1, 1 (uses std c memory functions only), 0 (uses DSA along with std c lib call; in case of DSA page fault - reverts to std c lib call). Default is 0.
        DTO_COLLECT_STATS=0/1, 1 (enables stats collection - #of operations, avg latency for each API, etc.>, 0 (disables stats collection).
                                Adds about 40 ns per call (two clock reads), whatever the number of threads. Default is 0. See Statistics below.
        DTO_STATS_FILE=<path> when DTO_COLLECT_STATS=1, write stats to the provided regular file instead of standard output. Each %p in
                                the path is replaced by the process ID. Records are appended, not truncated: processes started from this one
                                append to the same file unless the path has a %p. Failures fall back to standard output and emit an error.
        DTO_STATS_FORMAT=text/json  text (default): the tables shown below; json: JSON Lines, one record per line (see Statistics).
        DTO_STATS_INTERVAL_MS=N  when N > 0, also write what happened in every N ms, from a background thread. Default is 0.
	DTO_WAIT_METHOD=<yield,busypoll,umwait,tpause> (specifies the method to use while waiting for DSA to complete operation, default is busypoll)
	DTO_MIN_BYTES=xxxx (specifies minimum size of API call needed for DSA operation execution, default is 65536 bytes)
	DTO_CPU_SIZE_FRACTION=0.xx (specifies fraction of job performed by CPU, in parallel to DSA). Default is 0.00
	DTO_AUTO_ADJUST_KNOBS=0/1 (disables/enables auto tuning of cpu_size_fraction and dsa_min_bytes parameters. 0 -- disable, 1 -- enable (default))
   DTO_IS_NUMA_AWARE=0/1/2 (disables/buffer-centric/cpu-centric numa awareness. 0 -- disable (default), 1 -- buffer-centric, 2 - cpu-centric)
				Only available when DTO is built with NUMA support (see Build options).
	DTO_WQ_LIST="semi-colon(;) separated list of DSA WQs to use". The WQ names should match their names in /dev/dsa/ directory (see example below).
				If not specified, DTO will try to auto-discover and use all available WQs. Auto-discovery requires DTO to be
				built with accel-config support (see Build options); otherwise DTO_WQ_LIST is mandatory.
   DTO_DSA_MEMCPY=0/1, 1 (default) - DTO uses DSA to process memcpy, 0 - DTO uses system memcpy
   DTO_DSA_MEMMOVE=0/1, 1 (default) - DTO uses DSA to process memmove, 0 - DTO uses system memmove
   DTO_DSA_MEMSET=0/1, 1 (default) - DTO uses DSA to process memset, 0 - DTO use system memset
   DTO_DSA_MEMCMP=0/1, 1 (default) - DTO uses DSA to process memcmp, 0 - DTO use system memcmp
   DTO_DSA_CC=0/1, 1 (default) - DTO sets DSA Cache Control flag to 1 if DSA supports cache control, 0 - DTO sets DSA Cache Control flag to 0
   DTO_OVERLAPPING_MEMMOVE_ACTION=0/1 0 (default) DTO submits memmove operations with overlapping buffers entirely to CPU, 1 - entirely to DSA
   DTO_UMWAIT_DELAY=xxxx defines delay for umwait command (check max possible value at: /sys/devices/system/cpu/umwait_control/max_time), default is 100000
	DTO_LOG_FILE=<dto log file path> Redirect the DTO output to the specified file instead of std output (useful for debugging and statistics collection). file name is suffixed by the program name and process pid (<path>.<program>.<pid>).
	DTO_LOG_LEVEL=0/1/2 controls the log level. higher value means more verbose logging (default 0).
	DTO_CRC_MIN_BYTES=xxxx smallest buffer dto_crc32c_with_seed() sends to DSA; smaller ones are checksummed on the CPU (default: DTO_MIN_BYTES).
```

Although not the only usage models of DTO, the following are some common ones:
   Latency reduction - the goal is to minimize the latency of offloaded operations. Use the following settings:
      DTO_AUTO_ADJUST_KNOBS=1 (the CPU fraction setting is critical to this mode. The optimal value is dynamic so autotune algorithm needs to be enabled) 
      DTO_WAIT_METHOD=busypoll 

   Power Reduction - the goal is to reduce power by offloading memory operations to DSA allowing the cpu core to go into a lower power state. This mode may reduce or increase the latency of operations depending on the load on DSA devices.
      DTO_AUTO_ADJUST_KNOBS=0 
      DTO_CPU_SIZE_FRACTION=0.0  (offload the entire operations to DSA) 
      DTO_WAIT_METHOD=umwait 

   Cycle count Reduction - the goal is to reduce cpu cycles by offloading memory operations to DSA. This mode may reduce or increase the latency of operations depending on the load on DSA devices and on interaction with the OS scheduler and other threads. The idea is to offload operations to DSA and allow the OS to schedule other work while DSA perform the operation.
      DTO_AUTO_ADJUST_KNOBS=0 
      DTO_CPU_SIZE_FRACTION=0.0  (offload the entire operations to DSA) 
      DTO_WAIT_METHOD=yield

   Avoiding Cache polution - the goal is to avoid polluting the cache with data from the given process.
      DTO_DSA_CC=0
      DTO_AUTO_ADJUST_KNOBS=0 
      DTO_CPU_SIZE_FRACTION=0.0  (offload the entire operations to DSA so none of the data is pulled into cache) 
      DTO_WAIT_METHOD=yield or umwait (saves either cycles or power)


## Explicit asynchronous API

Besides intercepting the libc calls, libdto exports an explicit submit/poll
API, declared in `dto.h`, for applications that want to overlap DSA work with
their own: copy, fill, CRC32C (optionally fused with a copy), compare, compare
against a pattern and translation fetch, each as a single operation or batched
(up to 1024 operations in one DSA batch descriptor).

```c
#include <dto.h>

dto_async_op op;                    /* caller-owned, 64-byte aligned */

if (dto_submit_memcpy_crc(&op, dst, src, n, DTO_SUBMIT_CC) ==
    DTO_ASYNC_SUBMITTED) {
	do_other_work();
	dto_async_wait(&op);            /* waits with DTO_WAIT_METHOD */
	if (dto_async_poll(&op) == DTO_ASYNC_DONE)
		crc = dto_async_crc_val(&op);
	else
		redo_on_cpu();
} else {                            /* DTO_ASYNC_FALLBACK: nothing was done */
	redo_on_cpu();
}
```

A batch is built with `dto_batch_create`, `dto_batch_add_*` and
`dto_batch_submit`; `dto_batch_poll` redoes on the CPU anything the device
could not finish, so once it returns `DTO_ASYNC_DONE` every result is valid.

Explicit submits are not subject to `DTO_MIN_BYTES`: the caller decides what to
offload. `DTO_SUBMIT_BOF` makes the device wait for page faults to be
resolved; it requires a work queue configured with block-on-fault
(`accel-config config-wq --block-on-fault=1`), and on one without it the
device rejects the operation. Without the flag a page fault stops the
operation partway, on any work queue. `dto_async_crc_val` and `dto_batch_crc`
return the raw CRC32C (the SSE4.2 `crc32` instruction applied from 0, no final
inversion); the seeded `*_crc32c` calls return standard CRC32C and chain across
buffers. See `dto.h` for the full contract.

Programs that only use this API, and must not have their own large libc copies
sent to DSA, can link `libdto_explicit` instead: the same library without the
memcpy/memset/memmove/memcmp interposers (`-ldto_explicit`, or
`DTO::dto_explicit` from CMake).
## Using dtoctl

Setting `LD_PRELOAD` and exporting the `DTO_*` environment variables by hand is
error prone. The `dtoctl` tool provides a simpler interface: it takes the DTO
settings as command line options, converts them to the corresponding `DTO_*`
environment variables, prepends `libdto.so` to `LD_PRELOAD`, and then runs the
target program. For example, instead of

```bash
export LD_PRELOAD=/usr/local/lib/libdto.so
export DTO_WAIT_METHOD=busypoll
export DTO_CPU_SIZE_FRACTION=0.33
export DTO_AUTO_ADJUST_KNOBS=1
./prog
```

you can run

```bash
dtoctl -w busypoll -c 0.33 ./prog
```

Every environment variable listed above has a corresponding option, and option
values are checked before the program runs (DTO itself silently falls back to a
default on a value it does not understand). Only the options you pass are
applied: a `DTO_*` variable you already exported is left untouched, an explicit
option overrides it, and `-i/--ignore-env` drops all inherited `DTO_*`
variables.

The `libdto.so` path is resolved from `-l/--library`, then `$DTO_LIBRARY`, then
the library installed with `dtoctl`, then the compiled-in default
(`/usr/local/lib/libdto.so` for a CMake build, `/usr/lib64/libdto.so` for the
Makefile build). It is made absolute and prepended to any existing
`LD_PRELOAD`, and `dtoctl` fails if the library cannot be read rather than
running the program without DTO.

To see what would run without running it, use `--dry-run`:

```bash
$ dtoctl --dry-run -w umwait -b 16K ./prog
library: /usr/lib64/libdto.so.1.0 (compiled-in default)
LD_PRELOAD=/usr/lib64/libdto.so.1.0
DTO_WAIT_METHOD=umwait  # option
DTO_MIN_BYTES=16384  # option
command: ./prog
```

DSA work queues are often accessible to root only. Run `sudo dtoctl ...`
rather than `dtoctl sudo ...`, which loses `LD_PRELOAD` and the `DTO_*`
variables, and pass settings as options: `sudo` does not pass on variables
exported in your shell.

### Measuring the speedup from DSA

`dtoctl --stats-run` runs a program CPU-only (`DTO_USESTDC_CALLS=1`) and then
with DSA, collecting DTO's JSON stats (see [Statistics](#statistics)) from each
run into a directory, and `dto-stats-report` (installed with `dtoctl`) compares
them. With `--repeat=N` each way runs N times, alternating, and the report uses
the median wall time and, for the CPU and mem* times, the run with the median
CPU time; `--perf` adds one more run each way under `perf record`.

```bash
sudo taskset -c 2 dtoctl -w busypoll --no-auto-adjust -c 0 --stats-run=app-stats --repeat=3 --perf ./app
```

```
DTO stats run: app-stats
  command: ./app
  settings: DTO_AUTO_ADJUST_KNOBS=0 DTO_CPU_SIZE_FRACTION=0 DTO_WAIT_METHOD=busypoll
  timed runs: 3 CPU-only, 3 with DSA (medians)
  DTO in the DSA run: 1 WQs, wait busypoll, min_bytes 65536, cpu_size_fraction 0.0

                                     CPU only       with DSA   speedup
  wall time                            1.66 s         1.38 s     1.20x
  CPU time (user+sys)                  1.65 s         1.38 s     1.20x
    in mem* calls                   782.69 ms      493.11 ms     1.59x
    outside mem* calls              868.09 ms      885.71 ms     0.98x
  wall time, min-max              1.66-1.67 s    1.38-1.40 s

  Amdahl's law
    f, share of CPU time in mem* calls (CPU-only)     47.4%
    s, mem* speedup with DSA (time per byte)          1.59x
    predicted speedup, 1 / ((1 - f) + f / s)          1.21x
    measured speedup, wall time                       1.20x
    limit if mem* took no time, 1 / (1 - f)           1.90x

  per operation       calls       bytes   avg size   CPU only   with DSA  speedup  on DSA       f
  memcpy             20.00k     4.9 GiB  256.0 KiB  596.44 ms  180.24 ms    3.31x   99.4%   36.1%
  memset             20.00k     4.9 GiB  256.0 KiB   92.17 ms  100.59 ms    0.92x  100.0%    5.6%
  memcmp             20.00k     4.9 GiB  256.0 KiB   94.09 ms  212.28 ms    0.44x  100.0%    5.7%
    (on DSA: share of the bytes in calls sent to DSA, of which the CPU does cpu_size_fraction;
     f: share of the CPU-only run's CPU time)
    calls DSA left to the CPU, per run: page fault 128, retry 0, other 0

  by size (most time)       calls   CPU only   with DSA  speedup  on DSA
  256K-320K                60.00k   13.04 us    8.22 us    1.59x   99.8%
    (average time per call; on DSA: share of calls sent to DSA)

  profile (perf, % of CPU time)        CPU only   with DSA
    memcpy/memmove                        35.0%       0.3%
    memset                                 6.0%       0.0%
    memcmp                                 5.3%       0.0%
    CRC                                    9.8%      12.5%
    DTO library                            0.3%      28.7%
    other user code                       31.2%      43.8%
    other kernel code                     12.4%      14.8%

  note: 10% of the CPU-only run is CRC code: DSA can compute CRC32C, but DTO doesn't intercept CRC functions, so offloading it needs an explicit call
```

Here a single thread, on a core of the DSA's NUMA node, copies 256 KiB at a
time out of a 1 GiB buffer, so the data is not in the CPU caches, compares the
copy with the source, clears it, computes a CRC32C in software, and does other
work. The CPU-only run spent f = 47.4% of its CPU time in mem\* calls, and
those calls took s = 1.59x less time per byte with DSA, so Amdahl's law
predicts 1 / ((1 - f) + f / s) = 1.21x for the whole program; it measured
1.20x. At best, mem\* calls taking no time at all, it could run 1.90x faster.
The copies were 3.31x faster with DSA, but the compares, which read the copy
right after it is made, were slower: a copy on the CPU leaves the data in its
caches and a copy by DSA does not. The auto-tuner is off in this run
(`--no-auto-adjust -c 0`); with it on, it gave part of each copy to the CPU and
the program ran only 1.02x faster.

The report also shows each operation and the sizes with the most time, so you
can see where DSA wins and set `DTO_MIN_BYTES` accordingly, and notes anything
that makes the comparison doubtful. Among them: no call reaching DSA (no usable
work queue: run with `sudo`), and runs that did different amounts of work, as
a benchmark that runs for a fixed time does (speedups are then per byte). The
perf profile splits each run's CPU time into copies, memset, memcmp, CRC,
kernel copies, DTO itself (here, busy-polling for completions) and the rest. It
also counts mem\* calls DTO cannot intercept, such as those made inside libc.
Run `dto-stats-report app-stats` again at any time, or with `--json` for the
numbers. Amdahl's estimate assumes the program keeps its CPUs busy; one that
waits for I/O gains less in wall time.

Run `dtoctl --help` for the option list, or `man 1 dtoctl` for the full manual,
which documents each option and gives a ready-to-run command for each of the
usage models above.

## Build

Pre-requisite packages:

Should use glibc version 2.36 or later for best DTO performance. You can use "ldd --version" command to check glibc version on your system. glibc versions less than 2.36 have a bug which reduces DTO performance.

On Fedora/CentOS/Rhel: kernel-headers, accel-config-devel, libuuid-devel, libnuma-devel

On Ubuntu/Debian: linux-libc-dev, libaccel-config-dev, uuid-dev, libnuma-dev

The accel-config and numa packages are only required when the corresponding build options are enabled (they are enabled by default, see [Build options](#build-options) below).

### CMake ###
```bash
cmake -B build
cmake --build build -j
sudo cmake --install build
```

#### Build options ####

The accel-config and libnuma dependencies are optional and can be disabled at configure time:

| Option | Default | Description |
| --- | --- | --- |
| `DTO_ACCEL_CONFIG_SUPPORT` | `ON` | Use libaccel-config to auto-discover configured DSA work queues. When disabled, libdto does not link against libaccel-config, and the `DTO_WQ_LIST` environment variable **must** be set at runtime (e.g., `export DTO_WQ_LIST="wq0.0;wq1.0"`) since auto-discovery is unavailable. |
| `DTO_NUMA_SUPPORT` | `ON` | Use libnuma for NUMA-aware work queue selection. When disabled, libdto does not link against libnuma and the `DTO_IS_NUMA_AWARE` environment variable has no effect (work queues are used in round-robin order). |
| `DTO_STATIC_ACCEL_CONFIG` | `OFF` | Statically link libaccel-config into libdto.so instead of depending on the shared library at runtime. Requires `libaccel-config.a` to be installed. |
| `DTO_STATIC_NUMA` | `OFF` | Statically link libnuma into libdto.so instead of depending on the shared library at runtime. Requires a `libnuma.a` compiled with `-fPIC`. |
| `DTO_BUILD_EXPLICIT` | `ON` | Also build `libdto_explicit`, the explicit API (`dto.h`) without the libc interposers. |
| `DTO_BUILD_TESTS` | `ON` (`OFF` as a subproject) | Build the test suite (see [TESTING.md](TESTING.md)). |
| `DTO_BUILD_TEST_APPS` | `ON` (`OFF` as a subproject) | Build the `dto-test` and `dto-test-wodto` applications. |

Example: build without either dependency:
```bash
cmake -B build -DDTO_ACCEL_CONFIG_SUPPORT=OFF -DDTO_NUMA_SUPPORT=OFF
cmake --build build -j
```

The deprecated Makefile always builds with both features enabled (it passes
`-DDTO_ACCEL_CONFIG_SUPPORT -DDTO_NUMA_SUPPORT` to the compiler); use CMake if
you need to build without them.

The CMake build also produces the test applications in the build directory:
`dto-test` (for the -ldto method) and `dto-test-wodto` (for the LD_PRELOAD method).

DTO can also be built as part of another CMake project, with `add_subdirectory`
or `FetchContent`, and linked as `DTO::dto`. Only the library is built then,
unless `DTO_BUILD_TESTS` or `DTO_BUILD_TEST_APPS` is set, and the parent
project's build type is left as it is:
```cmake
include(FetchContent)
FetchContent_Declare(dto GIT_REPOSITORY https://github.com/intel/DTO.git GIT_TAG main)
FetchContent_MakeAvailable(dto)
target_link_libraries(myapp PRIVATE DTO::dto)
```

CMake installs the library to `/usr/local/lib` by default, while the deprecated
Makefile installs to `/usr/lib64`. Pass `-DCMAKE_INSTALL_PREFIX=<prefix>` at
configure time to change the install location.

### Makefile (deprecated) ###
```bash
make libdto
sudo make install
```

Building the test application with the Makefile:
```bash
# When using -ldto method
make dto-test
# When using LD_PRELOAD method
make dto-test-wodto

```
## Initializing DSA devices

You can initialize a DSA device using the following:
```bash
./accelConfig.sh <device-id> <enable (yes/no)> <wq-id>
```
If you get invalid traffic class error, reload the idxd driver with tc_override=1:
```bash
modprobe -r idxd
modprobe idxd tc_override=1
```
You can set the permissions of the DSA via chmod /dev/dsa/wqX.Y to allow users to submit
to the DSA device.

## Test
```bash
1. Follow the steps in the Initializing DSA Devices section. Or you can make changes to test.sh or dto-4-dsa.conf to change DSA configuration if desired. test.sh configures DSA(s) using the config parameters in dto-4-dsa.conf.
2. Run the test.sh script to run the dto-test app (DTO linked using -ldto) or dto-test-wodto app (DTO linked using LD_PRELOAD).
3. Using with other applications (two ways to use it)
    3a. Using "-ldto" linker option (requires recompiling the application)
        i. Recompile the application with "-ldto" linker options
	ii. Setup DTO environment variables (examples below)
            export DTO_USESTDC_CALLS=0
            export DTO_COLLECT_STATS=1
            export DTO_WAIT_METHOD=busypoll
            export DTO_MIN_BYTES=8192
            export DTO_CPU_SIZE_FRACTION=0.33
            export DTO_AUTO_ADJUST_KNOBS=1
            export DTO_WQ_LIST="wq0.0;wq2.0;wq4.0;wq6.0"
            export DTO_IS_NUMA_AWARE=1
	iii. Run the application - (CacheBench example below)
    3b. Using LD_PRELOAD method (doesn not require recompiling the application)
	i. setup LD_PRELOAD environment variable to point to DTO library
            export  LD_PRELOAD=<libdto file path>:$LD_PRELOAD
	ii. Export all environment variables (similar to ii. in option 3a. above)
	iii. Run the application - (CacheBench example below)

	<CBENCH_DIR>/cachebench -json_test_config <json file> --progress_stats_file=dto.log --report_api_latency

4. Sample stats output (DTO_COLLECT_STATS=1; see [Statistics](#statistics))
	i. Numbers under columns set, cpy, mov, and cmp show number of API calls or per-API completion latency for memset, memcpy, memmove, and memcmp respectively.
	ii. Numbers in bytes column show total bytes processed across all 4 API calls

------------------------------------------------------------------------------------------------------------------------------------------

```
DTO Run Time: 8 ms

******** Number of Memory Operations ********
                           <*************** stdc calls    ***************> <*************** dsa (success) ***************> <*************** dsa (failed)  ***************> <***** failure reason *****>
Byte Range              -- set      cpy      mov      cmp      bytes        set      cpy      mov      cmp      bytes        set      cpy      mov      cmp      bytes        Retry  PFs    Others
      65536-81919       -- 0        0        0        0        0            200      0        0        0        13926400     0        0        0        0        0            0      0      0
      98304-114687      -- 0        0        200      0        19662200     0        0        0        0        0            0        0        0        0        0            0      0      0
     131072-163839      -- 0        0        0        0        0            0        200      0        0        26239000     0        0        0        0        0            0      0      0
     262144-327679      -- 0        0        0        0        0            0        0        0        200      52441600     0        0        0        0        0            0      0      0

******** Average Memory Operation Latency (us)  ********
                           <******** stdc calls    ********> <******** dsa (success) ********> <******** dsa (failed)  ********>
Byte Range              -- set      cpy      mov      cmp      set      cpy      mov      cmp      set      cpy      mov      cmp
      65536-81919       -- 0        0        0        0        8.06     0        0        0        0        0        0        0
      98304-114687      -- 0        0        4.18     0        0        0        0        0        0        0        0        0
     131072-163839      -- 0        0        0        0        0        17.02    0        0        0        0        0        0
     262144-327679      -- 0        0        0        0        0        0        0        34.50    0        0        0        0
```

## Statistics

With `DTO_COLLECT_STATS=1`, DTO times and counts every memset, memcpy,
memmove and memcmp it intercepts, by operation, by path (CPU, DSA, or DSA
failed and finished on the CPU) and by size, and writes a summary when the
process exits (or on Ctrl-C). Each thread counts on its own, so collection
costs about 40 ns per call however many threads there are, and the counts
are summed for output.

The size histogram has a bucket for calls under 64 bytes, then four buckets
per power of two up to 4 GiB (65536-81919, 81920-98303, ...), and one for
anything larger. A call that DSA completed only partly (for example on a
page fault) is counted once as `dsa (failed)` and once as a CPU call for the
remainder.

With `DTO_STATS_INTERVAL_MS=N`, a background thread also writes, every N ms,
what happened since the previous record, so activity can be followed over
time. The interval records add up to the summary's totals.

With `DTO_STATS_FORMAT=json` the output is JSON Lines, one record per line:

```
{"type":"interval","pid":183654,"t_ms":14.333,"dt_ms":14.333,"ops":{"memcpy":{"dsa":{"calls":8,"bytes":1049560,"ns":96273}},"memmove":{"cpu":{"calls":8,"bytes":786488,"ns":31601}}},"failures":{"retry":0,"page_fault":0,"other":0}}
{"type":"summary","pid":183654,"program":"prog","run_ms":23.395,
 "config":{"stdc_only":0,"wait_method":"busypoll","min_bytes":65536,"cpu_size_fraction":0.00,"auto_adjust":1,"numa_aware":0,"wqs":1},
 "totals":{"memcpy":{"dsa":{"calls":12,"bytes":1574340,"ns":138591}},...},
 "failures":{"retry":0,"page_fault":0,"other":0},
 "histogram":[{"min":131072,"max":163839,"ops":{"memcpy":{"dsa":{"calls":12,"bytes":1574340,"ns":138591}}},"failures":{...}},...]}
```

(The summary is a single line; it is wrapped here.) `ops` maps `memset`,
`memcpy`, `memmove` and `memcmp` to the paths they took, `cpu`, `dsa` and
`dsa_failed`, each with the number of calls, bytes and total nanoseconds;
operations and paths with no calls are left out. `t_ms` and `run_ms` count
from DTO's start, and `config` holds the settings at exit, including the
auto-tuned `min_bytes` and `cpu_size_fraction`.

To compare a run with DSA against a CPU-only run, and see how close the speedup
comes to what Amdahl's law predicts, use `dtoctl --stats-run` (see
[Measuring the speedup from DSA](#measuring-the-speedup-from-dsa)).

Every process that loads DTO with stats enabled writes its own records,
tagged with its `pid`. They share `DTO_STATS_FILE` (records are appended
whole) unless the path contains `%p`, which gives each process its own file.

## Usage Notes

When linking DTO using LD_PRELOAD environment variable special care is required when the application is called from within a shell script.
   - It is best to set the LD_PRELOAD variable as close to the invocation of the actual application. For environments that use nested shell scripts
     calls, it is ideal to set LD_PRELOAD within the script that invokes the application, and not in outer scripts.
   - Setting the LD_PRELOAD variable before invoking the shell script which eventually invokes the application can lead to the following problems:
      - Some shell commands rely on stdout output, for example the output of 'pwd', to set internal variables or environment variables. Setting 
        DTO_LOG_LEVEL > 0 can cause problems because text output by DTO can be interspersed with output from the shell script causing errors 
        in the script.
      - When the application is started by a script with #!<location of shell> which invokes another script with #!<location of shell>, for 
        unknown reasons DTO causes a segmentation fault during a memset operation on an 8K sized buffer. This can be avoided by setting the minimum 
        DTO size above 8K, or by avoiding this invocation sequence.
