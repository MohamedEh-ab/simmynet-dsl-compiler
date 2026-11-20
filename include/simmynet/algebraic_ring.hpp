// simmynet/algebraic_ring.hpp - AlgebraicTuple: an exact element of the Z-module  { c_base + c_pi * pi }.
//
// PURPOSE
//   A compile-time (frontend) value type. The DSL parser and the expression evaluator carry numbers
//   such as  pi/4,  3*pi/4,  2*pi - pi/2  as a coefficient pair {c_base, c_pi} instead of a rounded
//   double, so that angle algebra stays exact until the value is LOWERED to a double at the
//   boundary of the compiled graph (to_double()).  Nothing here runs on the runtime hot path.
//
// WHAT IS EXACT
//   +, -, negation, scalar multiplication and division by a scalar act on the two coefficients
//   directly, and == compares the coefficients, so e.g.   pi/4 + pi/4 == pi/2   is true by
//   construction, and the expression can even be checked at compile time (static_assert).
//   "Exact" means exact on the STORED COEFFICIENTS:
//     * for integer and dyadic-rational multiples of pi (pi/2, 3*pi/8, ...) the coefficient
//       arithmetic is exact in binary floating point, so the result is exact;
//     * for non-dyadic coefficients (pi/3 stores fl(1/3)) each coefficient operation rounds once,
//       exactly like a plain double would, so the tuple is then no better than a double.
//
// WHAT IS NOT REPRESENTABLE
//   The module has no product: (a + b*pi)(c + d*pi) needs pi^2, and cos/sin/sqrt of a tuple leaves
//   the module (cos(pi/4) = sqrt(2)/2 is not in span{1, pi}). There is deliberately no
//   tuple * tuple operator; the expression layer (AST.hpp, namespace exprops) lowers such operands
//   to double and continues with a plain number {x, 0}.
//
// LOWERING
//   to_double() = c_base + c_pi * pi.  A tuple with c_pi == 0 is returned unchanged (bit for bit,
//   including -0.0 / inf / NaN), so programs that never mention pi behave exactly as before.
//
// C++17, header-only, constexpr throughout (except equal_mod_2pi, which needs std::fmod).
#pragma once

#include <cmath>
#include <type_traits>

namespace simmynet::ring {

template <class T>
inline constexpr T kPiValue = static_cast<T>(3.14159265358979323846264338327950288L);

template <class T = double>
struct AlgebraicTuple {
    static_assert(std::is_floating_point_v<T>, "AlgebraicTuple<T>: T must be float, double or long double");

    T c_base{};  // coefficient of 1
    T c_pi{};    // coefficient of pi

    constexpr AlgebraicTuple() noexcept = default;
    // Implicit on purpose: a plain number is the tuple {x, 0}.
    constexpr AlgebraicTuple(T base) noexcept : c_base(base), c_pi(T(0)) {}  // NOLINT(google-explicit-constructor)
    constexpr AlgebraicTuple(T base, T pi_coeff) noexcept : c_base(base), c_pi(pi_coeff) {}

    static constexpr AlgebraicTuple pi() noexcept { return {T(0), T(1)}; }

    // True when there is no pi component (the tuple is an ordinary number).
    constexpr bool is_pure() const noexcept { return c_pi == T(0); }

    // ---- exact ring arithmetic ------------------------------------------------------------
    constexpr AlgebraicTuple operator-() const noexcept { return {-c_base, -c_pi}; }
    constexpr AlgebraicTuple operator+() const noexcept { return *this; }

    friend constexpr AlgebraicTuple operator+(const AlgebraicTuple& a, const AlgebraicTuple& b) noexcept {
        return {a.c_base + b.c_base, a.c_pi + b.c_pi};
    }
    friend constexpr AlgebraicTuple operator-(const AlgebraicTuple& a, const AlgebraicTuple& b) noexcept {
        return {a.c_base - b.c_base, a.c_pi - b.c_pi};
    }
    // scalar multiplication (the only product the module supports)
    friend constexpr AlgebraicTuple operator*(const AlgebraicTuple& a, T s) noexcept {
        return {a.c_base * s, a.c_pi * s};
    }
    friend constexpr AlgebraicTuple operator*(T s, const AlgebraicTuple& a) noexcept { return a * s; }
    // division by a scalar: exact for powers of two, one rounding per coefficient otherwise
    friend constexpr AlgebraicTuple operator/(const AlgebraicTuple& a, T s) noexcept {
        return {a.c_base / s, a.c_pi / s};
    }

    constexpr AlgebraicTuple& operator+=(const AlgebraicTuple& o) noexcept { return *this = *this + o; }
    constexpr AlgebraicTuple& operator-=(const AlgebraicTuple& o) noexcept { return *this = *this - o; }
    constexpr AlgebraicTuple& operator*=(T s) noexcept { return *this = *this * s; }
    constexpr AlgebraicTuple& operator/=(T s) noexcept { return *this = *this / s; }

    // Symbolic equality: compares the coefficients directly (no tolerance, no evaluation of pi).
    friend constexpr bool operator==(const AlgebraicTuple& a, const AlgebraicTuple& b) noexcept {
        return a.c_base == b.c_base && a.c_pi == b.c_pi;
    }
    friend constexpr bool operator!=(const AlgebraicTuple& a, const AlgebraicTuple& b) noexcept { return !(a == b); }

    // ---- boundary lowering ---------------------------------------------------------------
    constexpr T to_double() const noexcept { return c_pi == T(0) ? c_base : c_base + c_pi * kPiValue<T>; }
    constexpr T evaluate() const noexcept { return to_double(); }
};

// Are two ANGLES (radians) equal modulo 2*pi, exactly on their coefficients?
//   true  : c_base equal and (c_pi - c_pi') is an even integer, with the subtraction itself exact.
//   false : not provably equal (they may still be numerically equal, e.g. a base coefficient that is
//           secretly a multiple of pi); callers fall back to a numeric comparison.
template <class T>
inline bool equal_mod_2pi(const AlgebraicTuple<T>& a, const AlgebraicTuple<T>& b) noexcept {
    if (!(a.c_base == b.c_base)) return false;
    const T nb = -b.c_pi;
    const T s = a.c_pi + nb;                         // Knuth TwoSum: is the subtraction exact?
    const T bb = s - a.c_pi;
    const T err = (a.c_pi - (s - bb)) + (nb - bb);
    if (err != T(0) || !std::isfinite(s)) return false;
    return std::fmod(s, T(2)) == T(0);               // fmod is exact
}

}  // namespace simmynet::ring
