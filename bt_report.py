#!/usr/bin/env python3
"""Turn "BusTiming Results" files into CPU clocks and plain words.

    bt_report.py results/hw-maciici.txt          one report per file
    bt_report.py -c results/snow-*.txt           one table, a column per file

See README-method.md for what the program measures.
"""

import argparse
import os
import re
import sys
import textwrap

# Shortest bus cycle without wait states, in CPU clocks: synchronous on the
# 68000, asynchronous (DSACK) on the 68020/68030. Not derived on the 68040,
# whose bus runs its own way (see README-method.md).
BASE_CYCLE = {"68000": 4, "68020": 3, "68030": 3}


class Row:
    def __init__(self, fields):
        self.k = int(fields[0])
        self.gap_ns = float(fields[1])
        self.raw_ns = float(fields[2])
        self.bus_ns = float(fields[4])
        self.bus_clk = float(fields[5])


class Target:
    def __init__(self, name, addr):
        self.name = name
        self.addr = addr
        self.rows = []


class Results:
    def __init__(self, path):
        self.path = path
        self.machine = "?"
        self.cpu = "?"
        self.clock_hz = None
        self.via_hz = 783360.0
        self.targets = []
        self.parse(path)

    def parse(self, path):
        target = None
        with open(path, errors="replace") as f:
            for line in f:
                line = line.rstrip()
                m = re.match(r"Machine: (.*?) \(gestalt.*CPU (\d+), clock (\d+) Hz", line)
                if m:
                    self.machine, self.cpu, self.clock_hz = m.group(1), m.group(2), int(m.group(3))
                    continue
                m = re.match(r"VIA clock: (\d+) Hz", line)
                if m:
                    self.via_hz = float(m.group(1))
                    continue
                m = re.match(r"(.+?) @ \$([0-9A-F]+)$", line)
                if m:
                    target = Target(m.group(1), m.group(2))
                    self.targets.append(target)
                    continue
                fields = line.split()
                if target and len(fields) >= 9 and fields[0].isdigit():
                    target.rows.append(Row(fields))
                elif not line:
                    target = None
        self.targets = [t for t in self.targets if t.rows and t.name != "register only"]

    def clk_ns(self):
        return 1e9 / self.clock_hz

    def via_ns(self):
        return 1e9 / self.via_hz


def near_int(x, tol=0.06):
    return abs(x - round(x)) <= tol and round(x) >= 1


def clocks(x):
    """'4 clocks' when within 0.15 of a whole number, else '3.6 clocks'"""
    if abs(x - round(x)) <= 0.15:
        return "%d clock%s" % (round(x), "" if round(x) == 1 else "s")
    return "%.1f clocks" % x


def per_via_cycles(n):
    return "per VIA cycle" if n == 1 else "per %d VIA cycles" % n


def via_slack(res, t, n):
    """The most other work (filler time) between two accesses that still
    keeps them n VIA cycles apart"""
    slack = 0.0
    for r in t.rows:
        ratio = r.raw_ns / res.via_ns()
        if not (near_int(ratio) and round(ratio) == n):
            break
        slack = r.gap_ns
    return slack


def knee_ns(t, floor):
    for r in t.rows:
        if r.bus_clk <= floor + 0.25:
            return r.gap_ns
    return None


def classify(res, t):
    """Returns (short, long): a table cell and a sentence"""
    b2b = t.rows[0]
    floor = min(r.bus_clk for r in t.rows)
    spread_ns = max(r.bus_ns for r in t.rows) - min(r.bus_ns for r in t.rows)
    b2b_via = b2b.raw_ns / res.via_ns()
    knee = knee_ns(t, floor)
    base = BASE_CYCLE.get(res.cpu)

    def ws(clk):
        if base is None:
            return ""
        n = round(clk) - base
        return " (%d wait state%s)" % (n, "" if n == 1 else "s")

    if floor < 0.5:
        text = "posted write: %s back to back" % clocks(b2b.bus_clk)
        if near_int(b2b_via):
            text += ", one write %s" % per_via_cycles(round(b2b_via))
        if knee is not None:
            text += "; hidden by %.0f ns of other work" % knee
        return ("%.1f/w" % b2b.bus_clk, text)

    if spread_ns > 0.3 * res.via_ns() and near_int(b2b_via):
        n = round(b2b_via)
        floor_via = floor * res.clk_ns() / res.via_ns()
        text = ("synchronous with the VIA clock: one access %s back to back, "
                "%s (%.2f VIA cycle) at best; still one %s with up to %.0f ns of "
                "other work between two accesses"
                % (per_via_cycles(n), clocks(floor), floor_via, per_via_cycles(n), via_slack(res, t, n)))
        return ("%dV/%.1f" % (round(b2b_via), floor), text)

    if b2b.bus_clk - floor > 0.75:
        rec = b2b.bus_clk - floor
        text = ("%s%s, then a recovery of %s (%.0f ns): %s back to back"
                % (clocks(floor), ws(floor), clocks(rec), rec * res.clk_ns(), clocks(b2b.bus_clk)))
        if knee is not None:
            text += "; no wait once %.0f ns of other work separates two accesses" % knee
        return ("%.1f/%.1f" % (b2b.bus_clk, floor), text)

    return ("%.1f" % floor, "%s%s" % (clocks(floor), ws(floor)))


def report(res):
    clk = res.clk_ns()
    print("%s (%s)" % (res.machine, os.path.basename(res.path)))
    print("  %s at %.3f MHz: 1 clock = %.1f ns" % (res.cpu, res.clock_hz / 1e6, clk))
    print("  VIA clock %.0f Hz: 1 VIA cycle = %.1f ns = %.1f clocks"
          % (res.via_hz, res.via_ns(), res.via_ns() / clk))
    base = BASE_CYCLE.get(res.cpu)
    if base is not None:
        print("  Shortest bus cycle %d clocks: wait states = clocks - %d" % (base, base))
    print()
    width = max(len(t.name) for t in res.targets)
    indent = " " * (width + 4)
    for t in res.targets:
        text = textwrap.fill(classify(res, t)[1], 78, initial_indent=indent, subsequent_indent=indent)
        print("  %-*s  %s" % (width, t.name, text[len(indent):]))
    print()


def compare(results):
    names = []
    for res in results:
        for t in res.targets:
            if t.name not in names:
                names.append(t.name)
    labels = [os.path.splitext(os.path.basename(r.path))[0] for r in results]
    cells = {}
    for i, res in enumerate(results):
        for t in res.targets:
            cells[(t.name, i)] = classify(res, t)[0]
    width = max(len(n) for n in names)
    cols = [max(len(l), max((len(cells.get((n, i), "")) for n in names), default=0))
            for i, l in enumerate(labels)]
    print("%-*s  %s" % (width, "", "  ".join(l.rjust(c) for l, c in zip(labels, cols))))
    for n in names:
        row = [cells.get((n, i), "-").rjust(c) for i, c in enumerate(cols)]
        print("%-*s  %s" % (width, n, "  ".join(row)))
    print()
    print("Clocks of the CPU. N: N clocks. B/F: B back to back, F at best (recovery).")
    print("NV/F: N VIA cycles back to back, F clocks at best. B/w: posted write,")
    print("B clocks back to back.")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("-c", "--compare", action="store_true", help="one table, a column per file")
    p.add_argument("files", nargs="+")
    args = p.parse_args()
    results = [Results(f) for f in args.files]
    for res in results:
        if res.clock_hz is None or not res.targets:
            sys.exit("%s: not a BusTiming results file" % res.path)
    if args.compare:
        compare(results)
    else:
        for res in results:
            report(res)


if __name__ == "__main__":
    main()
