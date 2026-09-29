#pragma once

// Forgent3D: numbers at full precision in text exports.
//
// Every node prints itself with a plain ostream (`stream << x`, or STR(), which reuses one thread_local
// stream), whose default precision is 6 significant digits: 100.0005 comes out as 100, 2·cos(45°) as
// 1.41421. A consumer that rebuilds the geometry from the .csg gets a slightly different part — a block
// that touches a sphere comes out sunk 0.001 mm into it.
//
// install_full_precision_facet() (once, first thing in main, before any stream exists) puts a num_put
// into the global locale that, while a FullPrecisionScope is alive, prints every double in default
// float format at the shortest length that reads back to the same double (std::to_chars). Outside a
// scope, and for streams that ask for fixed/scientific or a width, it prints exactly as before.
//
// Only export text changes: the language's own number formatting (str(), echo) goes through
// double-conversion, not streams, and keeps OpenSCAD's 6 digits — that is semantics, not output.

#include <string>

void install_full_precision_facet();

class FullPrecisionScope
{
public:
  FullPrecisionScope();
  ~FullPrecisionScope();
  FullPrecisionScope(const FullPrecisionScope&) = delete;
  FullPrecisionScope& operator=(const FullPrecisionScope&) = delete;

private:
  bool previous;
};

/*! Shortest round-trip text of `value`: "0.1", "1e-07", "inf", "-inf", "nan". */
std::string shortest_double(double value);
