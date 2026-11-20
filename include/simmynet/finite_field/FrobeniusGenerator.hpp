// include/simmynet/finite_field/FrobeniusGenerator.hpp
//
// AST node for  E : y^2 = x^3 + a x + b  over F_q  and its lift to a 2x2
// Frobenius generator.
//
// ---------------------------------------------------------------------------
// Étale-cohomology dictionary (what the 2x2 buffer means)
// ---------------------------------------------------------------------------
// For a prime l != char(F_q), H^1_et(E_{Fbar_q}, Q_l) is 2-dimensional (it is
// the dual of the Tate module V_l(E)).  The q-power Frobenius pi_q acts on it
// by a 2x2 matrix over Q_l.  Facts used by the rest of this directory:
//
//   * Lefschetz fixed-point formula:   #E(F_{q^n}) = det(I - F^n)
//                                              = 1 - Tr(F^n) + q^n
//     (H^0 contributes 1, H^2 = Q_l(-1) contributes q^n, H^1 contributes -Tr).
//   * det(F) = q  (Frobenius multiplies the cup-product pairing
//     H^1 x H^1 -> H^2 = Q_l(-1) by q: the Weil pairing / Poincare duality).
//   * Tr(F) = a_q = q + 1 - #E(F_q) is an INTEGER independent of l.
//
// What this header does NOT claim:
//   * The actual Frobenius matrix is only defined up to GL_2(Q_l)-conjugacy.
//     We emit the companion matrix of x^2 - a_q x + q,
//
//                      F_q = [ 0  -q  ]
//                            [ 1  a_q ],
//
//     one representative of that class.  Its ENTRIES carry no meaning; its
//     conjugation-invariants (trace, determinant, characteristic polynomial,
//     traces of powers, hence the zeta function) are exactly the right ones.
//   * Where the trace a_q comes from (be precise about what is implemented):
//       - `with_trace(t)` / `try_with_trace(t)`: attach a trace established
//         elsewhere.  It is checked against the Hasse-Weil bound t^2 <= 4q
//         (exact 128-bit comparison, no sqrt, no floating point); a trace that
//         violates it cannot belong to any elliptic curve over F_q and is
//         REJECTED (try_with_trace -> nullopt; with_trace leaves the curve's
//         trace unresolved).  The bound is necessary, not sufficient: it does
//         not prove the attached value is THE trace of this curve.
//       - `count_points()` / `compute_trace()` / `with_computed_trace()` /
//         `legendre_trace_resolver`: an exact O(p) point count by summing
//         Legendre symbols,
//                 #E(F_p) = 1 + sum_{x in F_p} (1 + chi(x^3 + a x + b)),
//         a_p = p + 1 - #E(F_p).  Supported for primes 5 <= p <= 10^6
//         (kMaxLegendreModulus); primality and nonsingularity are verified.
//         Every intermediate is < 2^41, so nothing can overflow int64/uint64.
//         This is a brute-force-in-p counter, NOT Schoof's algorithm.
//       - `TraceResolver`: plug in anything else (a table, an external
//         Schoof/SEA implementation, a certificate) for larger q.
//         `schoof_trace_stub` is a placeholder that always declines; Schoof/SEA
//         is NOT implemented in this library.
//   * Preconditions: q is an odd PRIME, 3 < q <= 2^62.  Primality is the
//     caller's guarantee for valid()/with_trace (it is not re-tested there; the
//     Legendre counter does test it, since q <= 10^6); a, b are reduced mod q.
#ifndef SIMMYNET_FINITE_FIELD_FROBENIUS_GENERATOR_HPP
#define SIMMYNET_FINITE_FIELD_FROBENIUS_GENERATOR_HPP

#include <cstdint>
#include <functional>
#include <optional>
#include <variant>
#include <vector>

#include "simmynet/Mat2.hpp"

namespace simmynet::ff {

namespace detail {

// (a + b) mod q for a, b < q < 2^62; branch-free.
inline std::uint64_t addmod(std::uint64_t a, std::uint64_t b, std::uint64_t q) noexcept {
  const std::uint64_t s = a + b;
  return s - (q & (0 - static_cast<std::uint64_t>(s >= q)));
}

// (a * b) mod q by fixed-length (64 step) double-and-add: no 128-bit
// division needed, so it is portable.  Used only for the one-off
// nonsingularity test at AST construction time.
inline std::uint64_t mulmod(std::uint64_t a, std::uint64_t b, std::uint64_t q) noexcept {
  a %= q;
  std::uint64_t r = 0;
  for (int i = 63; i >= 0; --i) {
    r = addmod(r, r, q);
    r = addmod(r, a & (0 - ((b >> i) & 1u)), q);
  }
  return r;
}

// Trial-division primality test; only ever called with n <= kMaxLegendreModulus.
inline bool is_prime_small(std::uint64_t n) noexcept {
  if (n < 2) return false;
  if (n % 2 == 0) return n == 2;
  for (std::uint64_t d = 3; d * d <= n; d += 2)
    if (n % d == 0) return false;
  return true;
}

}  // namespace detail

// Largest prime modulus accepted by the exact O(p) Legendre point counter.
// 10^6 keeps every intermediate below 2^41 and the chi table at ~1 MB.
inline constexpr std::int64_t kMaxLegendreModulus = 1000000;

// Hasse-Weil bound for an elliptic curve over F_q:  |t| <= 2 sqrt(q)  <=>  t^2 <= 4q.
// Exact: t^2 and 4q are compared as 128-bit integers (4q can reach 2^64 when q = 2^62).
// False for q <= 0.
inline bool hasse_bound_holds(std::int64_t t, std::int64_t q) noexcept {
  if (q <= 0) return false;
  const std::uint64_t mag = t < 0 ? 0 - static_cast<std::uint64_t>(t) : static_cast<std::uint64_t>(t);
  const U128 t2 = mul64(mag, mag);
  const std::uint64_t uq = static_cast<std::uint64_t>(q);
  const U128 four_q{uq >> 62, uq << 2};
  return !gt(t2, four_q);
}

// #E(F_p) (the point at infinity included) for y^2 = x^3 + a x + b over the prime field F_p,
// by summing Legendre symbols:  #E = 1 + sum_x (1 + chi(x^3 + a x + b)).   O(p) time, O(p) bytes.
//
// nullopt if p is not a prime in [5, kMaxLegendreModulus] or the curve is singular mod p.
// All arithmetic is exact in uint64: with p <= 10^6, every product is < 2^40 and every sum of
// two products plus a residue is < 2^41.
inline std::optional<std::int64_t> legendre_count_points(std::int64_t a, std::int64_t b,
                                                         std::int64_t p) {
  if (p < 5 || p > kMaxLegendreModulus || !detail::is_prime_small(static_cast<std::uint64_t>(p)))
    return std::nullopt;
  const std::uint64_t P = static_cast<std::uint64_t>(p);
  const std::uint64_t A = static_cast<std::uint64_t>(((a % p) + p) % p);
  const std::uint64_t B = static_cast<std::uint64_t>(((b % p) + p) % p);

  // Nonsingular: 4 a^3 + 27 b^2 != 0 (mod p).
  const std::uint64_t disc = (4 * (A * A % P) % P * A + 27 * (B * B % P)) % P;
  if (disc == 0) return std::nullopt;

  // chi[r] = +1 (nonzero square), -1 (non-square), 0 (r == 0).
  std::vector<std::int8_t> chi(P, -1);
  chi[0] = 0;
  for (std::uint64_t y = 1; y <= (P - 1) / 2; ++y) chi[y * y % P] = 1;

  std::int64_t n = 1;  // the point at infinity
  for (std::uint64_t x = 0; x < P; ++x) {
    const std::uint64_t rhs = ((x * x % P) * x + A * x + B) % P;
    n += 1 + chi[rhs];  // 2 points if rhs is a square, 1 if rhs == 0, 0 otherwise
  }
  return n;
}

class EllipticCurveNode;

// Resolves the Frobenius trace a_q for a curve (Schoof / SEA, a lookup table,
// a certificate from an external tool, ...).
using TraceResolver = std::function<std::optional<std::int64_t>(const EllipticCurveNode&)>;

class EllipticCurveNode {
 public:
  static constexpr std::int64_t kMaxModulus = std::int64_t{1} << 62;

  EllipticCurveNode(std::int64_t a, std::int64_t b, std::int64_t q) noexcept
      : q_(q), a_(reduce(a, q)), b_(reduce(b, q)) {}

  std::int64_t a() const noexcept { return a_; }
  std::int64_t b() const noexcept { return b_; }
  std::int64_t q() const noexcept { return q_; }
  std::optional<std::int64_t> trace() const noexcept { return trace_; }

  // Attach a trace that has been established elsewhere, validated against the Hasse-Weil bound
  // t^2 <= 4q.  nullopt if it violates the bound (no elliptic curve over F_q has such a trace).
  // The bound is necessary, not sufficient: it cannot tell you that t is THE trace of this curve.
  std::optional<EllipticCurveNode> try_with_trace(std::int64_t a_q) const noexcept {
    if (!hasse_bound_holds(a_q, q_)) return std::nullopt;
    EllipticCurveNode c = *this;
    c.trace_ = a_q;
    return c;
  }

  // Same, but returns a curve either way: if a_q violates Hasse-Weil the trace is left
  // UNRESOLVED (so lift() yields nullopt) instead of attaching an impossible value.
  EllipticCurveNode with_trace(std::int64_t a_q) const noexcept {
    const std::optional<EllipticCurveNode> c = try_with_trace(a_q);
    if (c) return *c;
    EllipticCurveNode u = *this;
    u.trace_.reset();
    return u;
  }

  // Ask a resolver for the trace; unresolved if it declines OR returns a value that violates the
  // Hasse-Weil bound.
  EllipticCurveNode resolve(const TraceResolver& r) const {
    if (!r) return with_trace_unresolved();
    const std::optional<std::int64_t> t = r(*this);
    return t ? with_trace(*t) : with_trace_unresolved();
  }

  // Exact point count #E(F_q) by Legendre-symbol summation (see legendre_count_points):
  // O(q), only for prime q in [5, kMaxLegendreModulus]; nullopt otherwise or if singular.
  std::optional<std::int64_t> count_points() const {
    return legendre_count_points(a_, b_, q_);
  }

  // a_q = q + 1 - #E(F_q), computed from count_points().  Same domain; nullopt outside it.
  std::optional<std::int64_t> compute_trace() const {
    const std::optional<std::int64_t> n = count_points();
    if (!n) return std::nullopt;
    return q_ + 1 - *n;
  }

  // Copy of the curve with compute_trace() attached; unresolved if it is outside the domain.
  EllipticCurveNode with_computed_trace() const {
    const std::optional<std::int64_t> t = compute_trace();
    return t ? with_trace(*t) : with_trace_unresolved();
  }

  // Well-formedness: 3 < q <= 2^62, q odd, q % 3 != 0 (cheap necessary
  // conditions for a prime > 3) and 4a^3 + 27b^2 != 0 (mod q).
  bool valid() const noexcept {
    return q_ > 3 && q_ <= kMaxModulus && (q_ & 1) == 1 && (q_ % 3) != 0 && nonsingular();
  }

 private:
  static std::int64_t reduce(std::int64_t x, std::int64_t q) noexcept {
    return q > 0 ? ((x % q) + q) % q : 0;
  }

  EllipticCurveNode with_trace_unresolved() const noexcept {
    EllipticCurveNode u = *this;
    u.trace_.reset();
    return u;
  }

  bool nonsingular() const noexcept {
    const std::uint64_t q = static_cast<std::uint64_t>(q_);
    const std::uint64_t a = static_cast<std::uint64_t>(a_);
    const std::uint64_t b = static_cast<std::uint64_t>(b_);
    const std::uint64_t a3 = detail::mulmod(detail::mulmod(a, a, q), a, q);
    const std::uint64_t b2 = detail::mulmod(b, b, q);
    const std::uint64_t disc =
        detail::addmod(detail::mulmod(4, a3, q), detail::mulmod(27, b2, q), q);
    return disc != 0;
  }

  std::int64_t q_;
  std::int64_t a_;
  std::int64_t b_;
  std::optional<std::int64_t> trace_;
};

// Placeholder for a future Schoof/SEA implementation: ALWAYS declines (returns nullopt).
// Schoof/SEA is not implemented in this library; use legendre_trace_resolver for q <= 10^6, or
// supply your own TraceResolver.
inline std::optional<std::int64_t> schoof_trace_stub(const EllipticCurveNode&) noexcept {
  return std::nullopt;
}

// TraceResolver backed by the exact O(p) Legendre counter: curve.resolve(legendre_trace_resolver).
// Declines (nullopt) for q outside the counter's domain.
inline std::optional<std::int64_t> legendre_trace_resolver(const EllipticCurveNode& e) {
  return e.compute_trace();
}

// The lifted generator: the 2x2 buffer together with its field size.
struct FrobeniusNode {
  Mat2 F;
  std::int64_t q;
};

using FiniteFieldNode = std::variant<EllipticCurveNode, FrobeniusNode>;

class FrobeniusGenerator {
 public:
  // Curve -> companion-matrix Frobenius generator.  nullopt if the curve is
  // ill-formed or its trace has not been resolved.
  static std::optional<FrobeniusNode> lift(const EllipticCurveNode& e) noexcept {
    const std::optional<std::int64_t> t = e.trace();
    if (!e.valid() || !t.has_value()) return std::nullopt;
    const std::int64_t q = e.q();
    return FrobeniusNode{Mat2{{0, -q, 1, *t}}, q};
  }

  // Same lift, delivered as the engine's simmynet::Matrix2x2 (double).  nullopt
  // when the curve is not liftable OR an entry (-q, a_q) is not exactly
  // representable as a double (|entry| > 2^53); use the int64 Mat2 then.
  static std::optional<simmynet::Matrix2x2> lift_to_engine(const EllipticCurveNode& e) noexcept {
    const std::optional<FrobeniusNode> n = lift(e);
    if (!n) return std::nullopt;
    return toEngine(n->F);
  }

  // Variant entry point: a FrobeniusNode is already lifted.
  static std::optional<FrobeniusNode> lift(const FiniteFieldNode& n) {
    struct Visitor {
      std::optional<FrobeniusNode> operator()(const EllipticCurveNode& e) const noexcept {
        return FrobeniusGenerator::lift(e);
      }
      std::optional<FrobeniusNode> operator()(const FrobeniusNode& f) const noexcept { return f; }
    };
    return std::visit(Visitor{}, n);
  }
};

}  // namespace simmynet::ff

#endif  // SIMMYNET_FINITE_FIELD_FROBENIUS_GENERATOR_HPP
