---
title: Lean 4 bridge
layout: default
nav_order: 6
---

# Lean 4 (`mathlib`) bridge

SimmyNet and a proof assistant are complementary. SimmyNet answers *"what is the value for these parameters?"* exactly and in nanoseconds; Lean 4 with `mathlib` answers *"is this true for all parameters?"* with a small trusted kernel. The bridge in `bridges/lean4/` connects them in both directions and keeps the two trust levels visibly separate. It is opt-in and changes none of the DSL, the parser, the `pybind11` module or the wheel build.

```
 Lean 4 (mathlib)                      C++17 (SIMD)                              Python
 @[simmy_rule] theorems  ──────────▶  LeanVerifiedRules.hpp (constexpr)  ───▶  simmynet wheel
   kernel-checked, ∀ params            rewrite hooks + static_assert
 #eval / witness search  ◀──────────  libsimmynet_lean.so (C ABI)  ◀────────  simmynet::ff
   + certificate check in Lean         exact Legendre count, Hasse–Weil, Frobenius
```

Setup, build and verification commands are in `bridges/lean4/README.md`.

## Direction 1: formal rules into the C++ headers

A theorem $\mathrm{lhs} = \mathrm{rhs}$ tagged `@[simmy_rule]` is read by the `#export_simmy_rules "rules.json"` command, which walks its `Lean.Expr`, resolves the polymorphic Mathlib instances to a concrete carrier, and lowers the supported operations (`+ - * / ^`, inverse, `%`, group words, $SO(2)$ rotation identities) to a JSON AST. The standard-library-only script `lean_to_cpp.py` turns the JSON into `include/simmynet/verified/LeanVerifiedRules.hpp`.

The concrete type tag is **resolved from the statement**:

| Tag | Carrier of the equations | Example rule |
|---|---|---|
| `finite_field` | `ZMod p` (or `ℕ`/`ℤ` with `%`) | $x^p = x$ in $\mathbb{Z}/p$ (Frobenius), $p$ prime |
| `dihedral` | `DihedralGroup n` | $s\,r\,s = r^{-1}$ |
| `so2_phasor` | `ℝ` with `cos` / `sin` | $\cos(a+b) = \cos a \cos b - \sin a \sin b$ and the sine formula, i.e. $R(a)R(b) = R(a+b)$ |
| `algebraic_tuple` | `ℝ` inside $\mathrm{span}\{1,\pi\}$ | $\tfrac{\pi}{4} + \tfrac{\pi}{4} = \tfrac{\pi}{2}$ |

The `algebraic_tuple` tag is the module that `ring::AlgebraicTuple<T>` represents, so such a rule is checked in the exact arithmetic of the DSL frontend. The `so2_phasor` rules are what justify folding the product of two phasor generators into one generator whose angle is the exact sum.

Each generated rule carries its Lean theorem name, module path and type tag in comments **and** in `static_assert`s. For every tag whose arithmetic is `constexpr` the rule is also evaluated on a deterministic grid of instances at compile time, so a drift between Lean's semantics and the C++ evaluator fails the build. That check is a regression guard, not a proof: the proof is the Lean kernel's, for all parameters.

Because SimmyNet has no global rewrite table (constant folding is matrix-product based), the generated header is **additive**. It exposes a rule table (`kRules`, `find_rule`, `verify_all`) and rewrite hooks that exist only when the matching rule does:

| Macro (defined iff the rule is present) | Hook | Justified by |
|---|---|---|
| `SIMMYNET_VERIFIED_HAS_SO2_ANGLE_ADD` | `fold_phasor_angles(a, b)` on `AlgebraicTuple<T>` | $R(a)R(b) = R(a+b)$ |
| `SIMMYNET_VERIFIED_HAS_FROBENIUS_FIXED` | `frobenius_fold<P>(x)`, primality of `P` discharged by `static_assert` | $x^p = x$ |
| `SIMMYNET_VERIFIED_HAS_DIHEDRAL_SRS_INV` | `reduce_srs_to_rinv(word, len)` peephole | $s\,r\,s = r^{-1}$ (sound, not claimed confluent) |

Generation is strictly deterministic (no timestamps, rules sorted by Lean name, a SHA-256 of the canonical input in the header), and `python lean_to_cpp.py --check` fails when a committed header is stale.

## Direction 2: SimmyNet as a native oracle for Lean

`bridges/lean4/simmy_lean_ffi.cpp` builds the shared library `simmynet_lean` (CMake option `SIMMYNET_BUILD_LEAN_FFI`, default `OFF`). It exposes the exact $O(p)$ Legendre point counter, the Hasse–Weil bound check, Frobenius traces $\mathrm{Tr}(F^n)$, $\#E(\mathbb{F}_{p^n}) = \det(I - F^n)$ and the Weil certificate as `extern "C"` functions over `uint64_t`, `int64_t` and `double` only (no `lean.h`). `SimmyFFI.lean` declares them with `@[extern "simmy_..."]`.

| Level | Trusted by | Examples |
|---|---|---|
| Kernel-checked theorem | Lean's kernel | `@[simmy_rule]` rules; `example97_recount : … := by decide +kernel` |
| Run-time certificate check | Compiled Lean code (not the C++ oracle) | `checkTrace?`, `TraceCertificate.recountOk` |
| Untrusted oracle | Nobody | `SimmyFFI.Raw.*`, `SimmyFFI.Oracle.*`, `#eval` |

A returned trace $t$ for $y^2 = x^3 + ax + b$ over $\mathbb{F}_p$ is a *certificate candidate*. Lean checks the Hasse–Weil bound $t^2 \le 4p$ cheaply for any $p$, and for small $p$ recounts the points from a naive $O(p^2)$ definition of $\#E(\mathbb{F}_p) = p + 1 - t$. A wrong oracle can only make the check fail; it can never make a false statement pass.

## Limits

- The Legendre oracle covers primes $5 \le p \le 10^6$ (not Schoof). Beyond that, only the Hasse–Weil necessary condition is available as a certificate.
- The exporter supports the statement language listed in `bridges/lean4/README.md`; a rule outside it is rejected with the offending term, and nothing is written.
- Generic algebraic structures (`[Ring R]`, `[Group G]`) have no concrete SimmyNet tag; tag a specialised corollary instead.
