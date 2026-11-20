// verified_rules_test.cpp - the generated LeanVerifiedRules.hpp: ingestion, hooks and negative controls.
//
// Everything that is a hook is compared with an INDEPENDENT reference implementation, and the evaluator
// is shown to reject a false rule (so that the static_asserts in the generated header are not vacuous).
#include <simmynet/verified/LeanVerifiedRules.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

using namespace simmynet::verified;

namespace {
int g_checks = 0, g_fail = 0;
void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

// ---- negative controls: deliberately FALSE rules must be rejected at compile time -------------
// x^2 = x in ZMod p  (false for p >= 3)
inline constexpr Node kBadNodes[] = {
    {Op::Var, Dom::Nat, 0, -1, 0, -1}, {Op::Var, Dom::ZMod, 1, -1, 0, 0},
    {Op::Lit, Dom::Nat, -1, -1, 2, -1}, {Op::Pow, Dom::ZMod, 1, 2, 0, 0}};
inline constexpr Param kBadParams[] = {{Dom::Nat, kHypPrime, -1}, {Dom::ZMod, 0, 0}};
inline constexpr Equation kBadEqs[] = {{3, 1}};
inline constexpr Rule kBad{"bad.rule", "Bad", "generic", TypeTag::FiniteField, kBadNodes, 4, kBadParams, 2, kBadEqs, 1};
static_assert(!check_rule<TypeTag::FiniteField>(kBad).ok(), "the evaluator must reject a false rule");
static_assert(check_rule<TypeTag::FiniteField>(kBad).failures > 0, "...by counting failures, not by being unsupported");
static_assert(check_rule<TypeTag::FiniteField>(kBad).unsupported == 0, "...and nothing is silently skipped");

// A tuple identity that is false: pi/4 + pi/4 == pi/3 (exactness: not equal as coefficients).
inline constexpr Node kBadTuple[] = {
    {Op::Pi, Dom::Real, -1, -1, 0, -1}, {Op::Lit, Dom::Real, -1, -1, 4, -1},  {Op::Div, Dom::Real, 0, 1, 0, -1},
    {Op::Add, Dom::Real, 2, 2, 0, -1},  {Op::Lit, Dom::Real, -1, -1, 3, -1},  {Op::Div, Dom::Real, 0, 4, 0, -1}};
inline constexpr Param kNoParam[1] = {{Dom::Nat, 0, -1}};
inline constexpr Equation kBadTupleEq[] = {{3, 5}};
inline constexpr Rule kBadT{"bad.tuple", "Bad", "generic", TypeTag::AlgebraicTuple, kBadTuple, 6, kNoParam, 0, kBadTupleEq, 1};
static_assert(check_rule<TypeTag::AlgebraicTuple>(kBadT).failures == 1, "false tuple identity is rejected");
}  // namespace

int main() {
    // ---- metadata / table ------------------------------------------------------------------
    check(kRuleCount == kRules.size(), "kRuleCount matches the table");
    check(kSourceSha256.size() == 64, "source hash is a SHA-256 hex digest");
    check(kSchema == "simmynet.lean-rules/1", "schema id");
    for (const Rule& r : kRules) {
        check(!r.lean_name.empty() && !r.module.empty(), "rule has name and module: " + std::string(r.lean_name));
        check(find_rule(r.lean_name) == &r, "find_rule round-trips: " + std::string(r.lean_name));
    }
    check(find_rule("No.Such.Theorem") == nullptr, "find_rule on an unknown name");
    for (std::size_t i = 1; i < kRules.size(); ++i)
        check(kRules[i - 1].lean_name < kRules[i].lean_name, "rules are sorted by Lean name");

    Report total;
    check(verify_all(&total), "verify_all: every rule agrees with the C++ evaluator");
    check(total.instances > 0 && total.failures == 0 && total.unsupported == 0, "verify_all report is clean");
    for (const Rule& r : kRules) check(check_rule_runtime(r).ok(), "rule ok at run time: " + std::string(r.lean_name));

    // ---- hook: Frobenius fold ---------------------------------------------------------------
#ifdef SIMMYNET_VERIFIED_HAS_FROBENIUS_FIXED
    static_assert(frobenius_fold<97>(12345) == 12345 % 97, "compile-time Frobenius fold");
    static_assert(is_prime_trial(97) && !is_prime_trial(91) && !is_prime_trial(1), "primality helper");
    {
        const std::uint64_t ps[] = {2, 3, 5, 7, 97, 1009};
        for (std::uint64_t p : ps)
            for (std::uint64_t x = 0; x < 3 * p; x += (p > 100 ? 13 : 1))
                check(powmod64(x, p, p) == x % p, "x^p == x (mod p) by square-and-multiply, p=" + std::to_string(p));
        check(frobenius_fold<2>(7) == frobenius_reference(7, 2, 2), "fold == reference (p=2)");
        check(frobenius_fold<1009>(123456) == frobenius_reference(123456, 1009, 1009), "fold == reference (p=1009)");
        // Hook at the top of the 64-bit range: Fermat for the Mersenne prime 2^61 - 1.
        const std::uint64_t m61 = (std::uint64_t{1} << 61) - 1;
        check(powmod64(123456789123456789ULL, m61, m61) == 123456789123456789ULL % m61, "powmod64 at 2^61-1");
    }
#endif

    // ---- hook: SO(2) angle fold -------------------------------------------------------------
#ifdef SIMMYNET_VERIFIED_HAS_SO2_ANGLE_ADD
    {
        using T = simmynet::ring::AlgebraicTuple<double>;
        constexpr T s = fold_phasor_angles(T::pi() / 4.0, T::pi() / 4.0);
        static_assert(s == T::pi() / 2.0, "pi/4 + pi/4 == pi/2 exactly");
        const double angles[] = {-3.0, -0.7, 0.0, 0.3, 1.0, 2.2};
        for (double a : angles)
            for (double b : angles) {
                // R(a) R(b) entrywise vs R(a + b), for R(t) = [[cos, -sin], [sin, cos]]
                const double c = std::cos(a) * std::cos(b) - std::sin(a) * std::sin(b);
                const double sn = std::sin(a) * std::cos(b) + std::cos(a) * std::sin(b);
                check(std::fabs(c - std::cos(a + b)) < 1e-12 && std::fabs(sn - std::sin(a + b)) < 1e-12,
                      "R(a)R(b) == R(a+b)");
                const T folded = fold_phasor_angles(T{a, 0.0}, T{b, 0.0});
                check(folded.to_double() == a + b, "fold adds the angles");
            }
    }
#endif

    // ---- hook: dihedral peephole, against an independent D_n implementation -----------------
#ifdef SIMMYNET_VERIFIED_HAS_DIHEDRAL_SRS_INV
    {
        // Independent model: element = (flip, k) meaning s^flip r^k, relation s r = r^-1 s.
        struct El { int flip; long k; };
        auto mul = [](El x, El y, long n) {
            // (s^a r^i)(s^b r^j) = s^(a+b) r^((-1)^b i + j), because r^i s = s r^-i
            const long i = y.flip ? -x.k : x.k;
            long kk = i + y.k;
            if (n > 0) kk = ((kk % n) + n) % n;
            return El{(x.flip + y.flip) % 2, kk};
        };
        auto gen = [](char c) {
            switch (c) {
                case 'r': return El{0, 1};
                case 'R': return El{0, -1};
                default: return El{1, 0};  // 's'
            }
        };
        std::uint64_t seed = 0x9E3779B97F4A7C15ULL;  // deterministic LCG
        auto next = [&seed]() { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return static_cast<unsigned>(seed >> 33); };
        for (long n = 0; n <= 12; ++n) {
            for (int trial = 0; trial < 300; ++trial) {
                char w[40];
                const std::size_t len = 1 + next() % 39;
                for (std::size_t i = 0; i < len; ++i) w[i] = "rsRs"[next() % 4];
                auto eval = [&](const char* s, std::size_t m) {
                    El acc{0, 0};
                    for (std::size_t i = 0; i < m; ++i) acc = mul(acc, gen(s[i]), n);
                    return acc;
                };
                const El before = eval(w, len);
                char red[40];
                for (std::size_t i = 0; i < len; ++i) red[i] = w[i];
                const std::size_t m = reduce_srs_to_rinv(red, len);
                const El after = eval(red, m);
                check(m <= len, "reduction never lengthens a word");
                check(before.flip == after.flip && before.k == after.k, "reduction preserves the group element, n=" + std::to_string(n));
            }
        }
        char w1[] = "srs";
        check(reduce_srs_to_rinv(w1, 3) == 1 && w1[0] == 'R', "s r s -> R");
        char w2[] = "rsrsr";  // contains s r s at positions 1..3 -> r R r
        check(reduce_srs_to_rinv(w2, 5) == 3 && w2[0] == 'r' && w2[1] == 'R' && w2[2] == 'r', "r s r s r -> r R r");
        char w3[] = "rrss";
        check(reduce_srs_to_rinv(w3, 4) == 4, "no pattern, no change");
    }
#endif

    std::printf("lean_verified_rules_test: %d checks, %d failures (%zu rules, %llu instances)\n", g_checks, g_fail,
                kRules.size(), static_cast<unsigned long long>(total.instances));
    return g_fail == 0 ? 0 : 1;
}
