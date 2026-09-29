#include "utils/full_precision.h"

#include <charconv>
#include <cmath>
#include <ios>
#include <locale>
#include <string>

std::string shortest_double(double value)
{
  if (std::isnan(value)) return std::signbit(value) ? "-nan" : "nan";
  if (std::isinf(value)) return value < 0 ? "-inf" : "inf";
  char buf[64];
  const auto result = std::to_chars(buf, buf + sizeof(buf), value);
  return {buf, result.ptr};
}

namespace {

thread_local bool full_precision = false;

class ShortestNumPut : public std::num_put<char>
{
protected:
  iter_type do_put(iter_type out, std::ios_base& str, char_type fill, double value) const override
  {
    if (!full_precision || (str.flags() & std::ios_base::floatfield) != 0 || str.width() > 0) {
      return std::num_put<char>::do_put(out, str, fill, value);
    }
    const auto text = shortest_double(value);
    for (char c : text) *out++ = c;
    return out;
  }

  iter_type do_put(iter_type out, std::ios_base& str, char_type fill, long double value) const override
  {
    if (!full_precision || (str.flags() & std::ios_base::floatfield) != 0 || str.width() > 0) {
      return std::num_put<char>::do_put(out, str, fill, value);
    }
    const auto text = shortest_double(static_cast<double>(value));
    for (char c : text) *out++ = c;
    return out;
  }
};

}  // namespace

void install_full_precision_facet()
{
  std::locale::global(std::locale(std::locale(), new ShortestNumPut));
}

FullPrecisionScope::FullPrecisionScope() : previous(full_precision)
{
  full_precision = true;
}

FullPrecisionScope::~FullPrecisionScope()
{
  full_precision = previous;
}
