#include <cstdio>
#include <string_view>

#include "foxie_embedded/fixed_string.hpp"

namespace {

int g_failures = 0;

void Check(const bool cond, const char* msg) {
    if (cond) {
        return;
    }

    std::printf("[FAIL] %s\n", msg);
    ++g_failures;
}

} // namespace

int main() {
    std::puts("[FIXED_STRING] start");

    fxe::FixedString<8> s;
    Check(s.Empty(), "starts empty");
    Check(s.Size() == 0, "size=0");

    Check(s.Append("abc"), "append fits");
    Check(s.Equals("abc"), "equals after append");

    Check(!s.Append("defgh"), "append overflow returns false");
    Check(s.Equals("abcdefg"), "overflow truncates deterministically");
    Check(s.Size() == 7, "size capped at capacity-1");

    s.Clear();
    s += 'a';
    s += "bc";
    Check(s == "abc", "operator+= builds string");
    Check("abc" == s, "symmetric operator== with const char*");

    const std::string_view v = "abc";
    Check(s == v, "operator== with string_view");
    Check(v == s, "symmetric operator== with string_view");

    s.Clear();
    Check(s == "", "clear resets to empty");
    Check(s == nullptr, "nullptr equals empty");

    fxe::FixedString<16> f;
    Check(f.Format("%s-%d", "x", 12), "format fits");
    Check(f == "x-12", "format content");

    fxe::FixedString<8> f8;
    f8 += f;
    Check(f8 == "x-12", "operator+= with other FixedString capacity");

    fxe::FixedString<4> tiny;
    Check(!tiny.Format("%s", "abcd"), "format overflow returns false");
    Check(tiny == "abc", "format overflow truncates");

    fxe::FixedString<4> a("abc");
    fxe::FixedString<8> b("abc");
    Check(a == b, "operator== across capacities");

    if (g_failures == 0) {
        std::puts("[FIXED_STRING] ok");
        return 0;
    }

    std::printf("[FIXED_STRING] failures=%d\n", g_failures);
    return 1;
}
