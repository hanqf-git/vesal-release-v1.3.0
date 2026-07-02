# Copyright (c) 2023 ByteDance Inc.
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

import pathlib
import argparse
import itertools
import subprocess
import csv
import os
import json
import pickle
import pandas as pd
from typing import List, Dict, Any, Tuple

ALL = "all"
ENGINES = ["qat", "sw"]
ALGOS = ["aes", "sha256"]
ENGINE_TYPE="engine"
ALGO_TYPE="algo"
CYPHER_OP="cypher_op"
RESUME_FILE="perf_progress.pickle"

class PerformanceTester:
    def __init__(self, test_program: str, data_file: str, output_csv: str, resume_file: str, total_data: int):
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
        self.total_data = total_data
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
        with open(self.resume_file, 'rb') as f:
            t = pickle.load(f)
            data, self.results = t
            self.completed_params = set(tuple(p) for p in data.get('completed', []))
    
    def _save_resume_state(self):
        """保存当前进度到断点文件"""
        data = [{'completed': [list(p) for p in self.completed_params]}, self.results]
        with open(self.resume_file, 'wb') as f:
            pickle.dump(data, f)
    
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
        for name, value in zip(self.param_names, params):
            cmd_args.extend([f"--{name}", str(value)])
        cmd_args.extend(["--total_data_per_thread", str(self.total_data)])
        cmd_args.extend(["--vesal_metrics_sample_rate", "0"])
        cmd_args.extend(["--vesal_metrics_disable_poller_metrics", "true"])
        
        # 执行测试程序
        try:
            result = subprocess.run(
                [self.test_program] + cmd_args,
                capture_output=True,
                text=True,
                check=True
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
        # 检查是否需要创建新的CSV文件
        new_file = not os.path.exists(self.output_csv)
        
        # 生成所有参数组合
        all_combinations = itertools.product(*self.param_values)
            
        for params in all_combinations:
            # 检查是否已完成此参数组合
            if params in self.completed_params:
                continue
            
            print(f"测试参数组合: {dict(zip(self.param_names, params))}")
            
            # 运行测试
            self._run_test(params)
            
            # 标记此参数组合已完成
            self.completed_params.add(params)
            
            # 保存进度
            self._save_resume_state()

        self.output()

    def output(self):
        for engine in self.params[ENGINE_TYPE] :
            for op in self.results[CYPHER_OP] :
                mask = pd.Series(True, index = self.results.index)
                mask &= self.results[ENGINE_TYPE] == engine
                mask &= self.results[CYPHER_OP] == op
                self.results[mask].drop(columns=[ENGINE_TYPE, CYPHER_OP]).to_csv("{}_{}_{}.csv".format(self.output_csv, engine, op), index = False)


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
        "--restart",
        type=bool,
        default=False
    )
    g_parser.add_argument(
        "--total_data",
        type=int,
        default=10
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
        pathlib.Path(__file__) / "../../build/perf/cypher/cy_perf"
    ).resolve()

    tester = PerformanceTester(
        test_program=perf_simple_path,
        data_file="cy_perf_results.csv",
        output_csv="perf_summary",
        resume_file=RESUME_FILE,
        total_data=args.total_data
    )
 
    engines = ENGINES
    if args.engine != ALL:
        engines = [args.engine]

    algos = ALGOS
    if args.algo != ALL:
        algos = [args.algo]
    
    # 设置参数及其可能的值
    tester.set_parameters({
        ENGINE_TYPE: engines,
        ALGO_TYPE: algos,
        "channel_num": [1, 2, 3, 4],
        "source_size": [2 ** i for i in range(12, 21)],
        "inflight_num": [1, 4, 16, 64, 128, 256, 500],
    })
    
    # 运行所有测试
    tester.run_all_tests()


if __name__ == "__main__":
    main()