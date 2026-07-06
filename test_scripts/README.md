# QAT replay test guide

This guide describes how to install and configure the Intel QAT driver, build
veSAL, and run the `data_replay` QAT replay test.

## 1. Download QAT driver

Download `QAT20.L.1.2.30-00090.tar.gz` from:

[Intel® QuickAssist Technology (Intel® QAT) Driver for Linux* for Hardware Version 2.0](https://www.intel.com/content/www/us/en/download/765501/852759/intel-quickassist-technology-intel-qat-driver-for-linux-for-hardware-version-2-0.html)

Extract the package:

```bash
tar -xvf QAT20.L.1.2.30-00090.tar.gz
cd QAT20.L.1.2.30-00090
```

## 2. Install QAT build dependencies

Install required packages:

```bash
apt-get update
apt-get install -y libsystemd-dev
apt-get install -y libudev-dev
apt-get install -y libreadline6-dev
apt-get install -y pkg-config
apt-get install -y libxml2-dev
apt-get install -y libpci-dev
apt-get install -y libboost-all-dev
apt-get install -y libelf-dev
# apt-get install -y linux-headers-$(uname -r)
apt-get install -y build-essential
apt-get install -y yasm
apt-get install -y zlib1g-dev
apt-get install -y libssl-dev
apt-get install -y libnl-3-dev libnl-genl-3-dev
apt-get install -y gcc-12
apt-get install -y nasm
```

## 3. Build and install QAT driver

Configure QAT driver:

```bash
./configure --enable-icp-sriov=host \
  --enable-icp-dc-only \
  --enable-icp-without-qp-submission_lock
```

Build and install:

```bash
make -j install
```

## 4. Configure QAT section

Create QAT section `SSL0`.

Recommended test configuration:

```text
section: SSL0
QAT devices: 4
VF channel count: 1 channel per VF
```

The replay scripts use `--vesal_codec_qat_section_name=SSL0` unless otherwise
edited.

## 5. Clone veSAL source code

Clone the repository with submodules:

```bash
git clone --recursive https://github.com/hanqf-git/vesal-release-v1.3.0.git
```

Enter the source directory:

```bash
cd vesal-release-v1.3.0
```

## 6. Build veSAL and replay program

Build all programs:

```bash
./build.sh
```

This builds the replay test program from:

```text
perf/codec/data_replay.cc
```

After build, `build.sh` copies the executable to the repository root:

```text
./data_replay
```

## 7. Prepare test environment

Enter the test script directory:

```bash
cd test_scripts
```

Run preset script to set CPU performance governor and configure hugepages:

```bash
bash preset.sh
```

## 8. Start replay test

Enter the case directory:

```bash
cd case
```

Start the `bd_log0630` replay test:

```bash
bash replay_0630.sh 60000
```

The numeric argument is passed to `data_replay --loop_num` and controls how many
times the loaded replay data is replayed.

## 9. Monitor QAT utilization

During the replay test, open another terminal, enter the `test_scripts` directory,
and start the QAT monitor:

```bash
cd vesal-release-v1.3.0/test_scripts
bash start_qat_monitor.sh
```

Use the monitor output to observe QAT utilization while the replay test is
running.