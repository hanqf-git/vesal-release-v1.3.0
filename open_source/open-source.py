import os
import re
import shutil
from pathlib import Path
import sys

# This script should be running at the root of vesal like 'python3 open_source/open-source.py'. It will make the workspace ready to be go open source. It does the following:
# 1. Remove all the files in .codebase
# 2. Modify the cmake files and source files, remove all the code related to byte metrics and lz4ipp because they are internal stuff.
# 3. Completely clean all submodules, in open-source we use get_deps.py to fetch them.
# 4. Remove this file and move the build-open-source.sh to overwrite the build.sh. New build.sh will be used to build the open source version without bytelib and ipp stuff, and use the get_deps.py to fetch the dependencies, instead of using the submodules.

TARGET_DIRS = ["."]
C_EXTENSIONS = (".cc", ".c", ".h")
CMAKE_FILENAME = "CMakeLists.txt"
TARGET_VARS = ["VESAL_ENABLE_BYTE_METRICS", "VESAL_ENABLE_LZ4IPP"]

TO_BE_DELETED_DIRS = [".codebase", "benchmark", "docker"]

SUBMODULE_PATHS = [
    "third/spdlog",
    "third/gflags",
    "third/gperftools",
    "third/googletest",
    "third/zstd",
    "third/lz4",
    "third/dsa-uio-config",
    "third/libdeflate",
    "third/zlib",
    "third/openssl",
    "third/flat_hash_map",
]


def clean_git_submodules():
    gitmodules_path = Path(".gitmodules")
    if gitmodules_path.exists():
        gitmodules_path.unlink()
        print("Removed .gitmodules file")
    git_config_path = Path(".git/config")
    if git_config_path.exists():
        with open(git_config_path, "r") as f:
            lines = f.readlines()
        filtered = []
        skip = False
        for line in lines:
            if line.strip().startswith("[submodule "):
                skip = True
            elif skip and line.startswith("["):
                skip = False
            if not skip:
                filtered.append(line)

        with open(git_config_path, "w") as f:
            f.writelines(filtered)
        print("Cleaned .git/config")
    git_modules_dir = Path(".git/modules")
    if git_modules_dir.exists():
        for submodule in SUBMODULE_PATHS:
            module_path = git_modules_dir / submodule
            if module_path.exists():
                shutil.rmtree(module_path)
                print(f"Removed git module: {module_path}")
    for submodule in SUBMODULE_PATHS:
        path = Path(submodule)
        if path.exists():
            shutil.rmtree(path)
            print(f"Removed workspace submodule: {path}")


def process_c_files(filepath):
    modified = []
    current_var = None
    block_depth = 0

    with open(filepath, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()

            if not current_var:
                for var in TARGET_VARS:
                    if stripped.startswith(f"#ifdef {var}") or stripped.startswith(
                        f"#if defined({var}"
                    ):
                        current_var = var
                        block_depth = 1
                        break
                else:
                    modified.append(line)
                continue

            if stripped.startswith("#if"):
                block_depth += 1
            elif stripped.startswith("#endif"):
                block_depth -= 1
                if block_depth == 0:
                    current_var = None

    if modified:
        with open(filepath, "w", encoding="utf-8") as f:
            f.writelines(modified)
        print(f"Processed C: {filepath}")


def process_cmake(filepath):
    TARGET_VARS = ["VESAL_ENABLE_BYTE_METRICS", "VESAL_ENABLE_LZ4IPP", "VESAL_BUILD_BENCH"]
    modified = []
    in_block = False
    block_depth = 0
    
    var_pattern = r'|'.join(re.escape(var) for var in TARGET_VARS)
    block_pattern = re.compile(r'^\s*if\s*\(\s*\$\{(' + var_pattern + r')\}\s*\)\s*$', re.IGNORECASE)
    expr_pattern = re.compile(
        r'(?i)(\s*if\s*\(.*?)(?:\b(?:AND|OR)\b\s*(?:NOT\s+)?\$\{(' + var_pattern + r')\})(.*?)(\))'
    )

    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            stripped = line.strip()
            
            if not in_block and block_pattern.match(stripped):
                in_block = True
                block_depth = 1
                continue
                
            if in_block:
                if stripped.startswith('if('):
                    block_depth += 1
                elif stripped.startswith('endif()'):
                    block_depth -= 1
                    if block_depth == 0:
                        in_block = False
                        continue
                continue
            
            line = process_line_conditions(line, expr_pattern)
            modified.append(line)
    
    if modified:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.writelines(modified)
        print(f"Processed CMake: {filepath}")

def process_line_conditions(line, pattern):
    while True:
        match = pattern.search(line)
        if not match:
            break
            
        new_condition = match.group(1).rstrip() + match.group(3) + match.group(4)
        line = line[:match.start()] + new_condition + line[match.end():]
        
        line = re.sub(r'(?i)\b(AND|OR)\s+$', '', line)  
        line = line.replace('()', '')  
    
    line = re.sub(r'if\(\s*\)', 'if(TRUE)', line) 
    return line

def walk_directories():
    for dir_path in TARGET_DIRS:
        if not os.path.exists(dir_path):
            continue
        for root, dirs, files in os.walk(dir_path):
            for file in files:
                filepath = os.path.join(root, file)
                if Path(file).suffix in C_EXTENSIONS:
                    process_c_files(filepath)
                elif file == CMAKE_FILENAME:
                    process_cmake(filepath)


def remove_dirs():
    for dir_path in TO_BE_DELETED_DIRS:
        if os.path.exists(dir_path):
            os.system(f"rm -rf {dir_path}")
            print(f"Removed directory: {dir_path}")

def final_clean_up():
    os.system(f"rm -f {__file__}")
    os.system(f"mv -f open_source/build-open-source.sh build.sh")
    os.system(f"mv -f open_source/get-deps.py get-deps.py")
    os.system(f"rmdir open_source")

# Make sure at the right position
def prepare():
    wd = os.getcwd()
    if wd.split("/")[-1].lower() != "vesal" or not os.path.isdir(".codebase") or not os.path.isdir("third") or not os.path.isdir("src") or not os.path.isdir("include"):
        print("Please run this script from the root of the Vesal repository. Currently at: " + wd)
        sys.exit(1)
    
if __name__ == "__main__":
    prepare()
    remove_dirs()
    clean_git_submodules()
    walk_directories()
    final_clean_up()
    print("All processing completed.")
