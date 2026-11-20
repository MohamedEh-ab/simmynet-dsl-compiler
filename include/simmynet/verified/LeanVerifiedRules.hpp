// GENERATED FILE - DO NOT EDIT.
//
// Generator : bridges/lean4/lean_to_cpp.py   (regenerate with: python bridges/lean4/lean_to_cpp.py)
// Input     : rules.json, schema simmynet.lean-rules/1, Lean 4.36.0
// SHA-256   : 638897cc61b223a3504fed6008cb03c5b20d28806c6038113a81ab14e8799628   (of the canonical JSON: sorted keys, no whitespace)
// Rules     : 4
//
// Every rule below is a Lean 4 theorem (tagged @[simmy_rule], proof free of sorry, accepted by the Lean
// KERNEL), lowered
// to a flat constexpr expression pool.  This header only INGESTS them: the evaluator is
// simmynet/verified/RuleKit.hpp, the proofs live in Lean.  The static_asserts at the end of the file
// pin each rule's name, module and concrete type tag, and (for every tag whose arithmetic is constexpr)
// re-check the rule on a deterministic grid of instances, so a drift between Lean's semantics and the
// C++ evaluator is a compile error rather than a silent miscompile.
//
// Header-only, C++17, warning-free under -Wall -Wextra -Wpedantic, ASCII only.
#ifndef SIMMYNET_VERIFIED_LEAN_VERIFIED_RULES_HPP
#define SIMMYNET_VERIFIED_LEAN_VERIFIED_RULES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "simmynet/algebraic_ring.hpp"
#include "simmynet/verified/RuleKit.hpp"

// Feature macros: defined exactly when the corresponding rule (and hence rewrite hook) is present.
#define SIMMYNET_VERIFIED_HAS_DIHEDRAL_SRS_INV 1
#define SIMMYNET_VERIFIED_HAS_FROBENIUS_FIXED 1
#define SIMMYNET_VERIFIED_HAS_SO2_ANGLE_ADD 1

namespace simmynet::verified {

inline constexpr std::string_view kSchema = "simmynet.lean-rules/1";
inline constexpr std::string_view kLeanVersion = "4.36.0";
inline constexpr std::string_view kSourceSha256 = "638897cc61b223a3504fed6008cb03c5b20d28806c6038113a81ab14e8799628";
inline constexpr std::size_t kRuleCount = 4;

namespace generated {

// ==================================================================================================
// [0] SimmyBridge.Examples.dihedral_srs_inv
//   Lean theorem : SimmyBridge.Examples.dihedral_srs_inv
//   Lean module  : SimmyBridge.Examples
//   Type tag     : dihedral
//   Rule kind    : dihedral_srs_inv
//   Statement    : \u2200 (n : \u2115), DihedralGroup.sr 0 * DihedralGroup.r 1 * DihedralGroup.sr 0 = (DihedralGroup.r 1)\u207b\xb9
//   Axioms       : Quot.sound, propext
// ==================================================================================================
inline constexpr Node kNodes0[] = {
    {Op::Var, Dom::Nat, 0, -1, 0, -1},  // 0: var n
    {Op::Lit, Dom::ZMod, -1, -1, 0, 0},  // 1: lit 0 [mod node 0]
    {Op::DihSr, Dom::Dihedral, 1, -1, 0, 0},  // 2: dih_sr(1) [mod node 0]
    {Op::Lit, Dom::ZMod, -1, -1, 1, 0},  // 3: lit 1 [mod node 0]
    {Op::DihR, Dom::Dihedral, 3, -1, 0, 0},  // 4: dih_r(3) [mod node 0]
    {Op::Mul, Dom::Dihedral, 2, 4, 0, 0},  // 5: mul(2, 4) [mod node 0]
    {Op::Mul, Dom::Dihedral, 5, 2, 0, 0},  // 6: mul(5, 2) [mod node 0]
    {Op::Inv, Dom::Dihedral, 4, -1, 0, 0},  // 7: inv(4) [mod node 0]
};
inline constexpr Param kParams0[] = {
    {Dom::Nat, 0, -1},  // n
};
inline constexpr Equation kEqs0[] = {
    {6, 7},
};

// ==================================================================================================
// [1] SimmyBridge.Examples.frobenius_fixed
//   Lean theorem : SimmyBridge.Examples.frobenius_fixed
//   Lean module  : SimmyBridge.Examples
//   Type tag     : finite_field
//   Rule kind    : frobenius_fixed
//   Statement    : \u2200 (p : \u2115) [inst : Fact (Nat.Prime p)] (x : ZMod p), x ^ p = x
//   Axioms       : Classical.choice, Quot.sound, propext
// ==================================================================================================
inline constexpr Node kNodes1[] = {
    {Op::Var, Dom::Nat, 0, -1, 0, -1},  // 0: var p
    {Op::Var, Dom::ZMod, 1, -1, 0, 0},  // 1: var x [mod node 0]
    {Op::Pow, Dom::ZMod, 1, 0, 0, 0},  // 2: pow(1, 0) [mod node 0]
};
inline constexpr Param kParams1[] = {
    {Dom::Nat, 1, -1},  // p
    {Dom::ZMod, 0, 0},  // x
};
inline constexpr Equation kEqs1[] = {
    {2, 1},
};

// ==================================================================================================
// [2] SimmyBridge.Examples.quarter_turns
//   Lean theorem : SimmyBridge.Examples.quarter_turns
//   Lean module  : SimmyBridge.Examples
//   Type tag     : algebraic_tuple
//   Rule kind    : generic
//   Statement    : Real.pi / 4 + Real.pi / 4 = Real.pi / 2
//   Axioms       : Classical.choice, Quot.sound, propext
// ==================================================================================================
inline constexpr Node kNodes2[] = {
    {Op::Pi, Dom::Real, -1, -1, 0, -1},  // 0: pi
    {Op::Lit, Dom::Real, -1, -1, 4, -1},  // 1: lit 4
    {Op::Div, Dom::Real, 0, 1, 0, -1},  // 2: div(0, 1)
    {Op::Add, Dom::Real, 2, 2, 0, -1},  // 3: add(2, 2)
    {Op::Lit, Dom::Real, -1, -1, 2, -1},  // 4: lit 2
    {Op::Div, Dom::Real, 0, 4, 0, -1},  // 5: div(0, 4)
};
inline constexpr Param kParams2[1] = {{Dom::Nat, 0, -1}};  // placeholder: the rule has no parameters
inline constexpr Equation kEqs2[] = {
    {3, 5},
};

// ==================================================================================================
// [3] SimmyBridge.Examples.so2_angle_add
//   Lean theorem : SimmyBridge.Examples.so2_angle_add
//   Lean module  : SimmyBridge.Examples
//   Type tag     : so2_phasor
//   Rule kind    : so2_angle_add
//   Statement    : \u2200 (a b : \u211d), Real.cos (a + b) = Real.cos a * Real.cos b - Real.sin a * Real.sin b \u2227 Real.sin (a + b) = Real.sin a * Real.cos b + Real.cos a * Real.sin b
//   Axioms       : Classical.choice, Quot.sound, propext
// ==================================================================================================
inline constexpr Node kNodes3[] = {
    {Op::Var, Dom::Real, 0, -1, 0, -1},  // 0: var a
    {Op::Var, Dom::Real, 1, -1, 0, -1},  // 1: var b
    {Op::Add, Dom::Real, 0, 1, 0, -1},  // 2: add(0, 1)
    {Op::Cos, Dom::Real, 2, -1, 0, -1},  // 3: cos(2)
    {Op::Cos, Dom::Real, 0, -1, 0, -1},  // 4: cos(0)
    {Op::Cos, Dom::Real, 1, -1, 0, -1},  // 5: cos(1)
    {Op::Mul, Dom::Real, 4, 5, 0, -1},  // 6: mul(4, 5)
    {Op::Sin, Dom::Real, 0, -1, 0, -1},  // 7: sin(0)
    {Op::Sin, Dom::Real, 1, -1, 0, -1},  // 8: sin(1)
    {Op::Mul, Dom::Real, 7, 8, 0, -1},  // 9: mul(7, 8)
    {Op::Sub, Dom::Real, 6, 9, 0, -1},  // 10: sub(6, 9)
    {Op::Sin, Dom::Real, 2, -1, 0, -1},  // 11: sin(2)
    {Op::Mul, Dom::Real, 7, 5, 0, -1},  // 12: mul(7, 5)
    {Op::Mul, Dom::Real, 4, 8, 0, -1},  // 13: mul(4, 8)
    {Op::Add, Dom::Real, 12, 13, 0, -1},  // 14: add(12, 13)
};
inline constexpr Param kParams3[] = {
    {Dom::Real, 0, -1},  // a
    {Dom::Real, 0, -1},  // b
};
inline constexpr Equation kEqs3[] = {
    {3, 10},
    {11, 14},
};

}  // namespace generated

// Rules sorted by Lean theorem name.
inline constexpr std::array<Rule, 4> kRules = {{
    {"SimmyBridge.Examples.dihedral_srs_inv", "SimmyBridge.Examples", "dihedral_srs_inv", TypeTag::Dihedral,
     generated::kNodes0, 8, generated::kParams0, 1, generated::kEqs0, 1},
    {"SimmyBridge.Examples.frobenius_fixed", "SimmyBridge.Examples", "frobenius_fixed", TypeTag::FiniteField,
     generated::kNodes1, 3, generated::kParams1, 2, generated::kEqs1, 1},
    {"SimmyBridge.Examples.quarter_turns", "SimmyBridge.Examples", "generic", TypeTag::AlgebraicTuple,
     generated::kNodes2, 6, generated::kParams2, 0, generated::kEqs2, 1},
    {"SimmyBridge.Examples.so2_angle_add", "SimmyBridge.Examples", "so2_angle_add", TypeTag::So2Phasor,
     generated::kNodes3, 15, generated::kParams3, 2, generated::kEqs3, 2},
}};

// Look a rule up by its fully qualified Lean name; nullptr if absent.
constexpr const Rule* find_rule(std::string_view lean_name) noexcept {
    for (const Rule& r : kRules)
        if (r.lean_name == lean_name) return &r;
    return nullptr;
}

// Re-check every rule on its instance grid (including the So2Phasor rules, which need std::cos / std::sin
// and so cannot be static_asserted).  Returns true when every rule passes; fills `total` if given.
inline bool verify_all(Report* total = nullptr) noexcept {
    Report sum;
    bool all_ok = true;
    for (const Rule& r : kRules) {
        const Report rep = check_rule_runtime(r);
        sum.instances += rep.instances;
        sum.failures += rep.failures;
        sum.unsupported += rep.unsupported;
        all_ok = all_ok && rep.ok();
    }
    if (total != nullptr) *total = sum;
    return all_ok;
}

// ==================================================================================================
// Rewrite hooks.  Generated only for rules that are present above; each is justified by the named
// Lean theorem and by nothing else.
// ==================================================================================================

// ---- hook: SimmyBridge.Examples.dihedral_srs_inv (dihedral_srs_inv) ------------------------------------------------------
// Lean:  forall n, sr 0 * r 1 * sr 0 = (r 1)^-1   in DihedralGroup n
// Peephole rewrite on words over {'r' = r 1, 'R' = (r 1)^-1, 's' = sr 0}:  s r s  ->  R.
// Sound in every DihedralGroup n (a = b implies u a v = u b v); NOT claimed confluent or complete.
// In place, left to right; returns the new length.
constexpr std::size_t reduce_srs_to_rinv(char* w, std::size_t len) noexcept {
    std::size_t top = 0;
    for (std::size_t i = 0; i < len; ++i) {
        w[top++] = w[i];
        if (top >= 3 && w[top - 1] == 's' && w[top - 2] == 'r' && w[top - 3] == 's') {
            top -= 3;
            w[top++] = 'R';
        }
    }
    return top;
}

// ---- hook: SimmyBridge.Examples.frobenius_fixed (frobenius_fixed) ------------------------------------------------------------
// Lean:  forall p [Fact p.Prime] (x : ZMod p), x ^ p = x
// Frobenius fold: x^p (mod p) rewrites to x (mod p).  The hypothesis `p prime` is discharged HERE,
// at compile time, by trial division, so an instantiation with a composite P does not compile.
constexpr bool is_prime_trial(std::uint64_t n) noexcept {
    if (n < 2) return false;
    for (std::uint64_t d = 2; d <= n / d; ++d)
        if (n % d == 0) return false;
    return true;
}
template <std::uint64_t P>
constexpr std::uint64_t frobenius_fold(std::uint64_t x) noexcept {
    static_assert(is_prime_trial(P), "SimmyBridge.Examples.frobenius_fixed: the rewrite x^p = x needs a prime modulus P");
    return x % P;
}
// Reference (un-rewritten) semantics, for cross-checks: x^e mod p by square-and-multiply.
constexpr std::uint64_t frobenius_reference(std::uint64_t x, std::uint64_t e, std::uint64_t p) noexcept {
    return powmod64(x, e, p);
}

// ---- hook: SimmyBridge.Examples.so2_angle_add (so2_angle_add) -------------------------------------------------------------
// Lean:  forall a b : Real, cos (a + b) = cos a * cos b - sin a * sin b  AND
//                           sin (a + b) = sin a * cos b + cos a * sin b
// i.e. R(a) R(b) = R(a + b) for the SO(2) rotation matrices R.  Folding two phasor generators
// therefore adds their angles, and with AlgebraicTuple the sum stays exact (pi/4 + pi/4 == pi/2).
template <class T>
constexpr ring::AlgebraicTuple<T> fold_phasor_angles(const ring::AlgebraicTuple<T>& a,
                                                     const ring::AlgebraicTuple<T>& b) noexcept {
    return a + b;
}

// ==================================================================================================
// Invariants: Lean theorem name, module and concrete type tag are part of the type system of this header.
// ==================================================================================================
// [0] SimmyBridge.Examples.dihedral_srs_inv : dihedral  (module SimmyBridge.Examples)
static_assert(kRules[0].lean_name == "SimmyBridge.Examples.dihedral_srs_inv", "Lean theorem name");
static_assert(kRules[0].module == "SimmyBridge.Examples", "Lean module path");
static_assert(kRules[0].tag == TypeTag::Dihedral, "SimmyBridge.Examples.dihedral_srs_inv: concrete type tag dihedral");
static_assert(kRules[0].n_eqs > 0 && kRules[0].n_nodes > 0, "rule has a non-empty expression pool");
static_assert(check_rule<TypeTag::Dihedral>(kRules[0]).ok(),
              "SimmyBridge.Examples.dihedral_srs_inv: C++ evaluation disagrees with the Lean theorem on a checked instance");
// [1] SimmyBridge.Examples.frobenius_fixed : finite_field  (module SimmyBridge.Examples)
static_assert(kRules[1].lean_name == "SimmyBridge.Examples.frobenius_fixed", "Lean theorem name");
static_assert(kRules[1].module == "SimmyBridge.Examples", "Lean module path");
static_assert(kRules[1].tag == TypeTag::FiniteField, "SimmyBridge.Examples.frobenius_fixed: concrete type tag finite_field");
static_assert(kRules[1].n_eqs > 0 && kRules[1].n_nodes > 0, "rule has a non-empty expression pool");
static_assert(check_rule<TypeTag::FiniteField>(kRules[1]).ok(),
              "SimmyBridge.Examples.frobenius_fixed: C++ evaluation disagrees with the Lean theorem on a checked instance");
// [2] SimmyBridge.Examples.quarter_turns : algebraic_tuple  (module SimmyBridge.Examples)
static_assert(kRules[2].lean_name == "SimmyBridge.Examples.quarter_turns", "Lean theorem name");
static_assert(kRules[2].module == "SimmyBridge.Examples", "Lean module path");
static_assert(kRules[2].tag == TypeTag::AlgebraicTuple, "SimmyBridge.Examples.quarter_turns: concrete type tag algebraic_tuple");
static_assert(kRules[2].n_eqs > 0 && kRules[2].n_nodes > 0, "rule has a non-empty expression pool");
static_assert(check_rule<TypeTag::AlgebraicTuple>(kRules[2]).ok(),
              "SimmyBridge.Examples.quarter_turns: C++ evaluation disagrees with the Lean theorem on a checked instance");
// [3] SimmyBridge.Examples.so2_angle_add : so2_phasor  (module SimmyBridge.Examples)
static_assert(kRules[3].lean_name == "SimmyBridge.Examples.so2_angle_add", "Lean theorem name");
static_assert(kRules[3].module == "SimmyBridge.Examples", "Lean module path");
static_assert(kRules[3].tag == TypeTag::So2Phasor, "SimmyBridge.Examples.so2_angle_add: concrete type tag so2_phasor");
static_assert(kRules[3].n_eqs > 0 && kRules[3].n_nodes > 0, "rule has a non-empty expression pool");
// (so2_phasor uses std::cos / std::sin: re-checked at run time by verify_all())

}  // namespace simmynet::verified

#endif  // SIMMYNET_VERIFIED_LEAN_VERIFIED_RULES_HPP
