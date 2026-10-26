#!/usr/bin/env python3
"""Synthesize RTL to a gate-level netlist with yosys and verify OpenCDC parity.

For each design this tool:
  1. runs yosys to flatten + synthesize the RTL into a generic gate-level
     netlist (``$_DFF_*``/``$_AND_``... cells), written to a temp/output file,
  2. runs OpenCDC on the original RTL (``--input-type rtl``) and on the
     synthesized netlist (``--input-type netlist``),
  3. compares the two analyses: the set of crossed clock-domain pairs
     (CDC001) and the per-rule counts for the structural crossing rules
     (CDC001/CDC002/CDC004) must agree.

Reset-centric rules (CDC007/DC009) are intentionally excluded from the
comparison: netlist reset modelling is coarser than RTL and does not bear on
whether a domain crossing was found. Domain names are root clocks, which yosys
preserves, so pair comparison is name-stable across the two frontends.

Usage:
  synth_netlist.py --binary build/src/opencdc --rtl f.sv --top f
  synth_netlist.py --binary build/src/opencdc --rtl f.sv --top f --emit f.nl.v
  synth_netlist.py --binary build/src/opencdc --designs 'a.sv:f1;b.sv:f2'
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

# Rules whose domain-crossing behaviour must be identical RTL vs netlist.
PARITY_RULES = ("CDC001", "CDC002", "CDC004")

YOSYS_SCRIPT = """
read_verilog -sv {rtl}
hierarchy -check -top {top}
proc
opt
memory
opt
techmap
opt
abc -g AND,OR,XOR,NAND,NOR,XNOR,MUX
opt
clean
write_verilog -noattr -noexpr {out}
"""


def synthesize(rtl, top, out, yosys="yosys"):
    script = YOSYS_SCRIPT.format(rtl=rtl, top=top, out=out)
    # Pass the script as a FILE (-s), not as a -p option string: some yosys
    # builds parse -p with cxxopts, whose vector<string> values are
    # comma-split — the 'abc -g AND,OR,...' line would be cut into separate
    # commands and yosys would abort with "No such command: OR". Script
    # files go through yosys's own reader, where commas are ordinary
    # argument text, so the script works across yosys builds.
    fd, script_path = tempfile.mkstemp(suffix=".ys", text=True)
    try:
        with os.fdopen(fd, "w") as f:
            f.write(script)
        proc = subprocess.run(
            [yosys, "-q", "-s", script_path], capture_output=True, text=True
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"yosys failed for {rtl}:\n{proc.stdout}\n{proc.stderr}"
            )
    finally:
        try:
            os.unlink(script_path)
        except OSError:
            pass
    if not os.path.exists(out):
        raise RuntimeError(f"yosys produced no output netlist: {out}")


def run_opencdc(binary, files, top, input_type, extra_args=None):
    cmd = [binary, "check", *files, "--top", top,
           "--input-type", input_type, "--format", "json"]
    if extra_args:
        cmd.extend(extra_args)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    # 0 = no findings, 1 = findings present; both are valid analyses.
    if proc.returncode not in (0, 1):
        raise RuntimeError(
            f"opencdc ({input_type}) failed rc={proc.returncode}:\n"
            f"{proc.stdout}\n{proc.stderr}"
        )
    try:
        return json.loads(proc.stdout) if proc.stdout.strip() else {}
    except json.JSONDecodeError as e:
        raise RuntimeError(
            f"opencdc ({input_type}) emitted non-JSON output: {e}\n{proc.stdout}"
        )


def crossing_summary(report):
    """Extract domain-pair set and per-rule counts for the parity rules."""
    findings = report.get("findings", [])
    pairs = set()
    counts = {r: 0 for r in PARITY_RULES}
    for f in findings:
        rid = f.get("rule_id")
        if rid in counts:
            counts[rid] += 1
        if rid == "CDC001":
            pairs.add((f.get("source_domain"), f.get("dest_domain")))
    return pairs, counts


def compare(rtl_report, nl_report, strict_counts=False):
    """Compare RTL and netlist analyses.

    Returns (problems, notes):
      problems -- correctness mismatches: a domain pair crossed in one frontend
                  but not the other (a missed or fabricated crossing). Always
                  fatal.
      notes    -- per-rule count deltas. In a netlist a multi-bit RTL register
                  (`bus[7:0]`, one node) is bit-blasted into 8 flops, so one
                  wide crossing becomes 8 narrow ones (and CDC002 grouping is
                  lost) until bus regrouping lands. Same domain pairs are still
                  crossed, so this is granularity, not a missed crossing: it is
                  a warning by default and only fatal under strict_counts.
    """
    problems = []
    notes = []
    rtl_pairs, rtl_counts = crossing_summary(rtl_report)
    nl_pairs, nl_counts = crossing_summary(nl_report)

    if rtl_pairs != nl_pairs:
        missing = rtl_pairs - nl_pairs
        extra = nl_pairs - rtl_pairs
        if missing:
            problems.append(f"netlist missed domain pair(s): {sorted(missing)}")
        if extra:
            problems.append(f"netlist reported extra domain pair(s): {sorted(extra)}")

    for rule in PARITY_RULES:
        if rtl_counts[rule] != nl_counts[rule]:
            msg = (f"{rule} count differs (bit-blast granularity, same domain "
                   f"pairs): rtl={rtl_counts[rule]} netlist={nl_counts[rule]}")
            if strict_counts:
                problems.append(msg)
            else:
                notes.append(msg)
    return problems, notes


def check_design(binary, rtl, top, emit=None, cell_map=None, verbose=False,
                 strict_counts=False):
    """Synthesize + run both frontends + compare. Returns (ok, detail)."""
    tmpdir = None
    if emit:
        nl_path = emit
        os.makedirs(os.path.dirname(os.path.abspath(nl_path)), exist_ok=True)
    else:
        tmpdir = tempfile.mkdtemp(prefix="opencdc_parity_")
        nl_path = os.path.join(tmpdir, "synth.nl.v")

    try:
        synthesize(rtl, top, nl_path)

        extra = ["--cell-map", cell_map] if cell_map else None
        rtl_report = run_opencdc(binary, [rtl], top, "rtl", extra)
        nl_report = run_opencdc(binary, [nl_path], top, "netlist", extra)

        problems, notes = compare(rtl_report, nl_report, strict_counts=strict_counts)
        ok = not problems
        detail = {
            "design": top,
            "netlist": nl_path,
            "rtl_counts": crossing_summary(rtl_report)[1],
            "nl_counts": crossing_summary(nl_report)[1],
            "rtl_pairs": sorted(crossing_summary(rtl_report)[0]),
            "nl_pairs": sorted(crossing_summary(nl_report)[0]),
            "problems": problems,
            "notes": notes,
        }
        return ok, detail
    finally:
        if tmpdir and not emit:
            # Keep the netlist only when not explicitly emitted; else clean up.
            for f in os.listdir(tmpdir):
                try:
                    os.remove(os.path.join(tmpdir, f))
                except OSError:
                    pass
            try:
                os.rmdir(tmpdir)
            except OSError:
                pass


def parse_designs(spec):
    """'a.sv:top1;b.sv:top2' -> [(path, top), ...]."""
    designs = []
    for chunk in spec.split(";"):
        chunk = chunk.strip()
        if not chunk:
            continue
        if ":" in chunk:
            path, top = chunk.rsplit(":", 1)
        else:
            path, top = chunk, os.path.splitext(os.path.basename(chunk))[0]
        designs.append((path, top.strip()))
    return designs


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True, help="path to the opencdc binary")
    ap.add_argument("--rtl", help="single RTL file to check")
    ap.add_argument("--top", help="top module for --rtl")
    ap.add_argument("--designs",
                    help="semicolon list 'file.sv:top;...' for batch parity")
    ap.add_argument("--cell-map", help="optional cell map passed to both runs")
    ap.add_argument("--emit", help="write the synthesized netlist here "
                                   "(single-design mode)")
    ap.add_argument("--yosys", default="yosys", help="yosys executable")
    ap.add_argument("--strict-counts", action="store_true",
                    help="also fail on per-rule count deltas (default: count "
                         "deltas from multi-bit bit-blasting are warnings; "
                         "enable after bus regrouping lands)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    designs = []
    if args.designs:
        designs = parse_designs(args.designs)
    elif args.rtl:
        if not args.top:
            ap.error("--rtl requires --top")
        designs = [(args.rtl, args.top)]
    else:
        ap.error("provide --rtl/--top or --designs")

    failures = 0
    for rtl, top in designs:
        try:
            ok, detail = check_design(args.binary, rtl, top,
                                      emit=args.emit if len(designs) == 1 else None,
                                      cell_map=args.cell_map,
                                      verbose=args.verbose,
                                      strict_counts=args.strict_counts)
        except Exception as e:  # noqa: BLE001 - report and continue the batch
            print(f"[FAIL] {top}: {e}", file=sys.stderr)
            failures += 1
            continue
        if ok:
            print(f"[ OK ] {top}: {detail['rtl_counts']} "
                  f"pairs={detail['rtl_pairs']}")
            for n in detail["notes"]:
                print(f"       note: {n}")
            if args.verbose:
                print(f"       netlist counts={detail['nl_counts']} "
                      f"pairs={detail['nl_pairs']}")
        else:
            failures += 1
            print(f"[FAIL] {top}", file=sys.stderr)
            for p in detail["problems"]:
                print(f"       {p}", file=sys.stderr)

    if failures:
        print(f"\n{failures}/{len(designs)} design(s) failed parity",
              file=sys.stderr)
        return 1
    print(f"\n{len(designs)} design(s) passed RTL/netlist parity")
    return 0


if __name__ == "__main__":
    sys.exit(main())
