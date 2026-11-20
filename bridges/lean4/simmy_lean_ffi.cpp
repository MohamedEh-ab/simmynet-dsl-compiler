// bridges/lean4/simmy_lean_ffi.cpp - C ABI shared library for the Lean 4 `@[extern]` oracle bridge.
//
// Thin `extern "C"` wrappers over the exact finite-field engine (simmynet::ff, reached through
// <simmynet/Api.hpp>).  Lean declares each function in bridges/lean4/SimmyFFI.lean with
// `@[extern "simmy_..."]`; nothing here includes <lean/lean.h>.
//
// ABI rules (so Lean can call these with its unboxed scalar types)
//   * Arguments and results are only uint64_t / int64_t / double.  No pointers, no structs, no
//     Lean objects: Lean's `UInt64`, `Int64` and `Float` are passed in registers exactly like this.
//   * A function never throws across the C boundary (every body is wrapped in a catch-all).
//   * Every function is pure and deterministic: the result depends only on the arguments.
//   * Errors are reported in-band.  Functions returning int64_t return SIMMY_LEAN_ERR (INT64_MIN)
//     when the arguments are outside the supported domain; functions returning double return NaN.
//
// TRUST MODEL (read this before using a result in a proof)
//   Everything here is an UNTRUSTED ORACLE from Lean's point of view: Lean's kernel never sees
//   this code.  A value returned by an oracle is a candidate witness; it becomes a theorem only
//   after Lean re-checks a certificate for it (see SimmyFFI.lean, `TraceCertificate`).
//
// Domain notes
//   simmy_legendre_count_points : exact O(p) Legendre-symbol count, primes 5 <= p <= 10^6
//                                 (ff::legendre_count_points); NOT Schoof/SEA.
//   simmy_frobenius_trace_power : Tr(F^n) in overflow-checked int64 (ff::LefschetzTracePass).
//   simmy_extension_point_count : #E(F_{p^n}) = det(I - F^n), same overflow discipline.
//
// C++17, warning-free under -Wall -Wextra -Wpedantic.

#include <simmynet/Api.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#if defined(_WIN32)
#define SIMMY_LEAN_API extern "C" __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define SIMMY_LEAN_API extern "C" __attribute__((visibility("default")))
#else
#define SIMMY_LEAN_API extern "C"
#endif

namespace {

using std::int64_t;
using std::uint64_t;

constexpr int64_t kErr = std::numeric_limits<int64_t>::min();  // SIMMY_LEAN_ERR
constexpr uint64_t kAbiVersion = 1;
// simmy_weil_certify: result with this bit set means "arguments outside the supported domain".
constexpr uint64_t kCertifyDomainError = uint64_t{1} << 63;

// Curve y^2 = x^3 + a x + b over F_p with the trace resolved by the exact Legendre counter.
// nullopt outside the counter's domain (p not prime in [5, 10^6], or the curve is singular).
std::optional<simmynet::ff::FrobeniusNode> liftLegendre(int64_t a, int64_t b, int64_t p) {
    const simmynet::ff::EllipticCurveNode curve = simmynet::ff::EllipticCurveNode(a, b, p).with_computed_trace();
    return simmynet::ff::FrobeniusGenerator::lift(curve);
}

}  // namespace

// ---- identification --------------------------------------------------------------------------

// Version of this C ABI (bumped when a function is removed or changes meaning).  The argument is
// ignored; Lean needs one so that the extern is a function and not a constant.
SIMMY_LEAN_API uint64_t simmy_lean_abi_version(uint64_t /*unused*/) { return kAbiVersion; }

// SIMMYNET_VERSION_NUMBER (major * 10000 + minor * 100 + patch; 2.2.0 -> 20200).
SIMMY_LEAN_API uint64_t simmy_version_number(uint64_t /*unused*/) {
    return static_cast<uint64_t>(SIMMYNET_VERSION_NUMBER);
}

// ---- exact point counting --------------------------------------------------------------------

// #E(F_p) for y^2 = x^3 + a x + b (point at infinity included), or SIMMY_LEAN_ERR.
SIMMY_LEAN_API int64_t simmy_legendre_count_points(int64_t a, int64_t b, int64_t p) {
    try {
        const std::optional<int64_t> n = simmynet::ff::legendre_count_points(a, b, p);
        return n ? *n : kErr;
    } catch (...) {
        return kErr;
    }
}

// Frobenius trace a_p = p + 1 - #E(F_p), or SIMMY_LEAN_ERR.
SIMMY_LEAN_API int64_t simmy_frobenius_trace(int64_t a, int64_t b, int64_t p) {
    try {
        const std::optional<int64_t> t = simmynet::ff::EllipticCurveNode(a, b, p).compute_trace();
        return t ? *t : kErr;
    } catch (...) {
        return kErr;
    }
}

// Tr(F^n) = alpha^n + beta^n for the Frobenius of the curve (n >= 0), or SIMMY_LEAN_ERR when the
// curve is outside the Legendre domain or the value does not fit in int64.
SIMMY_LEAN_API int64_t simmy_frobenius_trace_power(int64_t a, int64_t b, int64_t p, uint64_t n) {
    try {
        const auto node = liftLegendre(a, b, p);
        if (!node) return kErr;
        const std::optional<int64_t> t = simmynet::ff::LefschetzTracePass::trace_of_power(node->F, n);
        return t ? *t : kErr;
    } catch (...) {
        return kErr;
    }
}

// #E(F_{p^n}) = det(I - F^n) for n >= 1, or SIMMY_LEAN_ERR (domain, n == 0 or int64 overflow).
SIMMY_LEAN_API int64_t simmy_extension_point_count(int64_t a, int64_t b, int64_t p, uint64_t n) {
    try {
        const auto node = liftLegendre(a, b, p);
        if (!node) return kErr;
        const std::optional<int64_t> c = simmynet::ff::LefschetzTracePass::count_points(node->F, n);
        return c ? *c : kErr;
    } catch (...) {
        return kErr;
    }
}

// ---- Hasse-Weil ------------------------------------------------------------------------------

// 1 iff t^2 <= 4q (exact 128-bit comparison, no sqrt), else 0; 0 for q <= 0.
SIMMY_LEAN_API uint64_t simmy_hasse_bound_holds(int64_t t, int64_t q) {
    return simmynet::ff::hasse_bound_holds(t, q) ? 1u : 0u;
}

// Slack to the Hasse bound, 2 sqrt(q) - |t|  (>= 0 exactly when the bound holds up to rounding).
// Informational (double); decide membership with simmy_hasse_bound_holds.  NaN for q <= 0.
SIMMY_LEAN_API double simmy_hasse_margin(int64_t t, int64_t q) {
    if (q <= 0) return std::numeric_limits<double>::quiet_NaN();
    return 2.0 * std::sqrt(static_cast<double>(q)) - std::fabs(static_cast<double>(t));
}

// Normalised trace t / (2 sqrt q) = cos(theta_q), the Sato-Tate angle; in [-1, 1] iff Hasse holds.
// NaN for q <= 0.
SIMMY_LEAN_API double simmy_sato_tate_cos(int64_t t, int64_t q) {
    if (q <= 0) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(t) / (2.0 * std::sqrt(static_cast<double>(q)));
}

// Full Weil certificate (ff::WeilProver::certify) of the curve y^2 = x^3 + a x + b over F_p as a
// bit mask:
//   bit 0  Hasse-Weil verdict is PROVED          bit 3  functional equation verdict is PROVED
//   bit 1  det(F) == q                           bit 4  weil pairing  F^T J F == q J
//   bit 2  Delta = t^2 - 4q < 0                  bit 5  an intermediate overflowed int64
// Bit 63 set means the curve is outside the Legendre domain (mask otherwise 0).
SIMMY_LEAN_API uint64_t simmy_weil_certify(int64_t a, int64_t b, int64_t p) {
    try {
        const auto node = liftLegendre(a, b, p);
        if (!node) return kCertifyDomainError;
        const simmynet::ff::WeilCertificate c = simmynet::ff::WeilProver::certify(*node);
        uint64_t m = 0;
        m |= (c.hasse.verdict == simmynet::ff::Verdict::PROVED ? 1u : 0u) << 0;
        m |= (c.hasse.det_is_q ? 1u : 0u) << 1;
        m |= (c.hasse.delta_negative ? 1u : 0u) << 2;
        m |= (c.functional_equation.verdict == simmynet::ff::Verdict::PROVED ? 1u : 0u) << 3;
        m |= (c.functional_equation.weil_pairing ? 1u : 0u) << 4;
        m |= ((c.hasse.overflow || c.functional_equation.overflow) ? 1u : 0u) << 5;
        return m;
    } catch (...) {
        return kCertifyDomainError;
    }
}
