#ifndef OPENCDC_CELL_LIBERTY_H
#define OPENCDC_CELL_LIBERTY_H

#include <string>

#include "cell/cell_library.h"

namespace opencdc::cell {

// Liberty (.lib) subset import. Parses the cell-classification subset of a
// Synopsys Liberty library — enough to generate CellDefs automatically so
// users do not have to hand-author a YAML cell map for every PDK:
//
//   library (name) {
//     cell (NAME) {
//       pin (CK) { direction : input; clock : true; }
//       pin (D)  { direction : input; function : "D"; }
//       pin (Q)  { direction : output; function : "IQ"; }
//       ff (IQ, IQN) {
//         clocked_on : "CK";  next_state : "D";
//         clear : "!CDN";     clear_preset_var1 : 1;
//       }
//     }
//     cell (AND2_X1) { pin (Y) { direction : output; function : "A & B"; } ... }
//   }
//
// Mapping to CellDef:
//   - `ff` group  -> Dff / DffReset / DffSetReset: clocked_on pins the clock,
//     next_state pins the data, clear/preset pin the reset/set (a leading
//     '!'/'~' in the expression means active-low; Liberty clear/preset is
//     asynchronous by definition), output pin = the pin with direction output.
//   - `latch` group -> Latch (enable = gate, data_in, data_out).
//   - output pin with a `function` expression -> Comb (or Buffer/Inverter for
//     identity/inversion), with `logic_type_hint` derived from the operators
//     (&, |, ^, ?:, !, ~). Complex expressions leave the hint unset so the
//     adapter falls back to the pattern-name heuristic.
//   - cells with none of the above -> Comb (recognized but opaque; never
//     assumed safe by the analyzer).
//
// Unknown groups (timing, leakage_power, ...), vendor attributes, and
// `` `define ``-style directives are skipped, so real vendor libraries parse.
// Only classification data is consumed; nothing timing-related is extracted.
bool load_liberty_file(const std::string& path, CellLibrary& out, std::string* error = nullptr);
bool load_liberty_string(const std::string& content, CellLibrary& out,
                         std::string* error = nullptr);

}  // namespace opencdc::cell

#endif  // OPENCDC_CELL_LIBERTY_H
