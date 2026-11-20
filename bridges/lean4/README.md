# SimmyNet ⇄ Lean 4 (`mathlib`) bridge

A two-way, deterministic bridge between the **Lean 4 proof assistant** (with `mathlib`) and **SimmyNet's C++ engine**:

```
 Lean 4 (mathlib)                         C++17 (SIMD)                             Python
 ───────────────                          ────────────                             ──────
 @[simmy_rule] theorem  ──export──▶  rules.json ──lean_to_cpp.py──▶  LeanVerifiedRules.hpp ──▶ simmynet (wheel)
   (kernel-checked, ∀ params)         (structured AST)                (constexpr pool + static_assert)

 #eval / witness search ◀──@[extern]── libsimmynet_lean.so ◀── simmynet::ff (exact O(p) Legendre, Hasse–Weil, Frobenius)
   + certificate check in Lean         (C ABI, no lean.h)
```

Everything lives under `bridges/lean4/`. The only edits outside it are additive: an opt-in CMake option, a generated header, and a hand-written evaluator header (`include/simmynet/verified/`). The DSL grammar, the parser, the `pybind11` module and the `cibuildwheel` build are untouched.

| Direction | What it does | Files |
|---|---|---|
| **Lean → SimmyNet** | Tag a Mathlib theorem `lhs = rhs` with `@[simmy_rule]`; `#export_simmy_rules` lowers its `Lean.Expr` to JSON; `lean_to_cpp.py` generates a header-only C++17 rule table that is **re-checked at compile time**. | `SimmyBridge.lean`, `SimmyBridge/Examples.lean`, `Export.lean`, `lean_to_cpp.py` |
| **SimmyNet → Lean** | Lean calls SimmyNet's exact point counter, Hasse–Weil checker and Frobenius traces as native `@[extern]` primitives, then **checks a certificate** for what the oracle returned. | `simmy_lean_ffi.cpp`, `SimmyFFI.lean`, `SimmyOracleDemo.lean` |

## Trust model: oracle versus certificate

This distinction is the point of the design. Read it before using any result.

| | Who vouches for it | Where it is used |
|---|---|---|
| **Kernel-checked theorem** (`@[simmy_rule]`, `example97_recount : … := by decide +kernel`) | Lean's kernel. True for **all** parameters (rules) or for the stated instance (kernel `decide`). | Proofs. |
| **Lean-checked certificate at run time** (`checkTrace?`, `TraceCertificate.recountOk`) | Compiled Lean code. Trusts the Lean compiler, **not** the C++ oracle. | Accepting an oracle answer. |
| **Untrusted oracle** (`SimmyFFI.Raw.*`, `SimmyFFI.Oracle.*`, anything reached by `#eval`) | Nobody. It is C++ that Lean's kernel never sees. | `#eval`, witness search, testing. |

Consequences:

* An oracle result is a **candidate**. It becomes a theorem only after a Lean check. A wrong oracle (a C++ bug, a mis-linked library) makes the certificate check fail; it can never make a false statement pass.
* Nothing in this bridge uses `native_decide` or `ofReduceBool`. The exporter records each rule's axioms and flags `Lean.ofReduceBool` in the generated header.
* In the other direction, the C++ compile-time `static_assert`s on the imported rules are a **regression check** that SimmyNet's evaluator agrees with Lean on a deterministic grid of instances. They are not a proof; the proof is the Lean kernel's, for all parameters.
* The exporter refuses any declaration whose proof depends on `sorryAx`, and it requires every operator to carry the *canonical* instance for its carrier type (checked with `isDefEq` against the synthesised instance), so a user-defined `Add` on `ZMod p` cannot be mistaken for the standard one.

## Prerequisites

* Lean via [`elan`](https://github.com/leanprover/elan) (the toolchain is pinned by `lean-toolchain`: `leanprover/lean4:v4.36.0`, matching `mathlib` tag `v4.36.0`).
* CMake ≥ 3.18 and a C++17 compiler (as for SimmyNet itself).
* Python ≥ 3.9 for `lean_to_cpp.py` (standard library only; developed on 3.12).

## Reproducible setup and verification

Run from the repository root unless noted.

```bash
# 0. Lean toolchain manager
curl https://elan.lean-lang.org/elan-init.sh -sSf | sh

# 1. C++: build the C ABI shared library and its tests (opt-in; default targets are unchanged)
cmake -S . -B build -DSIMMYNET_BUILD_LEAN_FFI=ON -DSIMMYNET_BUILD_PYTHON=OFF
cmake --build build
ctest --test-dir build -R lean --output-on-failure     # ABI test, header test, header-freshness test

# 2. Lean: fetch mathlib and its prebuilt cache, then build everything
cd bridges/lean4
lake update                 # resolves mathlib v4.36.0 and writes lake-manifest.json (commit it for reproducibility)
lake exe cache get          # prebuilt .olean files; without this mathlib is compiled from source (hours)
lake build                  # SimmyBridge, SimmyExamples (kernel-checks the theorems), SimmyFFI,
                            # SimmyOracleEval (#eval of the oracle), simmy_oracle_demo

# 3. Lean → C++: export the tagged theorems and regenerate the header
lake env lean Export.lean   # runs #export_simmy_rules; writes ./rules.json
python lean_to_cpp.py       # rules.json → ../../include/simmynet/verified/LeanVerifiedRules.hpp
python lean_to_cpp.py --check                  # exits 1 if the committed header is stale

# 4. Rebuild and test everything against the freshly generated header
cd ../.. && cmake --build build && ctest --test-dir build --output-on-failure

# 5. C++ → Lean: run the oracle + certificate demo (exit code 0 iff all checks pass)
cd bridges/lean4 && lake exe simmy_oracle_demo
```

The Lean steps need the library from step 1 first: `lakefile.toml` links `libsimmynet_lean` from `../../build/bridges/lean4`. If your CMake build directory is not `build/`, edit the two `-L…`/`-rpath…` paths in `lakefile.toml`.

### About `rules.json` in this repository

The committed `rules.json` (and therefore the committed header) is **provisional**. It was produced where `mathlib`'s binary cache was not reachable, so `SimmyBridge/Examples.lean` could not be compiled against real `mathlib` there (its lemma names were checked against the `v4.36.0` sources, but `lake build` is the first real kernel check of those proofs: run it). The expression ASTs in it come from the *real* exporter run on Mathlib-free stand-ins with identical statement shapes (`test/CoreSmoke.lean`); the names and statements were filled in by hand, and no axiom lists are recorded. The header says so in a `WARNING: PROVISIONAL INPUT` comment. Running step 3 replaces both files with the genuine export (and the warning disappears).

## Direction 1: Lean → SimmyNet

### Tagging a rule

```lean
import SimmyBridge
open SimmyBridge

@[simmy_rule]
theorem frobenius_fixed (p : ℕ) [Fact p.Prime] (x : ZMod p) : x ^ p = x := ZMod.pow_card x
```

A tag attribute cannot be used in the file that registers it, so the attribute and command live in `SimmyBridge.lean` and the example theorems in `SimmyBridge/Examples.lean`. `Export.lean` imports the modules whose rules you want and calls `#export_simmy_rules "rules.json"`; add your own `import` there.

### The example rules (`SimmyBridge/Examples.lean`)

| Theorem | Statement | Type tag | SimmyNet use |
|---|---|---|---|
| `dihedral_srs_inv` | $s\,r\,s = r^{-1}$ in `DihedralGroup n` (all $n$, including 0) | `dihedral` | peephole group-word rewrite `s r s → R` |
| `so2_angle_add` | $\cos(a+b)$ and $\sin(a+b)$ addition formulas, i.e. $R(a)R(b) = R(a+b)$ | `so2_phasor` | `fold_phasor_angles` on `AlgebraicTuple` (exact angle sums) |
| `frobenius_fixed` | $x^p = x$ in `ZMod p`, $p$ prime | `finite_field` | `frobenius_fold<P>` with primality discharged by `static_assert` |
| `quarter_turns` (bonus) | $\pi/4 + \pi/4 = \pi/2$ over `ℝ` | `algebraic_tuple` | checked exactly with `ring::AlgebraicTuple<double>` |

The first three are the required examples; the fourth exercises the `algebraic_tuple` tag, which none of them reach.

### What the exporter accepts

| Category | Supported |
|---|---|
| Carrier types | `ZMod n`, `DihedralGroup n`, `ℝ`, `ℕ`, `ℤ` (`n` a `ℕ`-term over the parameters) |
| Operations | `+ - * /`, `^` with a `ℕ` exponent, negation, `⁻¹`, `%`, `Nat.cast`, `ZMod.val`, `DihedralGroup.r` / `.sr`, `Real.pi`, `Real.cos`, `Real.sin`, numerals |
| Hypotheses | `[Fact (Nat.Prime p)]` and `[NeZero n]`; other instance arguments are ignored; explicit propositional hypotheses are rejected |
| Conclusion | `lhs = rhs`, or a conjunction of such equations |

Anything else is rejected with a message naming the offending term, **all rejections are reported together, and nothing is written**. A generic carrier (`{R : Type} [Ring R]`) has no concrete SimmyNet type tag; state a specialised corollary on `ZMod p`, `DihedralGroup n` or `ℝ` and tag that. Its operator instances (`Ring.toAdd …`) are resolved to the concrete carrier by the canonical-instance check.

The concrete type tag of a rule is *resolved from the statement*, not declared:

| Tag | Chosen when |
|---|---|
| `finite_field` | the equations live in `ZMod n` (or in `ℕ`/`ℤ` and use `%` / `ZMod.val`) |
| `dihedral` | the equations live in `DihedralGroup n` |
| `so2_phasor` | the equations live in `ℝ` and use `cos` / `sin` |
| `algebraic_tuple` | the equations live in `ℝ`, stay in $\mathrm{span}\{1,\pi\}$ (`+`, `-`, negation, scalar `*` and `/` only) |

### `rules.json` (schema `simmynet.lean-rules/1`)

One object per rule, keys sorted, rules ordered by name:

```json
{ "name": "SimmyBridge.Examples.frobenius_fixed", "module": "SimmyBridge.Examples",
  "type_tag": "finite_field", "statement": "∀ (p : ℕ) [inst : Fact (Nat.Prime p)] (x : ZMod p), x ^ p = x",
  "axioms": ["propext"],
  "params": [ {"name": "p", "dom": "nat", "hyps": ["prime"]},
              {"name": "x", "dom": "zmod", "hyps": [], "modulus": {"op": "var", "name": "p", "dom": "nat"}} ],
  "equations": [ {"lhs": {"op": "pow", "dom": "zmod", "modulus": …, "args": […]}, "rhs": {"op": "var", …}} ] }
```

Every node is `{op, dom, [modulus], …}`; `dom` is one of `nat`, `int`, `zmod`, `dihedral`, `real`, and `zmod` / `dihedral` nodes carry their modulus as a `ℕ`-valued node.

### The generated header (`include/simmynet/verified/LeanVerifiedRules.hpp`)

Header-only, ASCII, C++17, warning-free under `-Wall -Wextra -Wpedantic`. For each rule it records the **fully qualified Lean theorem name, the Lean module path and the concrete type tag** in a comment block *and* as `static_assert`s:

```cpp
static_assert(kRules[2].lean_name == "SimmyBridge.Examples.frobenius_fixed", "Lean theorem name");
static_assert(kRules[2].module    == "SimmyBridge.Examples",                 "Lean module path");
static_assert(kRules[2].tag       == TypeTag::FiniteField, "…frobenius_fixed: concrete type tag finite_field");
static_assert(check_rule<TypeTag::FiniteField>(kRules[2]).ok(), "…C++ evaluation disagrees with the Lean theorem…");
```

* **Data is generated, logic is not.** The flat expression pools are generated; the evaluator (`RuleKit.hpp`: exact `int64` residues, dihedral multiplication table, `double` for $SO(2)$, and the real `ring::AlgebraicTuple<double>` for the $\mathrm{span}\{1,\pi\}$ module) is ordinary reviewed C++.
* **Compile-time re-check.** Every rule whose arithmetic is `constexpr` (all tags except `so2_phasor`, which needs `std::cos`) is evaluated on a deterministic instance grid inside `static_assert`; `verify_all()` re-checks all four at run time. A drift between Lean's semantics and the C++ evaluator is a compile error. Integer evaluation is overflow-checked; an unevaluable instance is counted as `unsupported`, which also fails.
* **Rewrite hooks, only when the rule is present.** `SIMMYNET_VERIFIED_HAS_FROBENIUS_FIXED` / `…_SO2_ANGLE_ADD` / `…_DIHEDRAL_SRS_INV` are defined exactly when the corresponding rule is in the table; each hook is justified by that one theorem and by nothing else.
* **No existing table is patched.** SimmyNet has no global rewrite table (constant folding is matrix-product based), so the hooks are *additive*: `fold_phasor_angles` plugs into the `AlgebraicTuple<T>` templates by operating on them; nothing in the DSL parser or compiler is modified or required to include this header.
* **Strictly deterministic.** No timestamps, rules sorted by Lean name, LF endings, a SHA-256 of the canonical input in the header. `python lean_to_cpp.py --check` (also run by `ctest` as `simmynet_lean_header_fresh`) fails when the committed header is stale.

## Direction 2: SimmyNet → Lean

`simmy_lean_ffi.cpp` exports `extern "C"` functions using only `uint64_t`, `int64_t` and `double`; there is no `<lean/lean.h>` dependency.

| C function | Lean primitive (`SimmyFFI.Raw`) | Wraps |
|---|---|---|
| `simmy_legendre_count_points(a, b, p)` | `legendreCountPoints` | `ff::legendre_count_points` (exact $O(p)$, primes $5 \le p \le 10^6$; **not** Schoof) |
| `simmy_frobenius_trace(a, b, p)` | `frobeniusTrace` | `EllipticCurveNode::compute_trace` ($a_p = p+1-\#E$) |
| `simmy_frobenius_trace_power(a, b, p, n)` | `frobeniusTracePower` | `LefschetzTracePass::trace_of_power` |
| `simmy_extension_point_count(a, b, p, n)` | `extensionPointCount` | `LefschetzTracePass::count_points` ($\#E(\mathbb{F}_{p^n})$) |
| `simmy_hasse_bound_holds(t, q)` | `hasseBoundHolds` | `ff::hasse_bound_holds` (exact 128-bit $t^2 \le 4q$) |
| `simmy_hasse_margin(t, q)`, `simmy_sato_tate_cos(t, q)` | `hasseMargin`, `satoTateCos` | $2\sqrt q - \lvert t\rvert$ and $t/(2\sqrt q)$ (`double`, informational) |
| `simmy_weil_certify(a, b, p)` | `weilCertify` | `WeilProver::certify`, as a bit mask |
| `simmy_lean_abi_version(_)`, `simmy_version_number(_)` | `abiVersion`, `versionNumber` | identification |

Conventions: every function is pure and deterministic and never throws across the C boundary; errors are in-band (`INT64_MIN` for `int64_t`, NaN for `double`, bit 63 for the Weil mask). Lean's `Int64` is emitted as `uint64_t` by Lean's C backend, which is ABI-identical to `int64_t`.

### Using the oracle (UNTRUSTED)

```lean
import SimmyFFI
open SimmyFFI

#eval Oracle.countPoints 2 3 97             -- some 100    (oracle: untrusted)
#eval Oracle.frobeniusTrace 2 3 97          -- some (-2)   (oracle: untrusted)
#eval searchB? 1 31 0                       -- oracle proposes a witness, Lean re-validates it
```

`#eval` of an `@[extern]` needs the library to be loadable by Lean's interpreter. That is why `lean_lib SimmyFFI` sets `precompileModules = true` and links `simmynet_lean` through `moreLinkArgs`; `#eval` in the module that *declares* the externs does not work, hence the separate `SimmyOracleEval.lean`.

### Checking a certificate (CHECKED)

```lean
def example97 : TraceCertificate := ⟨2, 3, 97, -2⟩          -- y² = x³ + 2x + 3 over F₉₇, claimed trace −2
theorem example97_hasse   : example97.hasseOk   = true := by decide          -- t² ≤ 4p, kernel-checked
theorem example97_recount : example97.recountOk = true := by decide +kernel  -- #E(F₉₇) = 98 − (−2) = 100, kernel-checked

#eval (checkTrace? 2 3 97 (-2)).isSome      -- true:  proof-carrying CheckedTrace (compiled Lean code)
#eval (checkTrace? 2 3 97 0).isSome         -- false: a forged trace is rejected
```

`TraceCertificate.recountOk` compares the oracle's claim against `pointCountSpec`, a deliberately naive $O(p^2)$ *definition* of $\\#E(\mathbb{F}_p)$, so it is practical only for small $p$. For large $p$ (the oracle handles $p \le 10^6$ in about 10 ms) only the cheap necessary condition `hasseOk` ($t^2 \le 4p$) is available; the demo says so explicitly.

## Tests

| Test | What it checks |
|---|---|
| `ctest`: `simmynet_lean_ffi_abi` | every C function against `simmynet::ff` on a $(a,b,p)$ grid, the trace recurrence $t_{n+1} = t\,t_n - p\,t_{n-1}$, in-band domain errors |
| `ctest`: `simmynet_lean_verified_rules` | hooks against independent references (square-and-multiply, an independent $D_n$ model for $n = 0..12$, $R(a)R(b) = R(a+b)$), and **negative controls**: false rules must be rejected at compile time |
| `ctest`: `simmynet_lean_header_fresh` | committed header equals the generator's output for the committed `rules.json` |
| `lake build` | kernel-checks the example theorems and `example97_*`; `#eval`s the oracle; links the C++ library |
| `lake env lean test/CoreSmoke.lean` | Mathlib-free exporter smoke test (stand-ins for `ZMod`, `DihedralGroup`, `ℝ`); needs only Lean core |
| `lake exe simmy_oracle_demo` | oracle → certificate round trip; exit code 0 iff all pass |

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `ld.lld: error: undefined reference: arc4random@GLIBC_2.36` (and similar) | Lean's bundled linker uses an old glibc sysroot. `lakefile.toml` already passes `-Wl,--allow-shlib-undefined`; on macOS use `-Wl,-undefined,dynamic_lookup`. |
| `cannot open shared object file: libsimmynet_lean.so` | Build it first (step 1) or set `LD_LIBRARY_PATH`; the rpath assumes the `build/` layout above. |
| `could not find native implementation of external declaration` | The module that *declares* the extern was `#eval`ed, or `precompileModules` is off. Use a downstream module, as in `SimmyOracleEval.lean`. |
| `#export_simmy_rules: no @[simmy_rule] declarations` | The module that tags your theorems is not imported in `Export.lean`. |
| `non-canonical instance …` | The statement uses an instance that is not the synthesised one for that carrier (e.g. a hand-written `Add (ZMod p)`); restate it with the standard operators. |
| A tagged theorem fails to compile after a `mathlib` bump | Lemma names can move between releases. The proofs use only `ZMod.pow_card`, `Real.cos_add`, `Real.sin_add`, `DihedralGroup.sr_mul_r`, `DihedralGroup.sr_mul_sr`, `DihedralGroup.inv_r` and `ring`; adjust those. |
| Library built with `-march=native` runs elsewhere and crashes | CMake defaults to `SIMMYNET_NATIVE_ARCH=ON`; configure with `-DSIMMYNET_NATIVE_ARCH=OFF` for a portable `libsimmynet_lean`. |

## Platform notes

Developed and tested on Linux x86-64 with GCC 13, CMake 4.x, Lean 4.35.0. The `$ORIGIN` rpath and `--allow-shlib-undefined` in `lakefile.toml` are Linux-specific; the macOS equivalents are given above. Windows is not covered.
