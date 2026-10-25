# Netlist Frontend — Implementation Plan

Status: implemented — PR1–PR7 (MVP) complete with zero RTL regressions; PR8
adds multi-bit bus regrouping + structural gray-code recovery (netlist-only
post-passes). Liberty import, RTL→netlist name mapping, and hierarchical
elaboration remain open (see Phase 6 / Known limitations).
Goal: analyze gate-level (post-synthesis) netlists with the existing OpenCDC
pipeline by adding a second frontend behind the `ir::Graph` seam, without
changing RTL behavior.

## Why in-tree (decision record)

- `SlangAdapter` is the only producer of `ir::Graph`; `Analyzer::run_on_graph()`
  already runs every downstream stage (domains, 13 rules, waivers, baselines,
  SARIF/HTML/JSON, signoff, coverage) on any graph, and `run_incremental()`
  proves the pipeline works without re-elaboration.
- A parallel project would duplicate or re-link the whole rules/waivers/report
  engine and split rule semantics across two repos.
- One binary → one config, one waiver file, one report format for both RTL and
  post-synthesis signoff flows.

Non-goals for the MVP:
- Liberty (`.lib`) parsing (YAML cell map only).
- UPF/CPF low-power analysis.
- Multi-module hierarchical netlist elaboration (flat netlists first).
- RTL↔netlist name mapping (deferred to Phase 6; regex waivers as interim).

## Key architectural facts this plan relies on

| Fact | Location | Consequence for netlist path |
|---|---|---|
| `FrontendResult { ir::Graph, errors, warnings, ok }` is the frontend contract | `src/frontend/slang_adapter.h` | Netlist frontend returns the same struct; downstream unchanged |
| `Analyzer::run()` = config parse → frontend → `run_on_graph()` | `src/analysis/analyzer.cpp:18-57` | Only the frontend call site changes |
| `ir::Graph` nodes carry `clock_domain`, `root_clock`, `reset_signal`, `reset_pol`, `is_async_reset`, `logic_type` | `src/ir/graph.h` | Netlist cells map directly onto these fields |
| Clock resolver traces gated/muxed clocks through combinational graph edges | `src/clock/resolve.cpp` | Represent ICG cells as latch+AND structure → CDC004 works unchanged |
| `register_predecessors(traverse_combinational)` walks Net/Combinational nodes | `src/ir/graph.cpp` | Passthrough nets and gate nodes are traversed transparently |
| Synchronizer detection is structural (Register→Register, same `clock_domain`) | `src/cdc/synchronizer.cpp` | 2FF/3FF detection actually works *better* on netlists |
| Graph safety limits: 500k nodes / 1M edges | `src/ir/graph.h:90-91` | Netlists need raised limits + compaction (Phase 5) |
| Findings carry `ir::SourceLoc` (file/line) | `src/cdc/crossing.h:66` | Netlist nodes get file + instantiation line |
| yaml-cpp already linked; CLI parses flags in `src/main.cpp` | — | Cell map + `--input-type` reuse existing plumbing |

---

## Phase 0 — Frontend interface extraction (refactor only, no behavior change)

Estimated size: small. Every existing golden/unit test must stay green.

1. New `src/frontend/frontend.h`:
   - Move `FrontendOptions`, `FrontendResult` here (keep `#include` shims in
     `slang_adapter.h` so existing includes keep working).
   - `class IFrontend { virtual FrontendResult elaborate(files, top, options) = 0; virtual ~IFrontend() = default; };`
   - `enum class InputType { Auto, Rtl, Netlist };`
   - `InputType detect_input_type(const std::vector<std::string>& files);`
     Heuristic (documented, conservative): scan each file's first ~64 KB of
     comment-stripped text; `always`, `initial`, `function`, `task`,
     `property`, `interface`, `class`, `sequence` → RTL; only
     `module`/`wire`/`input`/`output`/`assign` + instantiations → Netlist;
     anything ambiguous → RTL (preserves current behavior).
2. `SlangAdapter : public IFrontend` (no logic changes).
3. `AnalysisRequest` (`src/analysis/analyzer.h`) gains:
   `InputType input_type = InputType::Rtl`, `std::string cell_map_path`,
   `size_t max_nodes = 0`, `size_t max_edges = 0` (0 = built-in defaults).
4. `Analyzer::run()` selects the frontend via a small factory
   (`make_frontend(InputType, ...)`) — netlist branch returns
   "netlist frontend not built" error until Phase 2 lands (behind
   `OPENCDC_ENABLE_NETLIST_FRONTEND`, default ON once Phase 2 merges).
5. `CheckOptions` (`src/opencdc/opencdc.h`) + `src/main.cpp`:
   `--input-type auto|rtl|netlist`, `--cell-map <file>`, `--max-nodes N`,
   `--max-edges N` + help text + the "unknown option" list.
6. `config::Config` (`src/config/config.h`): `input_type`, `cells` (list),
   `max_nodes`, `max_edges` + parser support in `config.cpp`.
   CLI overrides config, as with existing options.

Acceptance: `ctest` green, golden JSON byte-identical, new unit tests for
`detect_input_type` and config parsing.

## Phase 1 — Cell library

New: `src/cell/cell_library.h/cpp`, tests `tests/unit/cell_library_test.cpp`.

```cpp
enum class CellKind { Dff, DffReset, DffEnable, DffSetReset, Latch, Icg,
                      Buffer, Inverter, Comb, Unknown };
struct CellDef {
    std::string pattern;          // glob, e.g. "DFCNQ_*"
    CellKind kind;
    std::string clock_pin, data_pin, output_pin, enable_pin, reset_pin, set_pin;
    bool reset_active_low = false, reset_async = false;
};
class CellLibrary {
    const CellDef* lookup(const std::string& cell_type) const;  // glob, longest-match wins
    void add(CellDef);
    static CellLibrary with_defaults();
};
```

- `with_defaults()` ships *heuristic* generic patterns (clearly documented as
  overridable): `DFF*/DFXP*/SDFF*`, `DLATCH*`, `CLKGATE*/ICG*`, `BUFX*/CLKBUF*`,
  `INVD*`, `AND*/NAND*/OR*/NOR*/XOR*/XNOR*/MUX*`.
- YAML override (standalone file via `--cell-map` or `cells:` in opencdc.yaml;
  reuses yaml-cpp):

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
```

- Critical safety rule: unknown cell type → `CellKind::Unknown` → treated as
  opaque combinational with a frontend warning listing the unknown types.
  Never guess that a cell is safe (respects `require_structural_proof`).

## Phase 2 — Netlist parser + IR builder (the core)

New: `src/frontend/netlist/{netlist_lexer,netlist_parser,netlist_adapter}.{h,cpp}`.

### 2a. Lexer

- Verilog-2001/SV gate-level subset: identifiers, **escaped identifiers**
  (`\foo.bar `), `(* attributes *)` (skipped), sized literals (`1'bz`, `4'hF`),
  punctuation, comments (`//`, `/* */`).
- Line tracking for `SourceLoc`.

### 2b. Parser grammar subset

- `module name (ports);` — ANSI and non-ANSI port lists.
- Declarations: `input/output/inout/wire/reg/logic` with optional `[msb:lsb]`
  and comma lists (accept SV keywords since DC/ICC2/Genus write either).
- `assign lhs = rhs;` — rhs limited to identifier, bit/part-select, concat,
  literal. Anything complex → `Combinational` node, `LogicType::Unknown`,
  `logic_expression` = source text (fields already exist in `ir::Node`).
- Instantiations: `celltype [#(...)] inst ( .PIN(net), ... );` — named and
  positional ports, multiple instances per statement, hierarchical instance
  names (`u_a/u_b/genblk1/reg0`), parameters ignored via balanced-paren skip.
- `defparam`, `specify`, `timescale`, `celldefine` → skipped.
- `generate`/`always`/`initial` → frontend **warning** + block skip; if
  elaboration semantics would be silently wrong, fail with a clear error
  telling the user to flatten first (`--input-type rtl` is the fallback).

### 2c. IR mapping rules (normative table)

| Netlist construct | IR mapping |
|---|---|
| Dff / DffReset / DffEnable cell | `Register` node, `hier_name = <inst path>`, `clock_domain` = net on clock pin, `reset_signal`/`reset_pol`/`is_async_reset` from `CellDef` |
| Latch | `Register` node + new `Node::is_latch = true` (IR field added here, default false; RTL path never sets it) |
| Icg | Synthesize internal structure: latch-register on enable + `Combinational` And(clock, latched_en) → existing `ClockResolver` marks `clock_is_gated`; CDC004 unchanged |
| Buffer / Inverter on a **clock path** | `Combinational` with new `LogicType::Buf` / existing `Not` so root-clock tracing works |
| Buffer / Inverter off clock paths | Not materialized: net treated as passthrough (consumers connect to producers directly) — keeps graph size sane |
| Comb gates | `Combinational` + matching `LogicType` (And/Or/Xor/Not/Mux) |
| Unknown cells | `Combinational` + `LogicType::Unknown` + one aggregate frontend warning |
| Constant ties (`1'b0/1'b1/z`) on data pins | Pin left unconnected |
| Power/ground pins (`VPWR/VGND/VDD/VSS/VPB/VNB`) | Ignored silently |
| Assign of identifier/select/concat | `Net` nodes + edges (or passthrough collapse when single-fanin) |
| Every cell instantiation | `SourceLoc{netlist_file, line}` on produced nodes |

- Edges: Data from D-net producer → Register; `EdgeRole::Clock`/`EdgeRole::Reset`
  edges where the slang adapter currently creates them — follow existing
  conventions in `src/frontend/slang_adapter.cpp` so downstream code sees the
  same shape.
- Limits: honor `MAX_GRAPH_NODES/EDGES` overridable via `--max-nodes/
  --max-edges` (netlist defaults e.g. 5M / 10M); hitting the limit sets
  `truncated_` → CDC010 + `analysis_status=incomplete`, as today.
- Hierarchy: flat netlists (hierarchical *names* are fine). Multi-module
  netlists → Phase 6.

### 2d. Tests

- `tests/unit/netlist_parser_test.cpp`: lexer tokens, escaped ids, ANSI/non-ANSI
  ports, positional/named/multi-instance statements, unknown cells, power-pin
  stripping, assign/select/concat, limit truncation.
- Fixtures `tests/fixtures/netlist/*.v`: hand-written 2FF sync crossing,
  async-reset crossing, ICG-gated clock, multi-bit crossing, reconvergence —
  plus golden JSON via the existing golden harness.

## Phase 3 — Analyzer / report / rule-policy integration

1. `Analyzer::run()` netlist branch calls `NetlistAdapter` → `run_on_graph()`
   unchanged.
2. Report JSON gains `"input_type"`; `Finding.source_loc` points at the
   netlist file/line (reporters already handle this).
3. Rule support matrix at netlist level (documented in `docs/netlist.md`):

   | Rule | Netlist support |
   |---|---|
   | CDC001, 003, 004, 006, 008, 009, 011, 012, 013 | Full (structural) |
   | Sync-chain 2FF/3FF detection | Full — often *more* accurate than RTL |
   | CDC002 multi-bit | Full since PR8 — bus regrouping re-merges `<base>[i]` bit flops into wide registers (regardless of granularity) |
   | Gray pattern recognition (CDC002 suppression) | Full since PR8 for the canonical `x ^ (x>>1)` encoder (structural recovery); handshake/FIFO still degraded (names mangled by synthesis) |
   | CDC005 / CDC007 | Supported when cell map defines reset/enable pins; otherwise explicit frontend warning |

4. `docs/netlist.md` documents the mapping table, cell-map format, and the
   degradation matrix; `docs/architecture.md` module map updated; README
   section + CHANGELOG entry.

## Phase 4 — Parity & benchmark harness (yosys)

- `benchmarks/scripts/synth_netlist.py`: for each corpus design run
  `yosys -p "read_verilog -sv <files>; synth -top TOP; flatten; write_verilog -noattr netlist.v"`.
- Netlist benchmark entries in the manifest: expected results derived from the
  RTL run (compare by domain-pair/rule counts, tolerant of extra findings from
  flattened internal logic).
- Optional non-blocking CI job on a 4-design subset.
- Gives real-scale validation (10k–500k cells) without hand-writing netlists.

## Phase 5 — Performance & compaction

- `src/ir/compact.{h,cpp}`: post-frontend pass removing pure buffer/inverter
  chains and collapsing single-fanin/single-fanout passthrough nets; runs for
  netlist input only (RTL unaffected). **[done]** Implemented as
  `ir::compact_graph()`: drops `LogicType::Buf` cells and 1-driver/1-consumer
  pass-through nets, keeping ports, registers, and the entire backward cone of
  every clock-domain/reset anchor (so ClockResolver, gated detection, and reset
  tracing are byte-for-byte unchanged). Connectivity-preserving: replaced by
  direct edges. Inverters are intentionally kept (removing a lone inverter
  flips polarity unless even-chain parity is tracked). Driven by
  `Graph::remove_nodes()` (batch removal, node ids stable). Only the netlist
  frontend calls it.
- String interning arena for net names; `reserve()` on parser vectors;
  profile with `benchmarks/` on synthesized corpus. **[deferred — targets
  already met; revisit only if a real corpus misses them.]**
- Targets (adjust after first measurements): parse ≥ 1 MB/s; full run on a
  100k-cell design < 60 s single-thread.
  - **Measured** (synthetic yosys-style netlist, `$_DFF_`/`$_XOR_`/`$_BUF_`):
    - parse + build: **~7 MB/s** (55 MB / 600k cells in ~7.5 s) — target ≥1 MB/s met.
    - full run, 600k cells: **~4 min**, dominated by crossing enumeration
      (CDC001/CDC008), not parsing. Linear extrapolation to 100k cells:
      **~40 s** — target <60 s met.
    - `ir::compact_graph` on the same graph: findings byte-identical
      (CDC001=192000 / CDC008=96000 before and after), peak RSS down
      ~1.86 GB → ~1.69 GB; larger relative win on buffer-heavy real synthesis
      output (abc inserts many buffers/passthrough nets).
  - **Audit `--jobs` parallelism path in `cdc/crossing.cpp` with netlist-sized
    graphs; determinism test already exists (`thread_determinism_test.cpp`).
    [deferred alongside string-interning: perf targets already met, so the
    audit was not required for the MVP; the existing determinism suite passes.]**

## Phase 6 — Stretch (post-MVP)

Done (PR8):
- Multi-bit bus regrouping: `<base>[<bit>]` bit-flop groups re-merged into
  wide registers (`src/frontend/netlist/regroup.{h,cpp}`), run before clock
  resolution; per-bit nets cleaned by compaction. Restores CDC001 count and
  CDC002 bus-width parity with RTL.
- Structural gray-code recovery: canonical `x ^ (x>>1)` encoder shape
  re-recognized and re-annotated (`annotate_gray_registers`), restoring
  CDC002 gray verdict parity.

Done (PR9):
- Liberty (`.lib`) import (`src/cell/liberty.{h,cpp}`, `--liberty` +
  `input.liberty`): cell/pin/ff/latch classification subset → `CellDef`s
  with logic-type hints from output `function` expressions; precedence
  defaults < liberty < YAML cell map.

Done (PR10):
- `--name-map` RTL→netlist register mapping
  (`src/frontend/netlist/name_map.{h,cpp}`, `input.name_map`): netlist
  registers renamed to their RTL names after build and before gray
  recovery/regrouping, so RTL-authored waivers keep matching; stale
  entries and name collisions warn.

Remaining:
- Multi-module/hierarchical netlist elaboration (module defs + instance
  expansion with prefixing).
- Low-power cells (isolation/retention) as safe-crossing blackboxes.
- LSP handling for netlist files (diagnostics only).

## Risks & mitigations

| Risk | Mitigation |
|---|---|
| Vendor netlist dialect quirks (escaped ids, attributes, `(* keep *)`) | Lexer support + fixtures copied from real DC/ICC/Yosys output |
| Graph explosion on millions of cells | Passthrough collapsing at build time, Phase 5 compaction, raised/overridable limits, truncation → CDC010 |
| Wrong cell polarity/roles produce false "safe" verdicts | Unknown cells are never assumed safe; cell map is explicit; frontend warning lists unmapped cells |
| CDC002 bit-blast false positives | **Done (PR8):** bus regrouping + structural gray recovery; regex waiver guidance remains for unusual gray styles |
| RTL waivers don't match synthesized names | **Done (PR10):** `--name-map` RTL→netlist register mapping; regex/wildcard waivers remain the fallback for clock-domain names |
| Pattern rules (gray/handshake) silent post-synthesis | Documented degradation; structural sync detection compensates |
| Refactor regressions (Phase 0) | Pure-move refactor, golden tests must stay byte-identical |

## PR sequence (small, reviewable)

1. **PR1** — Phase 0: `IFrontend` extraction + `InputType` plumbing (no
   netlist code yet). Golden tests unchanged.
2. **PR2** — Phase 1: cell library + YAML + defaults + unit tests.
3. **PR3** — Phase 2a/2b: lexer + parser + unit tests on tiny netlists.
4. **PR4** — Phase 2c/2d: IR builder + mapping table + golden netlist tests +
   analyzer/CLI/config wiring.
5. **PR5** — Phase 4: yosys parity harness + netlist corpus entries.
6. **PR6** — Phase 5: compaction + limits + perf numbers in docs.
7. **PR7** — docs: `docs/netlist.md`, architecture, README, CHANGELOG.

## MVP definition of done

Phases 0–4 (PR1–PR5): `opencdc check synthesized.v --top top --input-type
netlist --cell-map cells.yaml` produces correct CDC001/003/004/006/008/009/
011/012/013 findings on the synthesized corpus, with parity (domain-pair
counts) against RTL runs, and zero regressions on the existing RTL suite.
