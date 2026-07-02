#!/usr/bin/python
# -*- coding: UTF-8 -*-

# Running valgrind tests ("*_test") under the current directory.
# By default the script will not search the current recursively
# unless '-r' (argv[6]) is specified (non '-r'-valued argv[6] means non-recursive).
#
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

import sys
import os
import threading
import Queue
import time
import tempfile
import glob

kPollTimeout = 5
kWorkerTimeout = 1800
beginTime = {}
endTime = {}
results = {}
sysresults = {}
cmdMap = {}
indexMap = {}
tempFiles = {}
memErrorReport = {}
memLeakReport = {}
gtestReport = {}
timeoutInfo = {}
cmd = []
sysOut = True
kValgrindRepeat = 3
blockListPath = None
is_test_search_recursive = False
valgrind_ignore_file_name = "valgrind_ignore.supp"

# Create workers for callable function with arguments.
def createWorkers(callMethod, argList, callback=None):
    workers = []
    for arg in argList:
        if isinstance(arg, tuple):
            workers.append(Worker(callMethod, arg[0], arg[1], callback=callback))
        else:
            workers.append(Worker(callMethod, [arg], None, callback=callback))
    return workers

# All workers have been scheduled and processed.
class Finished(Exception):
    pass

# Worker thread in the background.
class WorkerThread(threading.Thread):
    def __init__(self, workerQueue, outputQueue, **kwds):
        threading.Thread.__init__(self, **kwds)
        self.setDaemon(1)
        self._workerQueue = workerQueue
        self._outputQueue = outputQueue
        self.event = threading.Event()
        self.start()

    def run(self):
        # Worker queue is processed until exits.
        while True:
            if self.event.isSet():
                break
            try:
                worker = self._workerQueue.get(True, kPollTimeout)
            except Queue.Empty:
                continue
            if self.event.isSet():
                self._workerQueue.put(worker)
                break
            try:
                output = worker.callMethod(*worker.args, **worker.kwds)
                self._outputQueue.put((worker, output))
            except:
                worker.exception = True
                self._outputQueue.put((worker, sys.exc_info()))

# Define a worker which can be executed by callMethod.
class Worker:
    def __init__(self, callMethod, args=None, kwds=None, workerID=None, callback=None):
        if workerID is None:
            self.workerID = id(self)
        else:
            self.workerID = hash(workerID)
        self.exception = False
        self.callback = callback
        self.callMethod = callMethod
        self.args = args or []
        self.kwds = kwds or {}

# Worker thread scheduler, provided by adding and join workers.
class WorkerScheduler:
    def __init__(self, numWorkers):
        self._workerQueue = Queue.Queue(0)
        self._outputQueue = Queue.Queue(0)
        self._workers = []
        self._workerMap = {}
        self.addWorkerThread(numWorkers)

    def addWorkerThread(self, numWorkers):
        for i in range(numWorkers):
            self._workers.append(WorkerThread(self._workerQueue, self._outputQueue))

    def joinWorkers(self):
        for i in range(len(self._workers)):
            worker = self._workers.pop()
            worker.event.set()
            worker.join()

    def addWorker(self, worker):
        self._workerQueue.put(worker, True, None)
        self._workerMap[worker.workerID] = worker

    def schedule(self):
        while True:
            if not self._workerMap:
                raise Finished
            try:
                # Get next results
                worker, output = self._outputQueue.get_nowait()
                if worker.callback and not worker.exception:
                    worker.callback(worker, output)
                del self._workerMap[worker.workerID]
            except Queue.Empty:
                break

    def killTimeoutWorkers(self):
        for w in self._workerMap:
            j = indexMap[w]
            if beginTime.has_key(j) and not endTime.has_key(j) and time.time() > beginTime[j] + kWorkerTimeout:
                command = "pid=`ps -ef|grep \"" + cmdMap[w] + \
                          "\"|grep -v \"sh -c \"|grep -v grep|grep -v gdb|awk '{print $2}'`; " + \
                          "if [ \"$pid\" ];then echo \"KILLED " + cmdMap[w] + "\"; " + \
                          "ulimit -c unlimited && gcore -o core_" + os.path.basename(cmdMap[w]) + " $pid; " + \
                          "pkill -9 -f " + cmdMap[w] + "; fi"
                endTime[j] = time.time()
                timeoutInfo[j] = True
                os.system(command)

if __name__ == '__main__':
    def runTest(index):
        # Run in the current working directory.
        tempFiles[index] = tempfile.NamedTemporaryFile()
        tmpFile = tempfile.NamedTemporaryFile()
        dir_name = os.path.dirname(os.path.abspath(__file__))
        test_name = os.path.basename(cmd[index])
        supp_file_path = os.path.join(dir_name, valgrind_ignore_file_name)
        valgrind_log_path =  "valgrind_report/valgrind_case_%s.log" % test_name
        if os.path.exists(supp_file_path):
            command = "valgrind -v --num-callers=20 --fullpath-after=. --read-inline-info=yes --leak-check=full --show-leak-kinds=definite,indirect --track-origins=yes --errors-for-leak-kinds=definite,indirect --run-libc-freeres=no --gen-suppressions=all --log-file=" + \
                       valgrind_log_path  + " --suppressions=" + supp_file_path + " " + \
                       cmd[index] + " --gtest_repeat=" + str(kValgrindRepeat) + " >" + tmpFile.name + " >valgrind_report/" + test_name + ".report 2>&1"
        else:
            print("something's not right! cannot find suppression file")
            command = "valgrind -v --num-callers=20 --fullpath-after=. --read-inline-info=yes --leak-check=full --show-leak-kinds=definite,indirect --track-origins=yes --errors-for-leak-kinds=definite,indirect --run-libc-freeres=no --gen-suppressions=all --log-file=" + \
                       valgrind_log_path + " " + \
                       cmd[index] + " --gtest_repeat=" + str(kValgrindRepeat) + " >" + tmpFile.name + " >valgrind_report/" + test_name + ".report 2>&1"
        timeoutInfo[index] = False
        beginTime[index] = time.time()
        output = os.system(command)
        endTime[index] = time.time()
        results[index] = output >> 8
        sysresults[index] = output & 255
        return output

    def outputResult(worker, output):
        print "Done %s status: %d" % (cmdMap[worker.workerID], output)

    os.system("mkdir -p valgrind_report")
    os.system("echo && echo \"********** Valgrind Run **********\" && echo")
    numWorkers = 0
    addedWorkers = 2
    if len(sys.argv) >= 2:
        numWorkers = int(sys.argv[1])
    if len(sys.argv) >= 3:
        addedWorkers = int(sys.argv[2])
    if len(sys.argv) >= 4:
        kWorkerTimeout = int(sys.argv[3])
    if len(sys.argv) >= 5:
        kValgrindRepeat = int(sys.argv[4])
    if len(sys.argv) >= 6:
        if sys.argv[5] != 'none':
            blockListPath = sys.argv[5]
    if len(sys.argv) >= 7:
        is_test_search_recursive = (sys.argv[6] == '-r')
    if len(sys.argv) >= 8:
        valgrind_ignore_file_name = sys.argv[7]

    def find_test_programs():
        results = []
        for dirname, _, filenames in os.walk('./'):
            if dirname == './' or is_test_search_recursive:
                results += [os.path.join(dirname, f) for f in filenames if f.endswith('_test')]

        return results

    cmd = find_test_programs()
    max_parallelism = min(len(cmd), int(os.popen("cat /proc/cpuinfo |grep processor -c").read()))
    if numWorkers <= 0:
        numWorkers = max_parallelism
    else:
        numWorkers = min(max_parallelism, numWorkers)

    if blockListPath:
        blockList = set()
        for blockItem in open(blockListPath):
            blockList.add(blockItem.replace('\n', ''))
        removeItems = []
        for testCase in cmd:
            if testCase in blockList:
                removeItems.append(testCase)
        for testCase in removeItems:
            cmd.remove(testCase)

    cmd = [t for t in cmd if \
           ("byte_keeper_test" not in t) and ("byte_status_macros_test" not in t)]
    indexes = [i for i in range(len(cmd))]
    workers = createWorkers(runTest, indexes, outputResult)

    # Create a pool of worker threads.
    scheduler = WorkerScheduler(numWorkers)

    i = 0
    for w in workers:
        scheduler.addWorker(w)
        cmdMap[w.workerID] = cmd[i]
        indexMap[w.workerID] = i
        i += 1

    interrupt = False
    i = 0
    while True:
        try:
            time.sleep(1)
            scheduler.schedule()
            # exceed 5 mins, add worker thread
            if i == 5 * 60:
                scheduler.addWorkerThread(addedWorkers)
            if i % 10 == 0:
                scheduler.killTimeoutWorkers()
            i += 1
        except KeyboardInterrupt:
            print "***** Interrupted! *****"
            interrupt = True
            break
        except Finished:
            break

    print "\n******************** Summary ********************\n"
    # Print all error reports.
    for i in range(len(cmd)):
        test_name = os.path.basename(cmd[i])
        command0 = "grep -v \"PASS\|^\[201\|^$\" " + tempFiles[i].name
        gtestReport[i] = os.system(command0 + "|grep \"\[  FAILED  \]\"")
        reportPath = "valgrind_report/%s.report" % test_name
        logPath =  "valgrind_report/valgrind_case_%s.log" % test_name
        if timeoutInfo[i]:
            continue
        command1 = "grep 'ERROR SUMMARY' %s %s |grep -v '0 errors from 0 contexts'" % (reportPath, logPath)
        memErrorReport[i] = os.system(command1)
        # ignore brpc bvar warning
        if memErrorReport[i] == 0:
            command2 = "bvar=`grep '== Thread' %s %s -A 4 | tail -n1 | awk '{print $4}'`; echo $bvar|grep -vw 'append_second' >/dev/null" % (reportPath, logPath)
            memErrorReport[i] = \
                os.system(command2 + " && echo MemError occurs in %s, see more at %s/%s or %s && echo" % (test_name, os.getcwd(), reportPath, logPath))

        command3 = "grep 'no leaks are possible' %s %s|wc -l|grep -vw '1' >/dev/null" % (reportPath, logPath)
        memLeakReport[i] = os.system(command3)

        if memLeakReport[i] == 0:
            command4 = "grep 'still reachable: 0 bytes in 0 blocks' %s %s >/dev/null" % (reportPath, logPath)
            memLeakReport[i] = \
                os.system(command4 + " && echo MemLeak occurs in %s, see more at %s/%s or %s && echo" % (test_name, os.getcwd(), reportPath, logPath))

    # Print the summaries.
    for i in range(len(cmd)):
        duration = endTime[i] - beginTime[i]
        if sysresults[i] == 2:
            # Interrupted.
            outString = "\033[0;31mINTERRUPTED\033[0m"
            sysOut = False
        elif gtestReport[i] == 0:
            # Error report found.
            outString = "\033[0;31mFAIL\033[0m"
            sysOut = False
        elif timeoutInfo[i]:
            # Binary is timeout
            outString = "\033[0;31mTIMEOUT\033[0m"
            sysOut = False
        elif results[i] == 11 or results[i] == 139:
            # Process Core dump.
            outString = "\033[0;31mCORE\033[0m"
            sysOut = False
        elif results[i] == 6 or results[i] == 134:
            # Process Abort.
            outString = "\033[0;31mABORT\033[0m"
            sysOut = False
        elif results[i] <> 0:
            # Process NOT return 0.
            outString = "\033[0;31mBINARY\033[0m"
            sysOut = False
        elif memErrorReport[i] == 0:
            # Memory error.
            outString = "\033[0;31mMEM_ERROR\033[0m"
            sysOut = False
        elif memLeakReport[i] == 0:
            # Memory leak.
            outString = "\033[0;31mMEM_LEAK\033[0m"
            sysOut = False
        else:
            outString = "\033[0;32mPASS\033[0m"
        print "%s:   %s   %.2f seconds" % (cmd[i], outString, duration)
    if sysOut == True:
        sys.exit(0)
    else:
        sys.exit(1)
