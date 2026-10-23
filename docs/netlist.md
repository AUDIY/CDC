# Gate-Level Netlist Analysis

OpenCDC can analyze **gate-level netlists** (post-synthesis `.v`) in addition to
RTL. The netlist frontend is a separate, purpose-built structural parser that
builds the same `ir::Graph` the RTL frontend produces, so every rule, report
format, waiver, and config option works identically — only the way the graph is
built differs.

- RTL input is parsed with [slang](https://github.com/MikePopoloski/slang).
- Netlist input uses a purpose-built Verilog-2001 structural lexer/parser (no
  slang), optimized for large flat netlists.

## Quick start

```bash
# Auto-detect the input type (RTL vs. netlist) and check.
opencdc check synthesized.v --top top

# Force the netlist frontend and supply a cell map.
opencdc check synthesized.v --top top --input-type netlist --cell-map cells.yaml

# Raise the graph safety limits for a very large design.
opencdc check synthesized.v --top top --input-type netlist --max-nodes 5000000 --max-edges 10000000
```

The report includes `"input_type": "netlist"` in JSON and prints
`Input type: netlist` in text output.

## Input type detection (`--input-type`)

`--input-type` accepts `auto` (default), `rtl`, or `netlist`.

Under `auto`, detection is **conservative in favor of RTL**:

1. If any readable file contains a behavioral construct (`always`, `always_ff`,
   `always_comb`, `initial`, `function`, `class`, `generate`, ...) — matched on
   word boundaries against comment-stripped text — the input is analyzed as
   **RTL**.
2. Otherwise the file must contain at least one structural marker (`module`,
   `input`, `output`, `wire`, `assign`) to be classified as **netlist**. This
   guards against calling an empty/garbled file a netlist.
3. Unreadable/empty input falls back to `rtl` (the historical default).

This guarantees existing RTL workflows behave exactly as before: a real RTL file
always routes to slang, even if it also happens to contain `assign` statements.
Force `--input-type netlist` if auto-detection misclassifies an unusual file.

## Cell maps

A cell map is a YAML file that tells the netlist frontend how to classify the
standard-cell (or macro) instances in a netlist. It is passed with
`--cell-map <file>` (or `input.cell_map` in config).

```yaml
cells:
  - pattern: "DFCNQ_*"
    kind: dff_reset
    clock: CK
    data: D
    output: Q
    reset: CDN
    reset_active_low: true
    reset_async: true

  - pattern: "CLKGATE*"
    kind: icg
    clock: GCK
    enable: EN

  - pattern: "MY_MACRO_*"
    kind: comb          # opaque combinational: not assumed safe
```

### Fields

| Key | Required | Meaning |
|-----|----------|---------|
| `pattern` | yes | Glob matched against the cell type name. **Longest match wins**, so a specific entry shadows a broad default (or another map entry). |
| `kind` | yes | One of `dff`, `dff_reset`, `dff_enable`, `dff_set_reset`, `latch`, `icg`, `buffer`, `inverter`, `comb`. |
| `clock`, `data`, `output`, `enable`, `reset`, `set` | no | Explicit pin names for each role. When omitted, pins are inferred from common names (`CK`/`CLK`/`C`, `D`/`DI`, `Q`, `E`/`EN`/`CE`, `R`/`RST`/`CDN`/`CLR`, ...). |
| `reset_active_low` | no | Reset polarity (`true` = active-low). Default `false`. |
| `reset_async` | no | `true` if the reset is asynchronous. Default `false`. |

### Built-in defaults

`CellLibrary::with_defaults()` ships broad, vendor-agnostic heuristics so many
netlists work with no cell map at all:

- `DFF*`, `SDFF*`, `DF*N*`, `DF*P*`, ... → flops (with/without reset/enable)
- `DLATCH*` → latches
- `CLKGATE*`, `ICG*` → integrated clock gates
- `AND*`, `OR*`, `XOR*`, `NAND*`, `NOR*`, `XNOR*`, `MUX*`, `INV*`, `BUF*`, ...
  → combinational primitives. Verilog gate primitives are recognized directly
  too: `and`/`or`/`not`/`nand`/`nor`/`xor`/`xnor`/`buf`/`bufif0`/`bufif1`/
  `notif0`/`notif1`/`pullup`/`pulldown` instantiations.
- Yosys generic gate-level cells (`$_DFF_*`, `$_DFFE_*`, `$_DFF_P?0_`,
  `$_AND_*`, `$_MUX_*`, `$_NOT_*`, `$_BUF_*`, ...) so post-Yosys output
  classifies out of the box.

Because lookup is longest-match-wins, a cell map can refine any default without
duplicating it.

### Unknown cells are never assumed safe

If a cell type matches no entry, it is classified **`Unknown`**: modeled as an
opaque combinational node and reported in a frontend warning:

```
netlist: unmapped cell type(s) treated as opaque combinational (add entries to
the cell map to classify them): MY_IP_X1
```

An unknown cell is **never** silently treated as a safe crossing (consistent
with `require_structural_proof`). Add a `comb`/`icg`/`dff_*` entry to classify
it precisely.

## What is modeled

The netlist frontend maps instances and nets onto the shared IR:

| Netlist element | IR node |
|-----------------|---------|
| `dff`/`dff_reset`/`dff_enable`/`dff_set_reset` cell | `Register` (width 1 — netlists are bit-blasted), with `clock_domain`, `root_clock`, `reset_signal`/polarity/async, `clock_is_gated`/`clock_is_muxed` |
| `latch` cell | `Register` with `is_latch = true` |
| `icg` cell | `And(clock, enable)` feeding the gated net, so clock resolution flags it as gated |
| `comb` / gate primitive | `Combinational` node (`And`/`Or`/`Xor`/`Mux`/...) |
| `buffer` / `inverter` cell | `Combinational` with `LogicType::Buf` / `Not` |
| net (incl. `assign a = b`) | `Net`; simple aliases are collapsed at build time; per-bit selects resolve to per-bit nodes so cross-bit connectivity is never fabricated |
| port | `Port` |

Clocks are resolved with the **same `ClockResolver` pass the RTL frontend
uses**, so root-clock, gated-clock, and muxed-clock detection behave
identically.

### Rules that work on netlists

CDC001, CDC003, CDC004, CDC006, CDC008, CDC009, CDC011, CDC012, CDC013 — these
are structural (register/graph topology) and are detected from the netlist IR
directly.

### What degrades vs. RTL

Semantic pattern rules rely on behavioral source that does not survive
synthesis. This is a documented degradation, compensated by structural
detection where possible:

- **Gray-code / handshake / async-FIFO pattern recognition** (CDC002 family) is
  mostly silent after synthesis: the XOR/encoding structure is flattened. The
  structural synchronizer detection still reports whether a destination is
  synchronized.
- **Multi-bit crossings are bit-blasted**: one RTL register `bus[7:0]` becomes
  8 one-bit flops, so CDC001 reports 8 narrow crossings instead of 1 wide one
  (and CDC002 grouping is lost) until bus regrouping lands. The set of crossed
  clock-domain pairs is identical. See the parity harness below, which treats
  this granularity difference explicitly.
- **Reset modeling is coarser** than RTL (polarity/async come from the cell map
  or defaults, not from always-block sensitivity).

## Graph safety limits

Netlists are far larger than RTL, so the netlist frontend raises the safety
limits to **5,000,000 nodes / 10,000,000 edges** (RTL keeps 500k / 1M).
Exceeding them truncates the graph and reports an error rather than OOM-ing.

Override per run:

```bash
--max-nodes 8000000 --max-edges 16000000
```

or in config:

```yaml
input:
  type: netlist
  cell_map: cells.yaml
  max_nodes: 8000000
  max_edges: 16000000
```

## Configuration

```yaml
input:
  type: auto          # auto | rtl | netlist
  cell_map: cells.yaml
  max_nodes: 5000000
  max_edges: 10000000
```

All other config sections (rules, waivers, constraints, output, ...) apply
unchanged to netlist input.

## RTL ↔ netlist parity harness

`benchmarks/scripts/synth_netlist.py` validates the netlist frontend against
the RTL frontend on the same design:

1. synthesize the RTL with **Yosys** into a generic gate-level netlist,
2. run `opencdc check` on the RTL (`--input-type rtl`) and on the netlist
   (`--input-type netlist`),
3. compare the two analyses.

```bash
# Single design
python3 benchmarks/scripts/synth_netlist.py --binary build/src/opencdc \
  --rtl design.sv --top design

# Batch
python3 benchmarks/scripts/synth_netlist.py --binary build/src/opencdc \
  --designs 'a.sv:top_a;b.sv:top_b'

# Also fail on per-rule count deltas (bit-blast granularity), not just
# domain-pair mismatches
python3 benchmarks/scripts/synth_netlist.py --binary build/src/opencdc \
  --strict-counts --rtl design.sv --top design
```

**Pass/fail** is **domain-pair set equality**: every clock-domain pair crossed
in RTL must also be crossed in the netlist and vice versa — a missed or
fabricated crossing fails the check. Per-rule **count** deltas are warnings by
default (multi-bit bit-blasting legitimately reports N narrow crossings for one
wide RTL register); `--strict-counts` escalates them to failures once bus
regrouping lands. Reset-centric rules (CDC007/CDC009) are excluded from the
comparison because netlist reset modeling is intentionally coarser.

## Supported Verilog subset

The netlist lexer/parser accepts the structural Verilog-2001 subset that
post-synthesis output uses:

- `module` / `endmodule`, ANSI and non-ANSI port declarations
- `wire` / `reg` / `inout` declarations, including `[msb:lsb]` ranges
- Cell and gate-primitive instantiations (named port connections)
- `assign` statements (constants, concat, bit-selects, simple aliases)
- Escaped identifiers (`\foo.bar `), attributes (`(* ... *)`), preprocessor
  `` ` `` directives (skipped)
- Numbers (`8'hff`, `4'b1010`, sized/unsized)

Behavioral constructs are out of scope (they are RTL and go to slang).

## Performance

Measured on a synthetic 600k-cell Yosys-style netlist (~55 MB), single thread:

- parse + build: **~7 MB/s**
- full run: **~4 min**, dominated by crossing enumeration (not parsing);
  ~100k cells ≈ 40 s
- `ir::compact_graph()` (netlist-only compaction of buffer/pass-through
  structure) keeps findings byte-identical and lowers peak memory

See [`docs/dev/NETLIST_PLAN.md`](dev/NETLIST_PLAN.md) for the full plan and
measurements.

## Known limitations

- **Multi-bit bus regrouping** is not yet done: bit-blasted buses report per-bit
  crossings (correct domain pairs, finer granularity). Tracked for a future
  phase; the parity harness exposes it as a count warning.
- **Liberty (`.lib`) import** is not implemented; use a YAML cell map.
- **RTL→netlist name mapping** for applying RTL-authored waivers to netlist
  runs is deferred; use regex/wildcard waivers in the interim.
- **Multi-module hierarchical netlists** are parsed as a single top module;
  deep hierarchy elaboration is a stretch goal.
