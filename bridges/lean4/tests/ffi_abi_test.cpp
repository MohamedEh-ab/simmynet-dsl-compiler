// ffi_abi_test.cpp - the Lean C ABI (simmy_lean_ffi.cpp) against the simmynet::ff functions it wraps.
//
// Declares the prototypes the way a Lean-compiled caller sees them (Lean passes Int64 as a 64-bit
// register value; int64_t and uint64_t are ABI-identical), links libsimmynet_lean and checks:
//   * documented values (y^2 = x^3 + 2x + 3 over F_97: #E = 100, trace -2),
//   * every (a, b, p) on a grid: C ABI == ff:: reference, trace recurrence, Hasse, Weil mask,
//   * domain errors are reported in-band (INT64_MIN / NaN / bit 63), never by exception.
#include <simmynet/Api.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

extern "C" {
std::uint64_t simmy_lean_abi_version(std::uint64_t);
std::uint64_t simmy_version_number(std::uint64_t);
std::int64_t simmy_legendre_count_points(std::int64_t, std::int64_t, std::int64_t);
std::int64_t simmy_frobenius_trace(std::int64_t, std::int64_t, std::int64_t);
std::int64_t simmy_frobenius_trace_power(std::int64_t, std::int64_t, std::int64_t, std::uint64_t);
std::int64_t simmy_extension_point_count(std::int64_t, std::int64_t, std::int64_t, std::uint64_t);
std::uint64_t simmy_hasse_bound_holds(std::int64_t, std::int64_t);
double simmy_hasse_margin(std::int64_t, std::int64_t);
double simmy_sato_tate_cos(std::int64_t, std::int64_t);
std::uint64_t simmy_weil_certify(std::int64_t, std::int64_t, std::int64_t);
}

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const char* what, long long a = 0, long long b = 0, long long p = 0) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s (a=%lld b=%lld p=%lld)\n", what, a, b, p);
    }
}
constexpr std::int64_t kErr = std::numeric_limits<std::int64_t>::min();
}  // namespace

int main() {
    namespace ff = simmynet::ff;

    check(simmy_lean_abi_version(0) == 1, "ABI version");
    check(simmy_version_number(0) == static_cast<std::uint64_t>(SIMMYNET_VERSION_NUMBER), "version number");

    // Documented values.
    check(simmy_legendre_count_points(2, 3, 97) == 100, "#E(F_97) == 100");
    check(simmy_frobenius_trace(2, 3, 97) == -2, "a_97 == -2");
    check(simmy_frobenius_trace_power(2, 3, 97, 2) == -190, "Tr(F^2) == t^2 - 2q");
    check(simmy_extension_point_count(2, 3, 97, 2) == 9600, "#E(F_97^2) == 9600");
    check(simmy_hasse_bound_holds(-2, 97) == 1 && simmy_hasse_bound_holds(20, 97) == 0, "Hasse bound");
    check(simmy_weil_certify(2, 3, 97) == 0x1F, "Weil mask for F_97 curve");

    // Grid: C ABI == library reference, plus algebraic identities that tie the functions together.
    const std::int64_t primes[] = {5, 7, 11, 13, 17, 97, 101};
    for (const std::int64_t p : primes) {
        for (std::int64_t a = 0; a < p; a += (p > 20 ? 7 : 1)) {
            for (std::int64_t b = 0; b < p; b += (p > 20 ? 5 : 1)) {
                const auto ref = ff::legendre_count_points(a, b, p);
                const std::int64_t n = simmy_legendre_count_points(a, b, p);
                if (!ref) {
                    check(n == kErr, "singular curve is an in-band error", a, b, p);
                    check(simmy_frobenius_trace(a, b, p) == kErr, "trace error on singular", a, b, p);
                    check((simmy_weil_certify(a, b, p) >> 63) == 1, "certify domain bit on singular", a, b, p);
                    continue;
                }
                check(n == *ref, "count == ff reference", a, b, p);
                const std::int64_t t = simmy_frobenius_trace(a, b, p);
                check(t == p + 1 - n, "trace == p + 1 - #E", a, b, p);
                check(simmy_hasse_bound_holds(t, p) == 1, "Hasse holds for a real curve", a, b, p);
                check(std::fabs(simmy_sato_tate_cos(t, p)) <= 1.0, "Sato-Tate cosine in [-1,1]", a, b, p);
                check(simmy_hasse_margin(t, p) >= -1e-9, "Hasse margin >= 0", a, b, p);
                check(simmy_weil_certify(a, b, p) == 0x1F || simmy_weil_certify(a, b, p) == 0x1B,
                      "Weil mask: Hasse+det+pairing+functional eq (bit 2 only when Delta<0)", a, b, p);
                // Recurrence  t_{n+1} = t * t_n - p * t_{n-1},  t_0 = 2, t_1 = t.
                std::int64_t prev = 2, cur = t;
                for (std::uint64_t k = 1; k <= 6; ++k) {
                    check(simmy_frobenius_trace_power(a, b, p, k) == cur, "Tr(F^k) recurrence", a, b, p);
                    check(simmy_extension_point_count(a, b, p, k) ==
                              static_cast<std::int64_t>(std::llround(std::pow(static_cast<double>(p), static_cast<double>(k)))) + 1 - cur,
                          "#E(F_{p^k}) == p^k + 1 - Tr(F^k)", a, b, p);
                    const std::int64_t next = t * cur - p * prev;
                    prev = cur;
                    cur = next;
                }
            }
        }
    }

    // Domain errors: in-band, never thrown.
    check(simmy_legendre_count_points(2, 3, 100) == kErr, "composite modulus");
    check(simmy_legendre_count_points(2, 3, 3) == kErr, "p < 5");
    check(simmy_legendre_count_points(2, 3, 1000003) == kErr, "p > 10^6");
    check(simmy_legendre_count_points(0, 0, 7) == kErr, "singular curve");
    check(simmy_extension_point_count(2, 3, 97, 0) == kErr, "n == 0");
    check(simmy_frobenius_trace_power(2, 3, 97, 4000000000ULL) == kErr, "int64 overflow reported");
    check(std::isnan(simmy_hasse_margin(1, 0)) && std::isnan(simmy_sato_tate_cos(1, -5)), "NaN for q <= 0");
    check(simmy_hasse_bound_holds(1, 0) == 0, "Hasse false for q <= 0");
    check((simmy_weil_certify(2, 3, 100) >> 63) == 1, "certify: domain bit");
    check(simmy_hasse_bound_holds(std::numeric_limits<std::int64_t>::min(), std::int64_t{1} << 62) == 0,
          "INT64_MIN trace handled exactly");

    std::printf("lean_ffi_abi_test: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail == 0 ? 0 : 1;
}
