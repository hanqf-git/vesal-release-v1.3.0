# Copyright (c) 2025 ByteDance Inc.
#
# This file is part of veSAL.
#
# veSAL is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# veSAL is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with veSAL. If not, see <https://www.gnu.org/licenses/>.

# This script is used to run perf_simple for lz4 release test.

import datetime
import pathlib
import argparse
import itertools
import subprocess
import os
import pickle
import pandas as pd
from typing import List, Dict, Any, Tuple

ALL = "all"
ENGINES = ["qat", "sw"]
ALGOS = ["lz4", "deflate", "zlib"]
CRC32 = ["true", "false"]
DC_OP = ["comp", "decomp"]
COMP_LEVEL_OPTIONS = ["1", "6", "9"]
MEM_TYPES = ["register_2mb", "user"]
INFLIGHT_NUMS = ["1", "64", "256"]
CHANNEL_NUMS = ["1", "2", "3", "4"]
SGL_SIZE = ["1", "2", "4", "8", "16"]

ENABLE_CRC32 = "enable_crc32"
ENGINE_TYPE = "engine_type"
ALGO_TYPE = "algorithm_type"
DC_OP_TYPE = "comp/decomp"
COMP_LEVEL = "compression_level"
MEMORY_TYPE = "memory_type"
INFLIGHT_NUM_KEY = "inflight_num"
CHANNEL_NUM_KEY = "channel_num"
SGL_SIZE_KEY = "sgl_size"

RESUME_FILE = "dc_perf_progress.pickle"


class PerformanceTester:
    def __init__(
        self,
        test_program: str,
        data_file: str,
        output_csv: str,
        resume_file: str,
        base_total_data: int,
    ):
        """
        初始化性能测试器

        Args:
            test_program: 性能测试程序路径
            data_file: 性能测试程序结果存放文件
            output_csv: 输出的CSV文件路径
            resume_file: 断点续传记录文件路径
        """
        self.test_program = test_program
        self.data_file = data_file
        self.output_csv = output_csv
        self.resume_file = resume_file
        # total_data is only base here, might multiple later.
        self.base_total_data = base_total_data
        self.completed_params = set()
        self.params = {}
        self.param_names = []
        self.param_values = []
        self.results = pd.DataFrame()

        # 检查是否有断点记录
        if os.path.exists(resume_file):
            self._load_resume_state()

    def set_parameters(self, parameters: Dict[str, List[Any]]):
        """
        设置测试参数

        Args:
            parameters: 参数字典，格式为 {参数名: [可能的值列表]}
        """
        self.params = parameters
        self.param_names = list(parameters.keys())
        self.param_values = [parameters[name] for name in self.param_names]

    def _load_resume_state(self):
        """从断点文件加载已完成的参数组合"""
        with open(self.resume_file, "rb") as f:
            data, self.results = pickle.load(f)
            self.completed_params = set(tuple(p) for p in data.get("completed", []))

    def _save_resume_state(self):
        """保存当前进度到断点文件"""
        data = [{"completed": [list(p) for p in self.completed_params]}, self.results]
        with open(self.resume_file, "wb") as f:
            pickle.dump(data, f)

    def _need_test(self, source_size: int, sgl_size: int, memory_type: str) -> bool:
        # calgary file size, we cannot let each chunk exceeds this.
        return (
            source_size * sgl_size <= 1024 * 1024
            if memory_type == "register_2mb"
            else source_size * sgl_size <= 256 * 1024
        )

    def _run_test(self, params: Tuple[Any, ...]):
        """
        使用指定参数运行测试程序并收集结果

        Args:
            params: 参数元组，按self.param_names的顺序排列

        Returns:
            包含性能指标的字典
        """
        # 构建命令行参数
        cmd_args = []
        channel_num = 1
        inflight_num = 1
        souce_size = 1
        sgl_size = 1
        memory_type = "user"
        engine_type = "qat"
        for name, value in zip(self.param_names, params):
            if name == "comp/decomp":
                if value == "comp":
                    cmd_args.extend(["--compression_perf=true"])
                    cmd_args.extend(["--decompression_perf=false"])
                else:
                    cmd_args.extend(["--compression_perf=false"])
                    cmd_args.extend(["--decompression_perf=true"])
                continue
            if name == "enable_crc32":
                cmd_args.extend(["--enable_crc32=" + value])
                continue
            cmd_args.extend([f"--{name}", str(value)])
            if name == "channel_num":
                channel_num = int(value)
            if name == "inflight_num":
                inflight_num = int(value)
            if name == "source_size":
                souce_size = int(value)
            if name == "sgl_size":
                sgl_size = int(value)
            if name == "memory_type":
                memory_type = value
            if name == ENGINE_TYPE:
                engine_type = value
        # total_data might be multiple of base_total_data, depending on channel_num
        total_data = self.base_total_data * channel_num
        if inflight_num > 16:
            total_data *= 2
            if souce_size > 65536:
                total_data *= 2
        if memory_type != "register_2mb":
            total_data //= 2
        if inflight_num == 1:
            total_data //= 2
        if engine_type == "sw":
            total_data //= 3
            if total_data < 1:
                total_data = 1
        cmd_args.extend(["--total_data", str(total_data)])
        cmd_args.extend(["--vesal_metrics_sample_rate", "0"])
        cmd_args.extend(["--vesal_metrics_disable_poller_metrics=true"])

        if not self._need_test(souce_size, sgl_size, memory_type):
            print("skip: ", cmd_args)
            return
        print("Run: ", cmd_args)
        # 执行测试程序
        try:
            result = subprocess.run(
                [self.test_program] + cmd_args,
                capture_output=True,
                text=True,
                check=True,
            )

            # 解析CSV输出（假设测试程序将结果输出到CSV文件）
            df = pd.read_csv(self.data_file)
            print(df)
            self.results = pd.concat([self.results, df], ignore_index=True)

        except subprocess.CalledProcessError as e:
            print(f"测试失败: {e.stderr}")
            return {"status": "failed", "error": e.stderr}

    def run_all_tests(self):
        """执行所有参数组合的测试"""

        # 生成所有参数组合
        all_combinations = itertools.product(*self.param_values)
        total_num = 1
        for values in self.param_values:
            total_num *= len(values)
        done = 0
        for params in all_combinations:
            print(
                f"done: {done}/{total_num}: now time: " + str(datetime.datetime.now())
            )
            next_one = dict(zip(self.param_names, params))
            print(f"测试参数组合: {next_one}")
            # 检查是否已完成此参数组合
            if params in self.completed_params:
                print("已完成此参数组合，跳过")
                done += 1
                continue

            # 运行测试
            self._run_test(params)

            # 标记此参数组合已完成
            self.completed_params.add(params)

            # 保存进度
            self._save_resume_state()
            done += 1

        self.output()

    def output(self):
        print("Start output...")
        self.results.to_csv(f"{self.output_csv}.csv", index=False)


g_parser = argparse.ArgumentParser()


def ParseArgs():
    g_parser.add_argument(
        "--engine",
        type=str,
        choices=[ALL] + ENGINES,
        default=ALL,
    )
    g_parser.add_argument(
        "--algo",
        type=str,
        choices=[ALL] + ALGOS,
        default=ALL,
    )
    g_parser.add_argument(
        "--comp_level", type=str, choices=[ALL] + COMP_LEVEL_OPTIONS, default=ALL
    )
    g_parser.add_argument(
        "--mem_mode", type=str, choices=[ALL] + MEM_TYPES, default=ALL
    )
    g_parser.add_argument(
        "--channel_num", type=str, choices=[ALL] + CHANNEL_NUMS, default=ALL
    )
    g_parser.add_argument(
        "--inflight_num", type=str, choices=[ALL] + INFLIGHT_NUMS, default=ALL
    )
    g_parser.add_argument("--sgl_size", type=str, choices=[ALL] + SGL_SIZE, default=ALL)
    g_parser.add_argument("--op", type=str, choices=[ALL] + DC_OP, default="all")
    g_parser.add_argument("--restart", action="store_true")
    g_parser.add_argument("--base_total_data", type=int, default=4)
    g_parser.add_argument(
        "--crc32", type=str, choices=["true", "false", ALL], default=ALL
    )
    return g_parser.parse_args()


def main():
    args = ParseArgs()
    print("Script args: ", args)

    if args.restart:
        print("set to restart, delete resume file...")
        if os.path.exists(RESUME_FILE):
            os.remove(RESUME_FILE)
        print("resume file deleted, restart the whole perf")

    perf_simple_path = (
        pathlib.Path(__file__) / "../../build/perf/codec/perf_simple"
    ).resolve()

    tester = PerformanceTester(
        test_program=perf_simple_path,
        data_file="perf_simple_results.csv",
        output_csv="dc_perf_summary",
        resume_file=RESUME_FILE,
        base_total_data=args.base_total_data,
    )

    # 设置参数及其可能的值
    tester.set_parameters(
        {
            ENGINE_TYPE: [args.engine] if args.engine != ALL else ENGINES,
            ALGO_TYPE: [args.algo] if args.algo != ALL else ALGOS,
            ENABLE_CRC32: [args.crc32] if args.crc32 != ALL else CRC32,
            CHANNEL_NUM_KEY: (
                [args.channel_num] if args.channel_num != ALL else CHANNEL_NUMS
            ),
            "source_size": [2**i for i in range(12, 21)],
            INFLIGHT_NUM_KEY: (
                [args.inflight_num] if args.inflight_num != ALL else INFLIGHT_NUMS
            ),
            SGL_SIZE_KEY: ([args.sgl_size] if args.sgl_size != ALL else SGL_SIZE),
            DC_OP_TYPE: [args.op] if args.op != ALL else DC_OP,
            COMP_LEVEL: (
                [args.comp_level] if args.comp_level != ALL else COMP_LEVEL_OPTIONS
            ),
            MEMORY_TYPE: [args.mem_mode] if args.mem_mode != ALL else MEM_TYPES,
        }
    )

    # 运行所有测试
    tester.run_all_tests()


if __name__ == "__main__":
    main()
