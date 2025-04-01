# Intel DSA Usage with CacheLib

## Platform Configuration

### BIOS Configuration
- Enable **Intel Virtualization Technology for Directed I/O (VT-d)**.

### Linux Kernel Configuration
Ensure the following kernel options are enabled:
```plaintext
CONFIG_INTEL_IOMMU=y
CONFIG_INTEL_IOMMU_SVM=y
CONFIG_INTEL_IOMMU_DEFAULT_ON=y
CONFIG_INTEL_IOMMU_SCALABLE_MODE_DEFAULT_ON=y
```
If either `CONFIG_INTEL_IOMMU_DEFAULT_ON` or `CONFIG_INTEL_IOMMU_SCALABLE_MODE_DEFAULT_ON` is not enabled, add the following to the kernel boot parameters:
```plaintext
intel_iommu=on,sm_on
```

### Intel DSA Driver (IDXD)
Enable the following kernel options while building or installing the Linux kernel (version 5.18 or later is needed for shared work queues used by DTO):
```plaintext
CONFIG_INTEL_IDXD=m
CONFIG_INTEL_IDXD_SVM=y
CONFIG_INTEL_IDXD_PERFMON=y
```

### Work Queues (WQs)
- WQs are on-device queues that contain descriptors submitted to the device.
- They can be configured in two modes:
  - **Dedicated (DWQ)**
  - **Shared (SWQ)**
- **SWQ is preferred** as it allows multiple clients to submit descriptors simultaneously, improving device utilization compared to DWQs.
- **DTO uses SWQ**.

### Verify IDXD Driver Initialization
Use the following command to check the kernel message buffer:
```sh
dmesg | grep "idxd "
```

## Initialize the DSA Device

### List All DSA Devices
```sh
lspci | grep 0b25
```
### Initialize DSA device(s)


#### Accel Config Script
```sh
./accelConfig.sh <DSA device number (0, 1, 2, 3 ...)> <yes/no (enabled/disabled)> <work queue id> <engine_count>
```
To enable the first DSA device (assuming it is device 0) with a shared work queue and 4 engines, you would run:
```sh
sudo ./accelConfig.sh 0 yes 0 4
```
#### DSA Device Permissions
To allow userpace applications to submit work to the DSA, you need to give the appropriate permissions, 
for example to give all users the ability to submit work to device 0 work queue:
```sh
sudo chmod 0777 /dev/dsa/wq0.0
```
Of course, regular Unix permissions can be used to control access to the device.

#### Accel config
You can also setup the device using the accel-config tool with a configuration file:
```sh
sudo accel-config -c <config_file>
```

## Using the DTO library

### Pre-requisite Packages
- **glibc version 2.36 or later** is recommended for best DTO performance.
- Check your glibc version:
```sh
ldd --version
```
- Systems with glibc versions **less than 2.36** may experience reduced DTO performance due to a known bug.
- Centos 10 requires the CRB repo to be enabled for 'accel-config-devel'
#### Package Requirements
- **Fedora/CentOS/RHEL**:
```sh
sudo dnf config-manager --set-enabled crb
sudo dnf install -y \
  kernel-headers \
  accel-config-devel \
  accel-config \
  numactl-devel \
  libuuid-devel
```
- **Ubuntu/Debian**:
```sh
sudo apt-get install -y \
  linux-libc-dev \
  libaccel-config-dev \
  uuid-dev \
  numactl-dev
```

### Build DTO Library
```sh
make libdto
sudo make install
```

### Build the Test Application With Stats Output
```sh
make dto-test
source dto_env.sh
./dto-test
```

## Using with CacheLib

To connect to CacheLib (OSS version) using the DTO library, you will need to ensure that your application is linked against the `libdto` library.

### Linking to CacheLib with build patch

Cachebench needs a change for setStringItem to use 'std::memmove' instead of 'strncpy' as DTO does not intercept strncpy call. The patch also adds the build option for DTO in cmake.

```sh
cd ~/CacheLib/
git apply ~/DTO/cachelib.patch
cd build-cachelib/
cmake .. \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_WITH_DTO=ON
make -j$(nproc)
make install
```
### Example Usage For Complete Offload

You can set the following environment variables to use the DTO library with CacheLib:

```plaintext
DTO_USESTDC_CALLS=0 // Set to 1 to use standard C library calls (like memcpy) instead of DSA enabled calls
DTO_COLLECT_STATS=0 // Set to 1 to collect stats on DSA performance
DTO_WAIT_METHOD=sleep // sleep, umwait, tpause, yield, or busypoll after submitting to DSA
DTO_MIN_BYTES=32768 // Minimum bytes for DSA to process, smaller requests will be done on CPU to avoid latency increase
DTO_CPU_SIZE_FRACTION=0.0 //offload the entire call to DSA
DTO_AUTO_ADJUST_KNOBS=0 //Tries to find the optimal split between CPU fraction and DSA fraction, attempting to minimize the difference in time between the two
DTO_DSA_CC=0 // Set to 0 to bypass CPU cache for the destination buffer, this will ensure that the data is written from DSA directly to memory avoidng polluting the cache with a large buffer that has low probablity of being reaccessed.
```

```sh
source ~/DTO/dto_cachelib_env.sh
./opt/cachelib/bin/cachebench --json_test_config ~/DTO/cachebench_config.json
```

### Example Usage For Partial Offload

Set the following enviroment variables to use the DTO library with CacheLib:
```plaintext
DTO_USESTDC_CALLS=0 // Set to 1 to use standard C library calls (like memcpy) instead of DSA enabled calls
DTO_COLLECT_STATS=0 // Set to 1 to collect stats on DSA performance
DTO_WAIT_METHOD=tpause // or umait
DTO_MIN_BYTES=32768 // Minimum bytes for DSA to process, smaller requests will be done on CPU
DTO_CPU_SIZE_FRACTION=0.0 //offload the entire call to DSA
DTO_AUTO_ADJUST_KNOBS=2 //Tries to find the optimal split between CPU fraction and DSA fraction, attempting to minimize the difference in time between the two
DTO_DSA_CC=0 // Set to 0 to bypass CPU cache for the destination buffer, this will ensure that the data is written from DSA directly to memory avoidng polluting the cache with a large buffer that has low probablity of being reaccessed.
```
```sh
./opt/cachelib/bin/cachebench --json_test_config ~/DTO/cachebench_config.json
```

## A note on the wait methods

The wait method is used to wait for the DSA to complete the operation. The following methods are supported:
- **sleep**: The thread will sleep for a fixed amount of time.
- **umwait**: The thread will use the umwait instruction to wait for the DSA to complete the operation. Umwait
monitors the address of the DSA descriptor and waits for the DSA to complete the operation.
- **tpause**: The thread will use the tpause instruction to wait for the DSA to complete the operation.
- **yield**: The thread will yield the CPU to the OS.
- **busypoll**: The thread will busy poll the DSA to check if the operation is complete. It uses the 
regular mm_pause instruction to send noops to the core while waiting.

Both umwait and tpause are recommended as they are more efficient than sleep and yield since they do not
cause a context switch. In addition, umwait and tpause are more efficient than busypoll as the allow
the CPU to enter C0.1 or C0.2 states which are low power states, C0.2 has ~0.5us exit latency while C0.1
has ~0.25us exit latency. Therefore, C0.1 is used when autotuning is enabled to do a portion of the work
on the CPU and the remainder on the core as our goal is to minimize the time gap between the two. If we 
are doing a complete offload we enter C0.2.


### Additional References
- [Data Streaming Accelerator User Guide](https://www.intel.com/content/www/us/en/content-details/759709/intel-data-streaming-accelerator-user-guide.html)
- [Data Streaming Accelerator Architecture Specification](https://cdrdv2-public.intel.com/671116/341204-intel-data-streaming-accelerator-spec.pdf)


