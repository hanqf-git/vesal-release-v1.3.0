from pathlib import Path
import logging
import os
import queue
import subprocess
import threading
import time

"""
The script will:
1. Build all suites and store them in a folder named "build_{suite}"
2. While building each suite, run its UT. Also generate gcov, do format and tidy check if needed, etc.
3. Run perf of each suite, parallelly.
"""

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - [%(threadName)s] - %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)

BASE_WORKDIR = Path(os.getcwd())

BUILD_SUITES = ["release", "debug_gcov_err_sim", "release_asan", "debug"]
# Perf args
SW_TOTAL_DATA_PER_THREAD = 1
QAT_TOTAL_DATA_PER_THREAD = 10
SOURCE_SIZE = 4096
INFLIGHT_NUM = 64
ENABLE_CRC32 = True
CHANNEL_NUM = 4
MEMORY_TYPE = "register_2mb"

# 10 min
SUBPROCESS_TIMEOUT = 600

TOTAL_CORE_NUM = os.cpu_count()
g_next_core = 0

g_start_timestamp = 0
g_end_timestamp = 0

g_err_q = queue.Queue()


def get_time_cost():
    global g_start_timestamp, g_end_timestamp
    g_end_timestamp = time.time()
    return g_end_timestamp - g_start_timestamp


def gen_build_cmd(suite):
    ret = "./build.sh"
    assert suite in BUILD_SUITES, f"Invalid build suite: {suite}"
    if suite == "release":
        ret += " --release"
    elif suite == "release_asan":
        ret += " --release --asan"
    elif suite == "debug":
        ret += " --debug"
    elif suite == "debug_gcov_err_sim":
        ret += " --debug --gcov --enable_err_sim"
    else:
        raise ValueError(f"Invalid build suite: {suite}")
    return ret


class CmdResult:
    def __init__(self, retcode, stdout, stderr, cmd):
        self.ret = retcode
        self.stdout = stdout
        self.stderr = stderr
        self.cmd = cmd

    def __str__(self):
        return f"ret: {self.ret}, stdout: {self.stdout}, stderr: {self.stderr}, cmd: {self.cmd}"


def run_cmd(cmd):
    global g_start_timestamp, g_end_timestamp
    logging.info(f"run cmd: {cmd}")
    try:
        subprocess.run(
            cmd,
            shell=True,
            text=True,
            check=True,
            capture_output=True,
            timeout=SUBPROCESS_TIMEOUT,
        )
    except Exception as e:
        g_err_q.put(CmdResult(e.returncode, e.stdout, e.stderr, cmd))


def build_and_store(suite: str):
    build_cmd = gen_build_cmd(suite)
    target_file_name = f"build_{suite}"
    run_cmd(build_cmd)
    run_cmd(f"rm -rf {target_file_name} && cp -r build {target_file_name}")
    return target_file_name


def gen_perf_cmd(engine_type="qat", core=0):
    perf_cmd = f"taskset -c {core} build/perf/codec/perf_simple"
    perf_cmd += f" --engine_type={engine_type}"
    if engine_type == "qat":
        perf_cmd += f" --total_data={QAT_TOTAL_DATA_PER_THREAD}"
        perf_cmd += f" --channel_num={CHANNEL_NUM}"
        perf_cmd += " --memory_type=" + MEMORY_TYPE
    else:
        perf_cmd += f" --total_data={SW_TOTAL_DATA_PER_THREAD}"
    perf_cmd += f" --source_size={SOURCE_SIZE}"
    perf_cmd += f" --inflight_num={INFLIGHT_NUM}"
    if ENABLE_CRC32:
        perf_cmd += " --enable_crc32=true"
    return perf_cmd


def run_perf(artifact_path, engine_type="qat", core=0):
    run_cmd(f"cd {artifact_path}")
    perf_cmd = gen_perf_cmd(engine_type, core)
    run_cmd(perf_cmd)


def run_ut():
    run_cmd("bash run_test.sh")


def async_run_with_new_core(target, args: list):
    global g_next_core
    core = g_next_core
    g_next_core += 1
    g_next_core %= TOTAL_CORE_NUM
    args.append(core)
    return threading.Thread(target=target, args=args)


def clang_tidy(path):
    run_cmd(f"bash check-clang-tidy.sh {path}")


def clang_format():
    run_cmd(
        r'clang-format -style=file -i $(find ./src ./test ./include -type f \( -name "*.cc" -or -name "*.h" \))'
    )
    run_cmd(
        'git diff --quiet || (echo "Please run \'clang-format -style=file -i \$(find ./src ./test ./ include -type f \( -name "*.cc" -or -name "*.h" \))\' in root directory of the repo && exit 1")'
    )


def prepare_gcov_info(build_path: str):
    run_cmd(f"lcov -d {build_path} -c -o cov_all.info")
    run_cmd('lcov -e cov_all.info "$(pwd)/src/*" "$(pwd)/include/*" -o vesal_cov.info')
    run_cmd("lcov -r vesal_cov.info '*_test.cc perf_*.cc' -o vesal_final.info")
    assert Path("vesal_final.info").exists(), "generate vesal_final.info failed"


def prepare_dsa_env():
    run_cmd("bash tools/pci_bind.sh")
    # Note this must be after build
    run_cmd(r"build/dsa-uio-config/prepare_dsa_env")


def main():
    global g_start_timestamp
    g_start_timestamp = time.time()
    logging.info(f"Build suites: {BUILD_SUITES}, BASE_WORKDIR: {BASE_WORKDIR}")
    artifact_paths = []
    # Only check format and tidy for release
    for suite in BUILD_SUITES:
        if suite == "release":
            clang_format()
        artifact_path = build_and_store(suite)
        artifact_paths.append(artifact_path)
        if suite == "release":
            clang_tidy(artifact_path)
            prepare_dsa_env()
        logging.info(f"Artifact path: {artifact_path}")
        logging.info(f"Run Unit Test for {suite}:")
        run_ut()
        # Store gcov info for later process. Note though we store the artifact somewhere else, we still run UT at current build dir.
        if suite == "debug_gcov_err_sim":
            prepare_gcov_info("build")
    perf_threads = []
    for artifact_path in artifact_paths:
        logging.info(f"Run Perf for {artifact_path}:")
        qat_thread = async_run_with_new_core(run_perf, [artifact_path, "qat"])
        sw_thread = async_run_with_new_core(run_perf, [artifact_path, "sw"])
        perf_threads.append(qat_thread)
        perf_threads.append(sw_thread)
        qat_thread.start()
        sw_thread.start()
    for thread in perf_threads:
        thread.join()
    failed = False
    if not g_err_q.empty():
        failed = True
    while not g_err_q.empty():
        logging.error(g_err_q.get())
    if failed:
        logging.error(f"Some test failed, time cost: {get_time_cost()}s")
        exit(1)
    logging.info(f"All success, time cost: {get_time_cost()}s")
    return 0


if __name__ == "__main__":
    main()
