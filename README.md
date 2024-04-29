# Intel® DSA Transparent Offload Library
DSA Transparent Offload (DTO) is a shared library for applications to transparently (i.e., without modification) use DSA (Data Streaming Accelerator).
The library dynamically links to applications using "-ldto" linker options (requires recompilation).
The library can also be linked using LD_PRELOAD environment variable (doesn't require recompilication).
The library intercepts standard memcpy, memmove, memset, and memcmp standard API calls from the application
and transparently uses DSA to perform those operations using DSA's memory move, fill, and compare operations. DTO is limited to
synchronous offload model since these APIs have synchronous semantics.

DTO library works with DSA's Shared and Dedicated Work Queues (SWQs). DTO also works with multiple DSAs and uses them in round robin manner.
During initialization, DTO library can either auto-discover all configured SWQs (potentially on multile DSAs), or a list of specific SWQs that is
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
				Should be enabled for debugging/profiling only, not for perf evaluation (enabling it slows down the workload). Default is 0.
	DTO_WAIT_METHOD=<yield,busypoll,umwait> (specifies the method to use while waiting for DSA to complete operation, default is yield)
	DTO_MIN_BYTES=xxxx (specifies minimum size of API call needed for DSA operation execution, default is 8192 bytes)
	DTO_CPU_SIZE_FRACTION=0.xx (specifies fraction of job performed by CPU, in parallel to DSA). Default is 0.00
	DTO_AUTO_ADJUST_KNOBS=0/1 (disables/enables auto tuning of cpu_size_fraction and dsa_min_bytes parameters. 0 -- disable, 1 -- enable (default))
   DTO_IS_NUMA_AWARE=0/1/2 (disables/buffer-centric/cpu-centric numa awareness. 0 -- disable (default), 1 -- buffer-centric, 2 - cpu-centric)
	DTO_WQ_LIST="semi-colon(;) separated list of DSA WQs to use". The WQ names should match their names in /dev/dsa/ directory (see example below).
				If not specified, DTO will try to auto-discover and use all available WQs.
   DTO_DSA_MEMCPY=0/1, 1 (default) - DTO uses DSA to process memcpy, 0 - DTO uses system memcpy
   DTO_DSA_MEMMOVE=0/1, 1 (default) - DTO uses DSA to process memmove, 0 - DTO uses system memmove
   DTO_DSA_MEMSET=0/1, 1 (default) - DTO uses DSA to process memset, 0 - DTO use system memset
   DTO_DSA_MEMCMP=0/1, 1 (default) - DTO uses DSA to process memcmp, 0 - DTO use system memcmp
   DTO_ENQCMD_MAX_RETRIES - defines maximal number of retries for enquing command into DSA queue, default is 3
   DTO_UMWAIT_DELAY - defines delay for umwait command (check max possible value at: /sys/devices/system/cpu/umwait_control/max_time), default is 100000
	DTO_LOG_FILE=<dto log file path> Redirect the DTO output to the specified file instead of std output (useful for debugging and statistics collection). file name is suffixed by process pid.
	DTO_LOG_LEVEL=0/1/2 controls the log level. higher value means more verbose logging (default 0).
```

## Build

Pre-requisite packages:

Should use glibc version 2.36 or later for best DTO performance. You can use "ldd --version" command to check glibc version on your system. glibc versions less than 2.36 have a bug which reduces DTO performance.

On Fedora/CentOS/Rhel: kernel-headers, accel-config-devel, libuuid-devel, libnuma-devel

On Ubuntu/Debian: linux-libc-dev, libaccel-config-dev, uuid-dev, libnuma-dev

```bash
make libdto
make install
```

Building the test application.
```bash
# When using -ldto method
make dto-test
# When using LD_PRELOAD method
make dto-test-wodto

```

## Test
```bash
1. Make changes to test.sh or dto-4-dsa.conf to change DSA configuration if desired. test.sh configures DSA(s) using the config parameters in dto-4-dsa.conf
2. Run the test.sh script to run the dto-test app (DTO linked using -ldto) or dto-test-wodto app (DTO linked using LD_PRELOAD).
3. Using with other applications (two ways to use it)
    3a. Using "-ldto" linker option (requires recompiling the application)
        i. Recompile the application with "-ldto" linker options
	ii. Setup DTO environment variables (examples below)
            export DTO_USESTDC_CALLS=0
            export DTO_COLLECT_STATS=1
            export DTO_WAIT_METHOD=yield
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

4. Sample histogram given below (generated using DTO_COLLECT_STATS=1)
	i. Numbers under columns set, cpy, mov, and cmp show number of API calls or per-API completion latency for memset, memcpy, memmove, and memcmp respectively.
	ii. Numbers in bytes column show total bytes processed accross all 4 API calls

------------------------------------------------------------------------------------------------------------------------------------------

DTO Run Time: 22826 ms

******** Number of Memory Operations ********
                     <*************** stdc calls    ***************> <*************** dsa (success) ***************> <*************** dsa (failed)  ***************> <***** failure reason *****>
Byte Range        -- set      cpy      mov      cmp      bytes        set      cpy      mov      cmp      bytes        set      cpy      mov      cmp      bytes        Retries PFs    Others
       0-4095     -- 24616626 12319310 7676731  244777209 4531255821  0        0        0        0        0            0        0        0        0        0            0      0      0
    4096-8191     -- 3        290      14       0        1842507      0        0        0        0        0            0        0        0        0        0            0      0      0
    8192-12287    -- 0        2        6        0        45579        64       176      2        0        2545968      0        2        6        0        25276        0      8      0
   12288-16383    -- 1        1        0        0        20756        1        137      0        0        1965343      1        1        0        0        11172        0      2      0
   16384-20479    -- 0        1        3        0        41974        0        126      2        0        2328434      0        1        3        0        24337        0      4      0
   32768-36863    -- 0        0        0        0        0            0        67       2        0        2378165      0        0        0        0        0            0      0      0
   53248-57343    -- 0        0        0        0        0            0        46       0        0        2545890      0        0        0        0        0            0      0      0
   98304-102399   -- 0        0        0        0        0            0        13       0        0        1309627      0        0        0        0        0            0      0      0
  106496-110591   -- 0        1        0        0        31637        0        8        0        0        869880       0        1        0        0        77586        0      1      0
  118784-122879   -- 0        0        0        0        0            0        9        0        0        1083519      0        0        0        0        0            0      0      0
  131072-135167   -- 0        0        2        0        175638       0        5        0        0        665475       0        0        2        0        86506        0      2      0
  143360-147455   -- 0        0        0        0        0            0        1        0        0        143399       0        0        0        0        0            0      0      0
  147456-151551   -- 0        0        0        0        0            0        1        0        0        148647       0        0        0        0        0            0      0      0
  159744-163839   -- 0        0        0        0        0            0        6        0        0        970186       0        0        0        0        0            0      0      0
  212992-217087   -- 0        0        11       0        1580248      0        4        2        0        1292252      0        0        11       0        779384       0      11     0
  278528-282623   -- 0        0        0        0        0            0        1        0        0        279719       0        0        0        0        0            0      0      0
  466944-471039   -- 0        0        0        0        0            0        1        0        0        467783       0        0        0        0        0            0      0      0
  528384-532479   -- 0        0        0        0        0            0        1        0        0        529927       0        0        0        0        0            0      0      0
  720896-724991   -- 0        0        0        0        0            1        0        0        0        720896       0        0        0        0        0            0      0      0
  999424-1003519  -- 0        0        0        0        0            0        1        0        0        1002439      0        0        0        0        0            0      0      0
   >=2093056      -- 0        1        0        0        1975911      0        0        0        0        0            0        1        0        0        973209       0      1      0

******** Average Memory Operation Latency (us)  ********
                     <******** stdc calls    ********> <******** dsa (success) ********> <******** dsa (failed)  ********>
Byte Range        -- set      cpy      mov      cmp      set      cpy      mov      cmp      set      cpy      mov      cmp
       0-4095     -- 0.01     0.02     0.01     0.04     0        0        0        0        0        0        0        0
    4096-8191     -- 0.07     0.42     0.47     0        0        0        0        0        0        0        0        0
    8192-12287    -- 0        1.29     0.47     0        0.84     1.19     1.82     0        0        56.18    2.54     0
   12288-16383    -- 2.90     3.34     0        0        7.49     1.26     0        0        2.07     1.93     0        0
   16384-20479    -- 0        0.27     3.24     0        0        1.37     1.86     0        0        84.92    2.47     0
   32768-36863    -- 0        0        0        0        0        1.87     2.91     0        0        0        0        0
   53248-57343    -- 0        0        0        0        0        2.45     0        0        0        0        0        0
   98304-102399   -- 0        0        0        0        0        3.76     0        0        0        0        0        0
  106496-110591   -- 0        229.18   0        0        0        4.19     0        0        0        3.65     0        0
  118784-122879   -- 0        0        0        0        0        5.18     0        0        0        0        0        0
  131072-135167   -- 0        0        25.00    0        0        5.92     0        0        0        0        14.39    0
  143360-147455   -- 0        0        0        0        0        5.11     0        0        0        0        0        0
  147456-151551   -- 0        0        0        0        0        6.16     0        0        0        0        0        0
  159744-163839   -- 0        0        0        0        0        7.65     0        0        0        0        0        0
  212992-217087   -- 0        0        36.10    0        0        8.15     6.59     0        0        0        18.07    0
  278528-282623   -- 0        0        0        0        0        15.47    0        0        0        0        0        0
  466944-471039   -- 0        0        0        0        0        15.91    0        0        0        0        0        0
  528384-532479   -- 0        0        0        0        0        18.65    0        0        0        0        0        0
  720896-724991   -- 0        0        0        0        17.71    0        0        0        0        0        0        0
  999424-1003519  -- 0        0        0        0        0        28.42    0        0        0        0        0        0
   >=2093056      -- 0        422.15   0        0        0        0        0        0        0        272.87   0        0

