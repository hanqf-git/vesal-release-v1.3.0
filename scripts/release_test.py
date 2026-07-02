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

import math
import subprocess
import pathlib
import argparse


inflight_nums = [1, 2, 4, 8, 16, 32, 64, 128]
source_sizes = [
    1024,
    2048,
    4096,
    8192,
    16384,
    32768,
    65536,
    131072,
    262144,
    524288,
    1048576,
]
channel_nums = [1, 2, 3, 4]
sgl_sizes = [1, 2, 4, 8, 16, 32]
# always enable crc32
enable_crc32s = ["true"]
# metrics_prefix is useless, unless built with metrics enabled
metrics_prefix = "release_test"
base_total_data_per_thread = 1  # base 1gb, increase by channel_num and inflight_num and source_size to avoid running too long
compression_levels = [1, 6, 9]

# not exceeds 1MB
kMaxPayLoadSizeKb = 1 * 1024 * 1024
kMinTotalDataGb = 1
kMaxTotalDataGb = 40

perf_simple_path = (
    pathlib.Path(__file__) / "../../build/src/codec/perf/perf_simple"
).resolve()

compress_data_file_name = "vesal_release_test_compress.log"
decompress_data_file_name = "vesal_release_test_decompress.log"


def validate(source_size, sgl_size) -> bool:
    return source_size * sgl_size <= kMaxPayLoadSizeKb


def FileExists(file_path: str) -> bool:
    file = pathlib.Path(file_path)
    return file.exists() and file.is_file()


def produce_log(is_compress: bool, is_copy: bool, cpu_id: int, section_name: str):
    fname = compress_data_file_name if is_compress else decompress_data_file_name
    total_count = (
        len(inflight_nums)
        * len(source_sizes)
        * len(channel_nums)
        * len(sgl_sizes)
        * len(enable_crc32s)
        * len(compression_levels)
    )
    curr_count = 0
    with open(fname, "w") as f:
        for compression_level in compression_levels:
            for inflight_num in inflight_nums:
                for source_size in source_sizes:
                    for channel_num in channel_nums:
                        for sgl_size in sgl_sizes:
                            for enable_crc32 in enable_crc32s:
                                curr_count += 1
                                print(
                                    "Running "
                                    + str(curr_count)
                                    + "/"
                                    + str(total_count)
                                    + " tests..."
                                )
                                total_data_per_thread = int(
                                    base_total_data_per_thread
                                    * channel_num
                                    * math.sqrt(inflight_num)
                                    * math.sqrt(source_size / 1024)
                                )
                                total_data_per_thread = min(
                                    total_data_per_thread, kMaxTotalDataGb
                                )
                                total_data_per_thread = max(
                                    total_data_per_thread, kMinTotalDataGb
                                )
                                args = [
                                    "--inflight_num",
                                    str(inflight_num),
                                    "--source_size",
                                    str(source_size),
                                    "--channel_num",
                                    str(channel_num),
                                    "--sgl_size",
                                    str(sgl_size),
                                    "--enable_crc32",
                                    enable_crc32,
                                    "--metrics_prefix",
                                    metrics_prefix,
                                    "--total_data_per_thread",
                                    str(total_data_per_thread),
                                    "--compression_level",
                                    str(compression_level),
                                ]
                                if is_compress:
                                    args.extend(
                                        [
                                            "--compression_perf",
                                            "true",
                                            "--decompression_perf",
                                            "false",
                                        ]
                                    )
                                else:
                                    args.extend(
                                        [
                                            "--compression_perf",
                                            "false",
                                            "--decompression_perf",
                                            "true",
                                        ]
                                    )
                                memory_type = ""
                                if is_copy:
                                    memory_type = "user"
                                else:
                                    memory_type = "register_2mb"
                                    args.append("true")
                                args.append("--memory_type")
                                args.append(memory_type)

                                if section_name:
                                    args.append("--vesal_codec_qat_section_name")
                                    args.append(section_name)

                                # combine cmd and args
                                cmd_str = str(perf_simple_path) + " "
                                for idx in range(len(args)):
                                    if idx % 2 == 0:
                                        cmd_str += args[idx] + "="
                                    else:
                                        cmd_str += args[idx] + " "
                                # binding cpu
                                cmd_str = "taskset -c " + str(cpu_id) + " " + cmd_str
                                cmd_list = cmd_str.split()
                                if not validate(source_size, sgl_size):
                                    print("Skipping invalid test: ", cmd_str + "...")
                                    continue
                                print("Running cmd: " + cmd_str + "...")
                                process = subprocess.Popen(
                                    cmd_list, stderr=f, stdout=f, shell=False
                                )
                                process.communicate()


g_compress_dict = {}
g_decompress_dict = {}


interested_keys = (
    "channel_num",
    "compression_perf",
    "decompression_perf",
    "enable_crc32",
    "inflight_num",
    "memory_type",
    "sgl_size",
    "source_size",
    "compression_level",
)


def AppendOneKeyIfInterested(line, key_str) -> str:
    start_pos = line.find("--")
    if start_pos == -1:
        return ""
    start_pos += len("--")
    key_name = line[start_pos:].split("=")[0].strip()
    if key_name not in interested_keys:
        return ""
    key_str += line[start_pos:].strip()
    key_str += ","
    return key_str


def ParseHist(line, latency_dict):
    # we assume the first keyword must be 'AVG'
    start_pos = line.find("AVG")
    if start_pos == -1:
        return
    items = line[start_pos:].split(",")
    for each in items:
        k = each.split(":")[0].strip()
        v = each.split(":")[1].strip()
        latency_dict[k] = v


def ParseThroughput(line, throughput_dict):
    # we assume the first keyword must be 'total_timecost'
    start_pos = line.find("total_timecost")
    if start_pos == -1:
        return
    items = line[start_pos:].split(",")
    for each in items:
        k = each.split(":")[0].strip()
        v = each.split(":")[1].strip()
        throughput_dict[k] = v


def CollectData(fname, is_compress):
    if not FileExists(fname):
        print("err:", fname, "not exist")
        return
    with open(fname, "r") as f:
        # read line by line, and parse the log, as key
        key = ""
        value = {}
        available = True
        for line in f:
            if "Get qat instance number" in line:
                # start parse
                available = True
                continue
            if not available:
                continue
            if "critical" in line or "error" in line:
                # invalid case, discard current and turn available to false
                available = False
                key = ""
                value = {}
                continue
            # note these three key words are order-dependent
            if "EchoAllFlags" in line:
                # this might be a key
                new_key = AppendOneKeyIfInterested(line, key)
                if new_key:
                    key = new_key
                continue
            if "compression ratio" in line:
                pos = line.find("compression ratio")
                ratio = line[pos:].split(":")[1].strip()
                value["compression_ratio"] = ratio
                continue
            if "EchoHistogram" in line:
                # this is a value for latency
                ParseHist(line, value)
                continue
            if "EchoResult" in line:
                # assume this is the last action for one kv pair
                ParseThroughput(line, value)
                if is_compress:
                    g_compress_dict[key] = value
                else:
                    g_decompress_dict[key] = value
                key = ""
                value = {}


g_parser = argparse.ArgumentParser()


def ParseArgs():
    g_parser.add_argument(
        "--only_collect_data",
        action="store_true",
        help="Only collect data, not running perf. Make sure the two log files"
        + compress_data_file_name
        + " and "
        + decompress_data_file_name
        + " exist.",
    )
    g_parser.add_argument(
        "--copy_mode",
        action="store_true",
        help="perf for copy mode or not. If not set, all data collected are from zerocopy mode.",
    )
    g_parser.add_argument(
        "--perf_type",
        type=str,
        choices=["both", "compress_only", "decompress_only"],
        default="both",
    )
    g_parser.add_argument("--cpu_id", type=int, default=0)
    g_parser.add_argument("--section_name", type=str, default="")
    g_parser.add_argument(
        "--compression_level",
        type=int,
        default=0,
        choices=[0, 1, 6, 9],
        help="If not set, will use 0, which means all compression levels will be tested.",
    )
    return g_parser.parse_args()


def MatchKey(s, key, delimer=",") -> bool:
    # this function is avoiding such case:
    # Matching 'inflight_num=1' in a string. If string contains 'inflight_num=1', matching. If string contains 'inflight_num=16', not matching.
    # Obviously, Python keyword 'in' is not a good choice here. Hence the function.
    start_pos = s.find(key)
    if start_pos == -1:
        return False
    end_pos = s[start_pos:].find(delimer)
    # means the key is exactly the key
    return end_pos == -1 or end_pos == len(key)


def RenderLatThrouCsv(is_compress, compression_level):
    if is_compress:
        data_dict = g_compress_dict
    else:
        data_dict = g_decompress_dict
    fname = "compression" if is_compress else "decompression"
    fname += "_level" + str(compression_level) + ".csv"
    level_str = "compression_level=" + str(compression_level)
    compression_perf_str = (
        "compression_perf=true" if is_compress else "compression_perf=false"
    )
    decompression_perf_str = (
        "decompression_perf=false" if is_compress else "decompression_perf=true"
    )
    with open(fname, "w") as f:
        f.write(("Compression" if is_compress else "Decompression") + ",\n")
        for channel_num in channel_nums:
            for sgl_size in sgl_sizes:
                channel_num_str = "channel_num=" + str(channel_num)
                sgl_size_str = "sgl_size=" + str(sgl_size)
                block_header = channel_num_str + " " + sgl_size_str
                block = {}
                for source_size in source_sizes:
                    block["source_size=" + str(source_size)] = {}
                    for inflight_num in inflight_nums:
                        block["source_size=" + str(source_size)][
                            "inflight_num=" + str(inflight_num)
                        ] = "N/A"
                for k, v in data_dict.items():
                    # ignore other compression levels
                    if (
                        MatchKey(k, level_str)
                        and MatchKey(k, compression_perf_str)
                        and MatchKey(k, decompression_perf_str)
                        and MatchKey(k, channel_num_str)
                        and MatchKey(k, sgl_size_str)
                    ):
                        start_pos = k.find("source_size")
                        end_pos = start_pos + k[start_pos:].find(",")
                        source_size_str = k[start_pos:end_pos].strip()
                        start_pos = k.find("inflight_num")
                        end_pos = start_pos + k[start_pos:].find(",")
                        inflight_num_str = k[start_pos:end_pos].strip()
                        block[source_size_str][inflight_num_str] = (
                            v["AVG"]
                            + " "
                            + v["P999"]
                            + " "
                            + v["MAX"]
                            + " "
                            + (
                                v["consume speed"]
                                if is_compress
                                else v["produce speed"]
                            )
                        )
                # to two blocks in csv format
                f.write(block_header + ",\n")
                # first row
                l = "Latency(AVG P999 MAX) and Throughput,"
                for inflight_num in inflight_nums:
                    inflight_num_str = "inflight_num=" + str(inflight_num)
                    l += inflight_num_str + ","
                f.write(l + "\n")
                # block
                for source_size in source_sizes:
                    source_size_str = "source_size=" + str(source_size)
                    l = source_size_str + ","
                    for inflight_num in inflight_nums:
                        inflight_num_str = "inflight_num=" + str(inflight_num)
                        l += block[source_size_str][inflight_num_str] + ","
                    f.write(l + "\n")


def RenderRatioCsv(compression_level):
    # this requires the compression results exist
    fname = "compression_ratio_level" + str(compression_level) + ".csv"
    data_dict = g_compress_dict
    if not FileExists(compress_data_file_name):
        print("err:", compress_data_file_name, "not exist")
        return
    with open(compress_data_file_name, "r") as compress_data_f:
        # Build the map
        d = {}
        for source_size in source_sizes:
            source_size_str = "source_size=" + str(source_size)
            d[source_size_str] = {}
            for sgl_size in sgl_sizes:
                sgl_size_str = "sgl_size=" + str(sgl_size)
                d[source_size_str][sgl_size_str] = "N/A"
        for k, v in data_dict.items():
            if (
                MatchKey(k, "compression_level=" + str(compression_level))
                and MatchKey(k, "compression_perf=true")
                and MatchKey(k, "decompression_perf=false")
            ):
                start_pos = k.find("source_size")
                end_pos = start_pos + k[start_pos:].find(",")
                source_size_str = k[start_pos:end_pos].strip()
                start_pos = k.find("sgl_size")
                end_pos = start_pos + k[start_pos:].find(",")
                sgl_size_str = k[start_pos:end_pos].strip()
                d[source_size_str][sgl_size_str] = v["compression_ratio"]
        with open(fname, "w") as f:
            # Build the frame
            f.write("Compression ratio,\n")
            # First line with titles
            line = "Level " + str(compression_level) + ","
            for sgl_size in sgl_sizes:
                sgl_size_str = "sgl_size=" + str(sgl_size)
                line += sgl_size_str + ","
            f.write(line + "\n")
            for source_size in source_sizes:
                source_size_str = "source_size=" + str(source_size)
                line = ""
                line += source_size_str + ","
                for sgl_size in sgl_sizes:
                    sgl_size_str = "sgl_size=" + str(sgl_size)
                    line += d[source_size_str][sgl_size_str] + ","
                f.write(line + "\n")


def main():
    args = ParseArgs()
    if args.compression_level != 0:
        global compression_levels
        compression_levels = [args.compression_level]
    print("Script args: ", args)
    if not args.only_collect_data:
        if args.perf_type == "both" or args.perf_type == "compress_only":
            print("Running compress perf...")
            produce_log(True, args.copy_mode, args.cpu_id, args.section_name)
            print("Collecting compress data...")
        if args.perf_type == "both" or args.perf_type == "decompress_only":
            print("Running decompress perf...")
            produce_log(False, args.copy_mode, args.cpu_id, args.section_name)
            print("Collecting decompress data...")
    CollectData(compress_data_file_name, True)
    CollectData(decompress_data_file_name, False)
    for is_compress in [True, False]:
        for compression_level in compression_levels:
            RenderLatThrouCsv(is_compress, compression_level)
    for compression_level in compression_levels:
        RenderRatioCsv(compression_level)


if __name__ == "__main__":
    main()
