#!/usr/bin/env python -u

"""This utility script takes combined stderr/stdout output from a binary
instrumented by malloc_instrument.so and calculates memory differentials
between actual output lines and periods of inactivity.
"""

__author__ = "JT Olds <jt@spacemonkey.com>"

import re
import sys
import Queue
import threading
from collections import defaultdict

OUTPUT_PREFIX = "|||||||||||||||||||||| "

class Allocation:
    def __init__(self, size, module):
        self.size = size
        self.module = module

def process(stream):
    queue = Queue.Queue()

    def readingThread(queue):
        for line in stream:
            queue.put(line)
        queue.put(None)

    thread = threading.Thread(target=readingThread, args=(queue,))
    thread.daemon = True
    thread.start()

    allocations = {}
    current_total = 0
    last_total = 0
    module_snapshot = defaultdict(lambda: 0)
    modules = defaultdict(lambda: 0)

    malloc_re = re.compile(r'^\[[^]]+\]: (.+)\(.+: malloc\(([0-9]+)\) = (.*)\n$')
    free_re = re.compile(r'^\[[^]]+\]: (.+)\(.+: free\((.*)\)\n$')
    realloc_re = re.compile(r'^\[[^]]+\]: (.+)\(.+: realloc\(([^,]*), ([0-9]+)\) = (.*)\n$')
    calloc_re = re.compile(r'^\[[^]]+\]: (.+)\(.+: calloc\(([0-9]+), ([0-9]+)\) = (.*)\n$')

    lines = 0
    exited = False
    while not exited:
        try:
            line = queue.get(timeout=1.0)
            if line is None:
                exited = True
        except Queue.Empty:
            line = None
        if line is not None and line.startswith("  >>"):
            continue
        if line is None or line[:len(OUTPUT_PREFIX)] != OUTPUT_PREFIX:
            diff = current_total - last_total
            last_total = current_total
            if diff != 0:
                sys.stdout.write("-- diff: %d bytes\n" % diff)
            if line is not None:
                sys.stdout.write(line)
            module_diffs = defaultdict(lambda: 0)
            for k, v in modules.iteritems():
                diff = v - module_snapshot[k]
                if diff != 0:
                    module_diffs[k] = diff
            module_snapshot = modules.copy()
            for k, v in module_diffs.iteritems():
                sys.stdout.write("-- diff %s: %d bytes\n" % (k, v))
            continue
        line = line[len(OUTPUT_PREFIX):]
        match = malloc_re.match(line)
        if match:
            caller = match.group(1)
            size = int(match.group(2))
            address = match.group(3)
            modules[caller] += size;
            current_total += size
            allocations[address] = Allocation(size, caller)
            continue
        match = free_re.match(line)
        if match:
            caller = match.group(1)
            address = match.group(2)
            allocation = allocations.get(address, None)
            if allocation:
                current_total -= allocation.size
                modules[caller] -= allocation.size;
                del allocations[address]
            continue
        match = realloc_re.match(line)
        if match:
            caller = match.group(1)
            old_address = match.group(2)
            old_allocation = allocations.get(old_address, None)
            if old_allocation:
                current_total -= old_allocation.size
                modules[old_allocation.module] -= old_allocation.size;
                del allocations[old_address]
            size = int(match.group(3))
            new_address = match.group(4)
            current_total += size
            modules[caller] += size;
            allocations[new_address] = Allocation(size, caller)
            continue
        match = calloc_re.match(line)
        if match:
            caller = match.group(1)
            size = int(match.group(2)) * int(match.group(3))
            address = match.group(4)
            current_total += size
            modules[caller] += size;
            allocations[address] = Allocation(size, caller)
            continue
        sys.stdout.write("unhandled malloc line: %s\n" % line)
    sys.stdout.write("-- allocated at exit: %d bytes\n" % current_total)
    modules = defaultdict(lambda: 0)
    for k, v in allocations.iteritems():
        sys.stdout.write("unfreed %s (%s): %d bytes\n" % (k, v.module, v.size))
        modules[v.module] += v.size
    for k, v in modules.iteritems():
        sys.stdout.write("-- allocated by %s at exit: %d bytes\n" % (k, v))


def main():
    process(sys.stdin)


if __name__ == "__main__":
    main()
