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

# Clocks of a filler (move.l d1,d5), to check the clock of the file
FILLER_CLOCKS = {"68000": 4, "68020": 2, "68030": 2, "68040": 1}
# A move.b (a0),d0 takes this many clocks on top of its read cycle. MC68030
# User's Manual, 11.6: fetch 3 + move 2 with a two-clock read. Not worked out
# for the other CPUs, so they only get raw times. See bustiming-calibration.md.
READ_OVERHEAD = {"68030": 3}


class Row:
    def __init__(self, fields):
        self.k = int(fields[0])
        self.gap_ns = float(fields[1])
        self.raw_ns = float(fields[2])
        self.over_ns = float(fields[4])


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
        self.stated_hz = self.clock_hz
        self.notes = []
        reg = next((t for t in self.targets if t.name == "register only"), None)
        self.targets = [t for t in self.targets if t.rows and t.name != "register only"]
        self.check_clock(reg)
        self.check_cache(reg)

    def check_clock(self, reg):
        """A file only assumes the clock of its model: trust the fillers when
        they are more than 10% off (a few percent is normal: the V8's 7%)"""
        rows = [r for r in reg.rows if 1 <= r.k <= 8] if reg else []
        if not rows or not self.clock_hz or self.cpu not in FILLER_CLOCKS:
            return
        filler_ns = max(r.gap_ns for r in rows) / max(r.k for r in rows)
        measured = FILLER_CLOCKS[self.cpu] * 1e9 / filler_ns
        if abs(measured - self.clock_hz) > 0.1 * self.clock_hz:
            self.notes.append("the file says %.3f MHz, its fillers say %.1f MHz (clock chipped, or an emulator "
                              "with other instruction times): using the fillers" % (self.clock_hz / 1e6, measured / 1e6))
            self.clock_hz = measured

    def check_cache(self, reg):
        """A cache card makes small loops as fast as the CPU's own caches"""
        by_name = {t.name: t for t in self.targets}
        ram = by_name.get("RAM read")
        if self.cpu not in READ_OVERHEAD or not ram:
            return
        clk = self.clk_ns()
        fast = ram.rows[0].raw_ns / clk <= READ_OVERHEAD[self.cpu] + 2.4
        fetch = by_name.get("Fetch, I-cache off")
        unfetched = fetch and reg and fetch.rows[0].raw_ns <= reg.rows[0].raw_ns + 0.3 * clk
        if fast or unfetched:
            self.notes.append("RAM reads at the speed of a two-clock read%s: a cache card, or an emulator without "
                              "wait states. RAM, ROM and screen numbers may not be the memory's"
                              % (", and the I-cache off fetch isn't slower" if unfetched else ""))

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


def knee_ns(res, t, floor_ns):
    for r in t.rows:
        if r.over_ns <= floor_ns + 0.25 * res.clk_ns():
            return r.gap_ns
    return None


def classify(res, t):
    """Returns (short, long): a table cell and a sentence. All in clocks; raw
    is the time per access back to back."""
    clk = res.clk_ns()
    b2b = t.rows[0]
    raw = b2b.raw_ns / clk
    floor_ns = min(r.over_ns for r in t.rows)
    floor = floor_ns / clk
    spread_ns = max(r.over_ns for r in t.rows) - floor_ns
    b2b_via = b2b.raw_ns / res.via_ns()
    knee = knee_ns(res, t, floor_ns)
    overhead = READ_OVERHEAD.get(res.cpu)

    if t.name.startswith(("Fetch", "D-cache")):
        step = (t.rows[1].raw_ns - b2b.raw_ns) / clk if len(t.rows) > 1 else 0
        return ("%.1f" % raw, "raw %.1f, then %.1f more with one filler" % (raw, step))

    if floor < 0.5:
        text = "posted write: one every %s back to back" % clocks(raw)
        if near_int(b2b_via):
            text += ", %s" % per_via_cycles(round(b2b_via))
        if knee is not None:
            text += "; hidden by %.0f ns of other work" % knee
        return ("%.1f/w" % raw, text)

    if spread_ns > 0.3 * res.via_ns() and near_int(b2b_via):
        n = round(b2b_via)
        text = ("synchronous with the VIA clock: one access %s back to back, CPU held %s "
                "(%.2f VIA cycle) at best; still one %s with up to %.0f ns of "
                "other work between two accesses"
                % (per_via_cycles(n), clocks(floor), floor_ns / res.via_ns(), per_via_cycles(n), via_slack(res, t, n)))
        return ("%dV/%.1f" % (n, floor), text)

    # floor + the register-only loop (which is the time of the same instruction
    # without a read) is the raw time when nothing else holds the access up
    alone = raw - (b2b.over_ns - floor_ns) / clk
    if " write" in t.name:
        return ("%.1f" % raw, "write, not posted (the CPU waits for it): one every %s" % clocks(raw))
    cycle = "; %s read cycle" % clocks(alone - overhead) if overhead else ""
    if raw - alone > 0.75:
        rec = raw - alone
        text = "raw %s isolated%s, then a recovery of %s (%.0f ns): %s back to back" % (
            clocks(alone), cycle, clocks(rec), rec * clk, clocks(raw))
        if knee is not None:
            text += "; no wait once %.0f ns of other work separates two accesses" % knee
        return ("%.1f/%.1f" % (raw, alone), text)

    return ("%.1f" % alone, "raw %s%s" % (clocks(alone), cycle))


def report(res):
    clk = res.clk_ns()
    print("%s (%s)" % (res.machine, os.path.basename(res.path)))
    print("  %s at %.3f MHz: 1 clock = %.1f ns" % (res.cpu, res.clock_hz / 1e6, clk))
    print("  VIA clock %.0f Hz: 1 VIA cycle = %.1f ns = %.1f clocks"
          % (res.via_hz, res.via_ns(), res.via_ns() / clk))
    for note in res.notes:
        print(textwrap.fill("WARNING: " + note, 78, initial_indent="  ", subsequent_indent="  "))
    overhead = READ_OVERHEAD.get(res.cpu)
    if overhead:
        print("  Read cycle = raw - %d. Posted write: its period is about its cycle." % overhead)
    else:
        print("  Cycle lengths aren't worked out for the %s: raw times only." % res.cpu)
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
    print("Raw clocks per access. N: N. B/A: B back to back, A isolated (recovery).")
    print("NV/F: N VIA cycles back to back, CPU held F clocks at best. B/w: posted")
    print("write, one every B clocks.")
    for i, res in enumerate(results):
        for note in res.notes:
            print("%s: %s" % (labels[i], note))


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
