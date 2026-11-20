// tests/finite_field_test.cpp
//
// Validation harness.  The brute-force point counters below are TEST-ONLY
// oracles (naive loops over F_p and F_{p^2}).  The library itself ships exactly
// one point counter: the exact O(p) Legendre-symbol counter behind
// EllipticCurveNode::count_points() (primes p <= 10^6; NOT Schoof), which section
// [5] cross-checks against these oracles.  They give independent ground truth for:
//   * a_q = q + 1 - #E(F_q)  ->  lift  ->  Hasse-Weil and functional equation
//   * #E(F_{p^2}) predicted by det(I - F^2)  vs  a direct count over F_{p^2}
//   * the collapsed zeta function vs the defining relation  k Z_k = sum N_j Z_{k-j}
//   * the corrected functional equation, and the failure of the "q T^2 Z(T)" form
//   * the library Legendre counter and the Hasse-Weil validation of attached traces
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "simmynet/finite_field/WeilProver.hpp"

using namespace simmynet::ff;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    ++g_checks;                                                              \
    if (!(cond)) {                                                           \
      ++g_fail;                                                              \
      std::printf("  FAILED: %s  (line %d)\n", #cond, __LINE__);             \
    }                                                                        \
  } while (0)

// ---- test-only oracles ------------------------------------------------------
static long long brute_count_Fp(long long a, long long b, long long p) {
  std::vector<int> sq(p, 0);
  for (long long y = 0; y < p; ++y) sq[y * y % p]++;
  long long n = 1;  // point at infinity
  for (long long x = 0; x < p; ++x) n += sq[((x * x % p) * x + a * x + b) % p];
  return n;
}

// Independent oracle #2: Euler's criterion chi(r) = r^((p-1)/2) mod p via square-and-multiply.
// Structurally different from the library's chi table and from brute_count_Fp's square tally.
static long long powmod_small(long long b, long long e, long long m) {
  long long r = 1;
  b %= m;
  while (e > 0) {
    if (e & 1) r = r * b % m;
    b = b * b % m;
    e >>= 1;
  }
  return r;
}
static long long euler_count_Fp(long long a, long long b, long long p) {
  long long n = 1;
  for (long long x = 0; x < p; ++x) {
    const long long rhs = ((x * x % p) * x + a * x + b) % p;
    if (rhs == 0) n += 1;
    else n += (powmod_small(rhs, (p - 1) / 2, p) == 1) ? 2 : 0;
  }
  return n;
}
static bool is_prime_ll(long long n) {
  if (n < 2) return false;
  for (long long d = 2; d * d <= n; ++d)
    if (n % d == 0) return false;
  return true;
}

static long long brute_count_Fp2(long long a, long long b, long long p) {
  long long nu = 2;  // quadratic non-residue
  for (;; ++nu) {
    bool is_sq = false;
    for (long long y = 0; y < p; ++y) is_sq |= (y * y % p == nu % p);
    if (!is_sq) break;
  }
  struct E2 { long long x0, x1; };
  auto mulE = [&](E2 u, E2 v) {
    return E2{(u.x0 * v.x0 + nu * (u.x1 * v.x1 % p)) % p, (u.x0 * v.x1 + u.x1 * v.x0) % p};
  };
  std::vector<int> sq(p * p, 0);
  for (long long y0 = 0; y0 < p; ++y0)
    for (long long y1 = 0; y1 < p; ++y1) {
      const E2 s = mulE({y0, y1}, {y0, y1});
      sq[s.x0 * p + s.x1]++;
    }
  long long n = 1;
  for (long long x0 = 0; x0 < p; ++x0)
    for (long long x1 = 0; x1 < p; ++x1) {
      const E2 x{x0, x1};
      const E2 x3 = mulE(mulE(x, x), x);
      n += sq[((x3.x0 + a * x0 + b) % p) * p + (x3.x1 + a * x1) % p];
    }
  return n;
}

// ---- exact rationals for the functional-equation demo -----------------------
using i128 = __int128;
static i128 gcd128(i128 a, i128 b) { if (a < 0) a = -a; if (b < 0) b = -b; while (b) { i128 t = a % b; a = b; b = t; } return a; }
struct Frac {
  i128 n, d;
  static Frac make(i128 n, i128 d) { if (d < 0) { n = -n; d = -d; } i128 g = gcd128(n, d); return {n / g, d / g}; }
};
static Frac operator+(Frac a, Frac b) { return Frac::make(a.n * b.d + b.n * a.d, a.d * b.d); }
static Frac operator*(Frac a, Frac b) { return Frac::make(a.n * b.n, a.d * b.d); }
static Frac operator/(Frac a, Frac b) { return Frac::make(a.n * b.d, a.d * b.n); }
static bool operator==(Frac a, Frac b) { return a.n == b.n && a.d == b.d; }
static Frac evalPoly(const Poly2& p, Frac u) { return Frac::make(p.c[0], 1) + Frac::make(p.c[1], 1) * u + Frac::make(p.c[2], 1) * u * u; }
static Frac evalZ(const ZetaNode& z, Frac u) { return evalPoly(z.num, u) / evalPoly(z.den, u); }

int main() {
  // ---- 1. Sweep: every nonsingular curve over small primes ----------------------
  std::printf("[1] sweep over all nonsingular curves y^2 = x^3 + ax + b over F_p\n");
  const long long primes[] = {5, 7, 11, 13, 17, 19, 23, 29, 31};
  long long curves = 0, supersingular = 0;
  for (long long p : primes) {
    for (long long a = 0; a < p; ++a)
      for (long long b = 0; b < p; ++b) {
        EllipticCurveNode e(a, b, p);
        if (!e.valid()) continue;
        ++curves;

        const long long N1 = brute_count_Fp(a, b, p);        // oracle
        const long long t = p + 1 - N1;
        const auto node = FrobeniusGenerator::lift(e.with_trace(t));
        CHECK(node.has_value());
        const Mat2& F = node->F;

        // Lefschetz at n = 1, trace, determinant, characteristic polynomial.
        CHECK(LefschetzTracePass::trace(F) == t);
        CHECK(LefschetzTracePass::determinant(F).value == p);
        const auto cp = LefschetzTracePass::characteristic_polynomial(F);
        CHECK((cp.value == Poly2{{1, -t, p}}) && !cp.overflow);
        CHECK(LefschetzTracePass::count_points(F, 1).value() == N1);

        // Both theorems.
        const WeilCertificate cert = WeilProver::certify(*node);
        CHECK(cert.hasse.verdict == Verdict::PROVED);
        CHECK(cert.functional_equation.verdict == Verdict::PROVED);
        supersingular += cert.hasse.delta_zero;

        // Zeta closed form.
        const auto z = ZetaFunctionPass::collapse(F, p);
        CHECK(z.has_value());
        CHECK((z->den == Poly2{{1, -(p + 1), p}}));

        // Series relation  k Z_k = sum_{j<=k} N_j Z_{k-j}  (T Z'/Z = sum N_j T^j),
        // with Z_k from long division of num/den and N_j from the Lefschetz pass.
        i128 Zk[7];
        Zk[0] = 1;
        for (int k = 1; k <= 6; ++k) {
          i128 v = (k <= 2) ? z->num.c[k] : 0;
          for (int i = 1; i <= 2 && i <= k; ++i) v -= (i128)z->den.c[i] * Zk[k - i];
          Zk[k] = v;
        }
        for (int k = 1; k <= 6; ++k) {
          i128 rhs = 0;
          for (int j = 1; j <= k; ++j) rhs += (i128)LefschetzTracePass::count_points(F, j).value() * Zk[k - j];
          CHECK((i128)k * Zk[k] == rhs);
        }
      }
  }
  std::printf("    %lld curves, %lld supersingular-with-q-square cases\n", curves, supersingular);

  // ---- 2. Extension fields: det(I - F^2) vs direct count over F_{p^2} ------------
  std::printf("[2] #E(F_{p^2}) from F^2 vs direct count over F_{p^2}\n");
  for (long long p : {5LL, 7LL, 11LL}) {
    for (long long a = 0; a < p; ++a)
      for (long long b = 0; b < p; ++b) {
        EllipticCurveNode e(a, b, p);
        if (!e.valid()) continue;
        const long long t = p + 1 - brute_count_Fp(a, b, p);
        const Mat2 F = FrobeniusGenerator::lift(e.with_trace(t))->F;
        CHECK(LefschetzTracePass::count_points(F, 2).value() == brute_count_Fp2(a, b, p));
        // Tr(F^2) = t^2 - 2q  (Newton identity)
        CHECK(LefschetzTracePass::trace_of_power(F, 2).value() == t * t - 2 * p);
      }
  }

  // ---- 3. Worked example: y^2 = x^3 + 2x + 3 over F_97 ---------------------------
  std::printf("[3] worked example y^2 = x^3 + 2x + 3 over F_97\n");
  {
    EllipticCurveNode e(2, 3, 97);
    const long long N1 = brute_count_Fp(2, 3, 97);
    const long long t = 97 + 1 - N1;
    const FrobeniusNode node = *FrobeniusGenerator::lift(e.with_trace(t));
    const WeilCertificate c = WeilProver::certify(node);
    const ZetaNode z = *ZetaFunctionPass::collapse(node.F, 97);
    std::printf("    #E(F_97)=%lld  a_q=%lld  F=[[%lld,%lld],[%lld,%lld]]\n", N1, t,
                (long long)node.F.m[0], (long long)node.F.m[1], (long long)node.F.m[2], (long long)node.F.m[3]);
    std::printf("    Delta = t^2-4q = %lld  ->  Hasse-Weil %s\n", t * t - 4 * 97, to_string(c.hasse.verdict));
    std::printf("    Functional equation: collapse=%d pairing=%d num=%d den=%d -> %s\n",
                c.functional_equation.inverse_collapse, c.functional_equation.weil_pairing,
                c.functional_equation.numerator_reflects, c.functional_equation.denominator_reflects,
                to_string(c.functional_equation.verdict));
    std::printf("    Z(T) = %s\n", to_string(z).c_str());
    for (int n : {1, 2, 3, 5}) {
      std::printf("    #E(F_97^%d) = %lld\n", n, (long long)LefschetzTracePass::count_points(node.F, n).value());
    }

    // Exact rational check: Z(1/(qT)) == Z(T), and the "q T^2 Z(T)" form is false.
    const Frac T0 = Frac::make(3, 1), u = Frac::make(1, 97 * 3);
    const Frac lhs = evalZ(z, u), rhs = evalZ(z, T0);
    const Frac literal = Frac::make(97, 1) * T0 * T0 * rhs;
    CHECK(lhs == rhs);
    CHECK(!(lhs == literal));
    std::printf("    Z(1/(qT)) == Z(T) at T=3: %s;  q T^2 Z(T) == Z(1/(qT)): %s\n",
                lhs == rhs ? "true" : "false", lhs == literal ? "true" : "false");
  }

  // ---- 4. Negative tests: the checkers must be able to say no --------------------
  std::printf("[4] negative tests\n");
  {
    // Trace violating Hasse: q = 5, t = 100.
    const Mat2 bad{{0, -5, 1, 100}};
    CHECK(WeilProver::prove_hasse_weil(bad, 5).verdict == Verdict::REFUTED);
    // t = 5, q = 5: Delta = 25 - 20 > 0.
    CHECK(WeilProver::prove_hasse_weil(Mat2{{0, -5, 1, 5}}, 5).verdict == Verdict::REFUTED);
    // Boundary t^2 = 4q is accepted (q = 4 is not prime; checks the comparison only, q must be > 3).
    CHECK(WeilProver::prove_hasse_weil(Mat2{{0, -4, 1, 4}}, 4).delta_zero);
    // det(F) != q: not a Frobenius, both theorems refuse.
    const Mat2 notfrob{{0, -7, 1, 2}};
    CHECK(WeilProver::prove_hasse_weil(notfrob, 5).verdict == Verdict::REFUTED);
    CHECK(WeilProver::prove_functional_equation(notfrob, 5).verdict == Verdict::REFUTED);
    // Unresolved trace -> no lift; singular curve -> invalid; stub resolver -> unresolved.
    CHECK(!FrobeniusGenerator::lift(EllipticCurveNode(2, 3, 97)).has_value());
    CHECK(!EllipticCurveNode(0, 0, 7).valid());
    CHECK(!FrobeniusGenerator::lift(EllipticCurveNode(2, 3, 97).resolve(schoof_trace_stub)).has_value());
    // Huge q near 2^62 with |t| near 2 sqrt(q): 128-bit comparison, no overflow.
    const std::int64_t bigq = (std::int64_t{1} << 62) - 57;   // value only used arithmetically
    const std::int64_t tmax = 2147483647LL * 2;               // ~2 * 2^31 ~ 2 sqrt(2^62)
    CHECK(WeilProver::prove_hasse_weil(Mat2{{0, -bigq, 1, tmax}}, bigq).delta_negative);
    CHECK(WeilProver::prove_hasse_weil(Mat2{{0, -bigq, 1, tmax + 100}}, bigq).verdict == Verdict::REFUTED);
    // Powers whose count leaves int64 are reported, not wrapped.
    const Mat2 F97 = FrobeniusGenerator::lift(EllipticCurveNode(2, 3, 97).with_trace(-2))->F;
    CHECK(!LefschetzTracePass::count_points(F97, 40).has_value());
    CHECK(!LefschetzTracePass::count_points(F97, 0).has_value());
    CHECK(LefschetzTracePass::count_points(F97, 9).has_value());
  }

  // ---- 5. Exact O(p) Legendre counter + Hasse-Weil-validated traces ---------------
  std::printf("[5] Legendre point counter and Hasse-Weil validation of traces\n");
  {
    // 3a. count_points / compute_trace agree with the brute-force oracle on EVERY curve
    //     over every prime 5 <= p <= 101; singular curves are declined.
    long long curves = 0, singular = 0;
    for (long long p = 5; p <= 101; ++p) {
      if (!is_prime_ll(p)) continue;
      for (long long a = 0; a < p; ++a)
        for (long long b = 0; b < p; ++b) {
          EllipticCurveNode e(a, b, p);
          if (!e.valid()) {
            CHECK(!e.count_points().has_value());
            CHECK(!e.compute_trace().has_value());
            ++singular;
            continue;
          }
          ++curves;
          const long long N = brute_count_Fp(a, b, p);
          const long long tr = p + 1 - N;
          CHECK(e.count_points().value() == N);
          CHECK(e.compute_trace().value() == tr);
          CHECK(hasse_bound_holds(tr, p));
          const EllipticCurveNode c = e.with_computed_trace();
          CHECK(c.trace().has_value() && *c.trace() == tr);
          CHECK(e.resolve(legendre_trace_resolver).trace() == c.trace());
          const auto node = FrobeniusGenerator::lift(c);
          CHECK(node.has_value());
          CHECK(WeilProver::certify(*node).hasse.verdict == Verdict::PROVED);
          CHECK(LefschetzTracePass::count_points(node->F, 1).value() == N);   // Lefschetz n = 1 round-trip
        }
    }
    std::printf("    %lld curves counted, %lld singular declined\n", curves, singular);

    // 3b. Known value: y^2 = x^3 + 2x + 3 over F_97 has 100 points, trace -2.
    CHECK(EllipticCurveNode(2, 3, 97).count_points().value() == 100);
    CHECK(EllipticCurveNode(2, 3, 97).compute_trace().value() == -2);
    // a, b are reduced mod p, negatives included.
    CHECK(EllipticCurveNode(-1, -1, 97).count_points() == EllipticCurveNode(96, 96, 97).count_points());

    // 3c. Largest prime below 10^6 against the independent Euler-criterion oracle, plus closed forms.
    const long long P = 999983;  // prime, P % 4 == 3, P % 3 == 2
    CHECK(is_prime_ll(P) && P % 4 == 3 && P % 3 == 2 && P <= kMaxLegendreModulus);
    for (auto ab : {std::pair<long long, long long>{1, 1}, {2, 3}, {-7, 10}, {123456, 654321}}) {
      EllipticCurveNode e(ab.first, ab.second, P);
      const long long N = euler_count_Fp(e.a(), e.b(), P);
      CHECK(e.count_points().value() == N);
      CHECK(e.compute_trace().value() == P + 1 - N);
      CHECK(hasse_bound_holds(*e.compute_trace(), P));
    }
    CHECK(EllipticCurveNode(1, 0, P).count_points().value() == P + 1);   // y^2 = x^3 + x, p = 3 (mod 4): supersingular
    CHECK(EllipticCurveNode(1, 0, P).compute_trace().value() == 0);
    CHECK(EllipticCurveNode(0, 1, P).count_points().value() == P + 1);   // y^2 = x^3 + 1, p = 2 (mod 3): supersingular

    // 3d. Domain: composite / too small / too large / singular are declined, never mis-counted.
    CHECK(!EllipticCurveNode(1, 1, 91).count_points().has_value());            // 7 * 13
    CHECK(!EllipticCurveNode(1, 1, 999999).count_points().has_value());        // composite, <= 10^6
    CHECK(!EllipticCurveNode(1, 1, 4).count_points().has_value());
    CHECK(!EllipticCurveNode(1, 1, 3).count_points().has_value());
    CHECK(!EllipticCurveNode(1, 1, 2).count_points().has_value());
    CHECK(!EllipticCurveNode(1, 1, 1000003).count_points().has_value());       // prime but > kMaxLegendreModulus
    CHECK(!EllipticCurveNode(1, 1, (std::int64_t{1} << 62) - 57).count_points().has_value());
    CHECK(!EllipticCurveNode(0, 0, 1009).count_points().has_value());          // singular
    CHECK(!legendre_trace_resolver(EllipticCurveNode(1, 1, 1000003)).has_value());
    CHECK(!EllipticCurveNode(1, 1, 1000003).with_computed_trace().trace().has_value());

    // 3e. Hasse-Weil bound t^2 <= 4q: exact, exhaustive near the boundary for q = 97 (|t| <= 19).
    for (long long tr = -100; tr <= 100; ++tr)
      CHECK(hasse_bound_holds(tr, 97) == (tr * tr <= 4 * 97));
    CHECK(hasse_bound_holds(4, 4) && !hasse_bound_holds(5, 4));   // t^2 == 4q accepted (comparison only)
    CHECK(!hasse_bound_holds(0, 0) && !hasse_bound_holds(0, -5));
    const std::int64_t bigq = (std::int64_t{1} << 62) - 57;
    CHECK(hasse_bound_holds(2147483647LL * 2, bigq) && hasse_bound_holds(-2147483647LL * 2, bigq));
    CHECK(!hasse_bound_holds(2147483647LL * 2 + 100, bigq));
    CHECK(!hasse_bound_holds(std::numeric_limits<std::int64_t>::min(), bigq));   // |INT64_MIN| must not overflow
    CHECK(!hasse_bound_holds(std::numeric_limits<std::int64_t>::max(), bigq));
    CHECK(hasse_bound_holds(std::int64_t{1} << 32, std::int64_t{1} << 62));      // t^2 == 4q at q = 2^62: 4q = 2^64

    // 3f. with_trace / try_with_trace / resolve reject impossible traces.
    const EllipticCurveNode E97(2, 3, 97);
    CHECK(E97.try_with_trace(-2).has_value() && *E97.try_with_trace(-2)->trace() == -2);
    CHECK(E97.try_with_trace(19).has_value() && E97.try_with_trace(-19).has_value());
    CHECK(!E97.try_with_trace(20).has_value() && !E97.try_with_trace(-20).has_value());
    CHECK(!E97.try_with_trace(std::numeric_limits<std::int64_t>::min()).has_value());
    CHECK(E97.with_trace(-2).trace() == std::optional<std::int64_t>(-2));
    CHECK(!E97.with_trace(20).trace().has_value());                              // rejected -> unresolved
    CHECK(!FrobeniusGenerator::lift(E97.with_trace(20)).has_value());            // so it can never be lifted
    CHECK(!FrobeniusGenerator::lift(E97.with_trace(-2).with_trace(1000)).has_value());   // a bad retry clears the old trace
    CHECK(E97.resolve([](const EllipticCurveNode&) { return std::optional<std::int64_t>(-2); }).trace().has_value());
    CHECK(!E97.resolve([](const EllipticCurveNode&) { return std::optional<std::int64_t>(1000); }).trace().has_value());
    CHECK(!E97.resolve([](const EllipticCurveNode&) { return std::optional<std::int64_t>{}; }).trace().has_value());
    CHECK(!E97.resolve(TraceResolver{}).trace().has_value());                    // empty std::function
    CHECK(legendre_trace_resolver(E97) == std::optional<std::int64_t>(-2));
  }

  std::printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail == 0 ? 0 : 1;
}
