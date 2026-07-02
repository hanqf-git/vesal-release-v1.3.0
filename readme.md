veSAL(Volcano Engine Storage Acceleration library) 定位是对火山存储业务中高频、高负载的操作提供加速优化库，如压缩/解压缩、加解密、CRC校验、memmove等。
veSAL目前包含三大组件：
- Codec：压缩/解压缩模块，支持lz4、zstd、deflate、zlib算法卸载
- Cypher：加解密模块，支持AES-XTS和Sha256两种算法卸载
- DataFlow：数据流模块，支持CRC和MemMove卸载

未来将加入更多能力支持，如Erasure Coding等能力。

目前veSAL已在火山引擎高性能存储中集成使用，在节省了大量CPU资源的情况下，获得了显著的性能提升。

### Prerequisites
#### 环境要求
理论上不要求特定的OS版本，但是建议使用>=5.4版本的debian系统，我们在该环境做了大量验证。另外，由于需要与硬件交互的能力，需要打开iommu的能力：
```bash
# cat /proc/cmdline
iommu=pt intel_iommu=on,sm_on # 请确保这些参数已打开。
```
且veSAL依赖大页，请确保1GB或2MB大页可用。

#### Codec & Cypher
压缩/解压缩，加密/解密/哈希在存储场景是一个非常常见的操作逻辑，耗费大量的CPU资源。vesal提出基于QAT进行压缩/解压缩/加密/解密/哈希的卸载方案，并支持对压缩前/后的数据随路计算CRC，大大节省业务的CPU资源。
确认您的机型是否支持QAT：
```bash
# lspci -nn | egrep -e '8086:37c8|8086:19e2|8086:0435|8086:6f54|8086:4940|8086:4942'
76:00.0 Co-processor [0b40]: Intel Corporation Device [8086:4942] (rev 40)
7a:00.0 Co-processor [0b40]: Intel Corporation Device [8086:4942] (rev 40)
f3:00.0 Co-processor [0b40]: Intel Corporation Device [8086:4942] (rev 40)
f7:00.0 Co-processor [0b40]: Intel Corporation Device [8086:4942] (rev 40)
```
QAT需要对应的kernel module，推荐直接使用最新版的QAT driver。https://www.intel.com/content/www/us/en/download/765501/intel-quickassist-technology-driver-for-linux-hw-version-2-0.html 按照Intel官方方式完成安装即可。安装完成后，需要确保用户态库libqat_s.so和usdm_drv.so可以被找到（在LD_LIBRARY_PATH中）。


#### DataFlow
Intel DSA硬件支持对CRC和Memcpy等常见的操作逻辑进行卸载，vesal基于DSA完成CRC和Memcpy卸载支持。
确认您的机型是否支持DSA：
```bash
# lspci | grep 0b25
75:01.0 System peripheral: Intel Corporation Device 0b25
f2:01.0 System peripheral: Intel Corporation Device 0b25
```
DSA不需要专门的kernel module，但是需要对硬件资源做一次初始化。该步骤需要先执行编译，请看后文。

### 编译方式
首先安装以下通用依赖：
```bash
cmake autoconf valgrind libnuma-dev libunwind-dev
```
之后执行：
```bash
./build.sh
```
该命令会下载第三方库依赖并执行编译，具体可见`get-deps.py`脚本
```python
REPOS = {
    "https://github.com/gflags/gflags.git": "v2.2.2",
    "https://github.com/google/googletest.git": "release-1.11.0",
    "https://github.com/gperftools/gperftools.git": "gperftools-2.12",
    "https://github.com/ebiggers/libdeflate.git": "v1.23",
    "https://github.com/lz4/lz4.git": "v1.9.3",
    "https://github.com/gabime/spdlog.git": "v1.8.2",
    "https://github.com/facebook/zstd.git": "v1.5.5",
    "https://github.com/madler/zlib.git": "v1.3.1",
    "https://github.com/openssl/openssl.git": "OpenSSL_1_0_1-stable",
    "https://github.com/skarupke/flat_hash_map.git": "master"
}
```
编译产物将存放于build目录下。

### 项目集成
```cmake
add_executable(foo foo.cc)

set(VESAL_ENABLE_ZSTD ON) # zstd is optional, set to false if zstd if not used. Default is false
add_subdirectory(vesal)

target_link_libraries(foo PRIVATE vesal)
```

### 运行
注意，由于需要基于uio与硬件进行交互，且需要对内存地址进行计算，*运行vesal所有程序需要root权限*。
#### Codec & Cypher
* 确保上面确认的QAT设备正常工作
* 执行`./perf_simple`，会使用QAT压缩和解压perf目录下的calgary数据集。注意默认的QAT section name为"SSL0"，可以使用gflags参数修改，如：--vesal_codec_qat_section_name="SSL"。详细的参数可见include/vesal/vesal.h

#### DataFlow
* 确保上面确认的DSA设备正常工作
* DSA不需要专门的kernel module，但是需要对硬件资源做一次初始化。在编译完成之后，首先执行`tools/pci_bind.sh`，再`./prepare_dsa_env`运行编译产物中的准备程序。该准备只需执行一次。
* 执行位于build文件下的demo `vesal_data_flow_example`。

### License
vesal基于GPLv3协议开源，您可以在遵守协议的前提下自由使用、修改和分发vesal。
