---
title: C++ API
layout: default
nav_order: 5
---

# C++ API (header-only, C++17)

All headers live in `include/simmynet/` and are included as `#include <simmynet/Name.hpp>`. Everything is in `namespace simmynet`; the finite-field passes are in `namespace simmynet::ff` and the exact ring in `namespace simmynet::ring`. The version is available as `SIMMYNET_VERSION_STRING` (`"2.2.0"`), `SIMMYNET_VERSION_NUMBER` (`20200`) and `simmynet::kVersionString` from `<simmynet/Version.hpp>`.

```cpp
#include <simmynet/Interpreter.hpp>                    // DSL: lexer + parser + compiler + runtime
#include <simmynet/finite_field/WeilProver.hpp>        // exact finite-field passes
```

Build: `g++ -std=c++17 -I include main.cpp`, or link the CMake interface target **`SimmyNet::native_cpp`** (target `simmynet_native_cpp`, which carries the include directory and `cxx_std_17`). Other targets: `simmynet_core` (the pybind11 module `simmynet._core`), `simmynet_cli` (the executable `simmynet_cli`). A single-file amalgamation of the CLI is `single_header/simmynet_single.cpp` (regenerate with `python3 scripts/amalgamate.py`).

| Area | Headers |
|---|---|
| [Exact ring and expressions](#exact-ring-and-expressions) | `algebraic_ring.hpp`, `AST.hpp` |
| [DSL pipeline](#dsl-pipeline) | `Lexer.hpp`, `Parser.hpp`, `Compiler.hpp`, `Runtime.hpp`, `Interpreter.hpp` |
| [Matrix engine](#matrix-engine) | `MatrixEngine.hpp`, `WordEval.hpp` |
| [Finite-field passes](#finite-field-passes-simmynetff) | `Mat2.hpp`, `finite_field/FrobeniusGenerator.hpp`, `LefschetzTracePass.hpp`, `ZetaFunctionPass.hpp`, `WeilProver.hpp` |

## Exact ring and expressions

### `simmynet::ring::AlgebraicTuple<T = double>` (`algebraic_ring.hpp`)

The exact element $c_{base} + c_\pi\,\pi$ of the $\mathbb{Z}$-module $\mathrm{span}\{1, \pi\}$ (`T` must be a floating-point type). All members are `constexpr`.

| Member | Description |
|---|---|
| `T c_base, c_pi` | Coefficients of $1$ and $\pi$. |
| `AlgebraicTuple(T base)` · `AlgebraicTuple(T base, T pi_coeff)` | A plain number is the tuple `{x, 0}` (implicit conversion). |
| `static AlgebraicTuple pi()` | `{0, 1}`. |
| `is_pure()` | `c_pi == 0`. |
| `+`, `-`, unary `-`, `* T`, `T *`, `/ T`, `+=`, `-=`, `*=`, `/=` | Exact on the coefficients. There is deliberately **no tuple × tuple product** and no trig. |
| `==`, `!=` | Compare coefficients: no tolerance. |
| `to_double()` / `evaluate()` | Boundary lowering: `c_base + c_pi * kPiValue<T>`; a pure tuple is returned unchanged bit for bit. |

`ring::kPiValue<T>` is the constant $\pi$. `bool ring::equal_mod_2pi(const AlgebraicTuple<T>& a, const AlgebraicTuple<T>& b) noexcept` decides whether two **angles** are equal modulo $2\pi$ exactly on their coefficients (equal `c_base`, even-integer difference of `c_pi`, subtraction verified exact); `false` means "not provably equal".

### Expressions (`AST.hpp`)

| Symbol | Description |
|---|---|
| `using Exact = ring::AlgebraicTuple<double>` | Value type of the frontend. |
| `Expr`, `ExprPtr` | `std::variant<Num, Var, Neg, BinOp, Call>`; builders `mkNum`, `mkPi`, `mkVar`, `mkNeg`, `mkBin`, `mkCall`. |
| `isKnownFunc(name)` | `abs`, `sqrt`, `relu`, `tanh`, `sin`, `cos`. |
| `evalExprExact(expr, env)` → `Exact` · `evalExpr(expr, env)` → `double` | Exact evaluation (tuple arithmetic) / with boundary lowering. `Env = std::unordered_map<std::string, Exact>`. |
| `freeVariables(expr)`, `fmtExpr(expr)`, `fmtExact(t)` | Free names; pretty printers. |
| `exprops::mul/div/func` | Operation semantics on tuples (scalar products exact; everything else lowers to `double`). |

**`ExprProgram`**: a compiled reverse-Polish tape with constant sub-trees folded using tuple arithmetic.

| Member | Description |
|---|---|
| `enum class Op : unsigned char { Const, Var, Neg, Add, Sub, Mul, Div, Abs, Sqrt, Relu, Tanh, Sin, Cos }` | Opcodes. |
| `struct Instr { Op op; int idx; double value; }` | One instruction: **16 bytes** (`static_assert`). `value` holds a `Const` already lowered to `double`. |
| `static constexpr int kMaxStack = 64` | Evaluation stack depth; deeper expressions throw `CompileError("expression is too deeply nested")`. |
| `static ExprProgram compile(const Expr& root, const std::vector<std::string>& names)` | Resolve variable names (the last occurrence wins); fold constants exactly, lower once. |
| `double run(const double* vars) const` | Hot path: doubles only, no tuple, no allocation. May throw `SimmyNetRuntimeError` (division by zero, `sqrt` domain). |
| `Exact run_exact(const Exact* vars) const` | The same tape on tuples; compile-time use only. |
| `size()` | Number of instructions. |

## DSL pipeline

| Header | Symbol | Description |
|---|---|---|
| `Lexer.hpp` | `tokenize(const std::string& source)` | `std::vector<Token>` (`TokKind::Number/Ident/Symbol/Eof`, text, line); throws `LexError`. |
| `Parser.hpp` | `Parser(std::vector<Token>).parseProgram()` | Recursive descent → `std::vector<GroupDecl>`; throws `ParseError`. |
| `Compiler.hpp` | `SimmyNetCompiler(KernelSynthesizer = KernelSynthesizer())` | The compiler. `static constexpr std::size_t MAX_GROUP_ORDER = 2048`. |
| | `compileProgram(const std::vector<GroupDecl>&)` / `compileGroup(const GroupDecl&)` | AST → `std::vector<CompiledGroup>` / `CompiledGroup` (declarations, subgroups, classifiers and `Task`s). Throws `CompileError("group NAME, line N: ...")`. |
| | `Network`, `FoldedNetwork` | Unfolded IR (`LinearLayer`, `LiftLayer`, `PerceptronBankLayer`, `HyperplaneLayer`; exact tuples) and folded graph (`Wc`, `foldedFactors`, `lifts` as `ExprProgram` tapes, `bank`, `hyper`; doubles only). |
| | `foldNetwork(const Network&)` | The constant-folding pass → `FoldedNetwork`. |
| | `Job`, `VerifyJob`, `CheckJob`, `Task = std::variant<Job, VerifyJob, CheckJob>` | Compiled queries. `CheckJob::exact` is an `ExactVerdict` (`NotApplicable`, `ProvedEqual`, `Inconclusive`) with `lhsAngle`, `rhsAngle`. |
| | `KernelSynthesizer(int maxLifts = 3, int maxSparseSupport = 3, int maxUpdates = 5000)` | `perceptronSeparable(Table, Labels)` (cycle certificate) and `synthesize(Table, Labels, names, label)` → `SynthesisResult { separableBefore, lifts, weight, bias, log }`. |
| | `ContinuousSO2Group` | `phasor`, `rotation`, `matrix`, `matrixFromPhasor`, `isRotation`, `phasorOf`, `angleOf`, `inSector`, `sectorPerceptrons`. |
| `Runtime.hpp` | `SimmyNetRuntime(double sharpness = 10.0)` | Executes folded graphs. |
| | `forward(const FoldedNetwork&, const std::vector<double>& x, bool withTrace = true)` | One folded pass → `ForwardResult { inputVector, finalVector, decision, score, softScore, trace }`. |
| | `decideFast(net, const double* x, Scratch& s, double* scoreOut = nullptr)` | Allocation-free decision (reuses a caller-owned `Scratch`). |
| | `forwardReference(const Network&, x)`, `decideUnfolded(...)`, `expandKron(...)` / `decideKron(...)` | The unfolded Kronecker-expanded oracle and the $O(Lk^3)$ / $O(Lk^4)$ baselines. |
| `Interpreter.hpp` | `SimmyNetInterpreter(double sharpness = 10.0, std::ostream* out = nullptr)` | End to end. |
| | `compile(const std::string& source) const` | → `std::vector<CompiledGroup>`. |
| | `run(const std::string& source)` | Compile, run every query, cross-check folded vs reference on every job → `Report { passed, failed, xChecked, xFailed }`. |
| `Examples.hpp` | `simmynet::examples::kExamples` | The built-in demonstration programs used by `simmynet_cli`. |
| `Api.hpp` | `simmynet::api::...` | The facade used by the Python bindings (`Interpreter`, `Program`, `GroupHandle`, `TaskHandle`, `JobHandle`, `RunResult`). |

Errors (`Common.hpp`): `SimmyNetError` (base, derives from `std::runtime_error`), `LexError`, `ParseError`, `CompileError`, `SimmyNetRuntimeError`. `MatrixEngine.hpp` also defines `SingularMatrix`. The fixed-size word kernels throw `std::invalid_argument`.

```cpp
simmynet::SimmyNetInterpreter interp(10.0, &std::cout);
simmynet::Report rep = interp.run(source);        // rep.failed == 0 && rep.xFailed == 0 on success
```

## Matrix engine

**`MatrixEngine.hpp`**

| Symbol | Description |
|---|---|
| `template <int N> struct alignas(32) Mat` | Fixed-size row-major `double` matrix: `zero()`, `identity()`, `fromData(const double*)`, `operator()(i, j)`, `operator*` (unrolled), `apply(const std::array<double, N>&)`, `maxAbsDiff(other)`. |
| `Matrix2x2`, `Matrix3x3`, `Matrix4x4` | `Mat<2>`, `Mat<3>`, `Mat<4>`. |
| `dot(a, b, n)` | Four-accumulator dot product (vectorisable without `-ffast-math`). |
| `gemm(A, B, C, k)`, `gemvSquare(A, x, y, k)`, `gemv(A, x, y, rows, cols)` | Dispatch to fully unrolled kernels for $k \in \{1, 2, 3, 4\}$, a generic loop otherwise. |
| `DMat` | Dynamic row-major matrix; `identity(n)`, `operator()`, `data()`, `row(i)`, `isSquare()`. |
| `matmul`, `kron`, `inverse` (throws `SingularMatrix`), `cleanMatrix`, `matrixKey`, `maxAbsDiff` | Dense helpers. |
| `std::size_t& gemmCounter()` | Matrix-multiplication passes executed so far (every `gemm`, including `ff::mul`). |

**`WordEval.hpp`**: fixed-size ($k \le 4$) word folding on `alignas(64)` buffers; used by the Python bindings.

| Symbol | Description |
|---|---|
| `constexpr int kMaxWordDim = 4` | Largest $k$. |
| `foldWord(int k, const std::vector<double>& factors, double* out)` | $F_1 \cdots F_L$ into `out` (rightmost acts first); `k` must be 2, 3 or 4, else `std::invalid_argument`. |
| `evalWordCompiled(k, factors, weight, bias)` | Fold, then $\mathrm{Step}(w \cdot \mathrm{vec}(W) + b)$. |
| `class CompiledWord` | `CompiledWord(int k, const std::vector<double>& factors)`; `dim()`, `length()`, `matrix()`, `decide(weight, bias)` (0 matrix passes). |

## Finite-field passes (`simmynet::ff`)

### `Mat2.hpp`: exact integer buffer

| Symbol | Description |
|---|---|
| `Mat2` | `alignas(64)` row-major $2 \times 2$ `int64` matrix; `Mat2::identity()`. |
| `Checked<T>` | `{ T value; bool overflow; }`: a result plus a sticky overflow flag. |
| `Poly2` | `c[0] + c[1]T + c[2]T²` with exact `int64` coefficients; `==`. |
| `ScaledMat2` | `{ num, den }`: a rational matrix so inverses stay integral. |
| `mul(A, B)`, `sub(A, B)` | `Checked<Mat2>` product (counted in `gemmCounter()`) / difference. |
| `det(A)`, `adjugate(A)` | `Checked`; `transpose(A)`, `equal(A, B)`, `select(c, A, B)`: branch-free. |
| `add_ovf`, `sub_ovf`, `mul_ovf` | Scalar arithmetic returning `true` on overflow (`mul_ovf`: `__builtin_mul_overflow`, or a portable 128-bit path under `SIMMYNET_FF_PORTABLE_ARITH`). |
| `U128`, `mul64`, `gt`, `eq`, `magnitude` | 128-bit helpers for the exact Hasse comparison. |
| `toEngine(A)` / `fromEngine(E)` | `optional<Matrix2x2>` / `optional<Mat2>`; `nullopt` unless exact (integer-valued, $\lvert\cdot\rvert \le 2^{53}$). |

### `finite_field/FrobeniusGenerator.hpp`

**Free symbols**

| Symbol | Description |
|---|---|
| `inline constexpr std::int64_t kMaxLegendreModulus = 1000000` | Largest prime the Legendre counter accepts. |
| `bool hasse_bound_holds(std::int64_t t, std::int64_t q) noexcept` | Exact $t^2 \le 4q$ in 128-bit arithmetic (safe for `INT64_MIN` and $q = 2^{62}$); `false` for $q \le 0$. |
| `std::optional<std::int64_t> legendre_count_points(a, b, p)` | $\#E(\mathbb{F}_p)$ by $O(p)$ Legendre-symbol summation. `nullopt` unless $p$ is a prime in $[5, 10^6]$ and the curve is nonsingular. Every intermediate is $< 2^{41}$. **Not Schoof's algorithm.** |
| `using TraceResolver = std::function<std::optional<std::int64_t>(const EllipticCurveNode&)>` | Plug any trace source in (a table, an external Schoof/SEA, a certificate). |
| `legendre_trace_resolver(const EllipticCurveNode&)` | A ready-made `TraceResolver` backed by `compute_trace()`. |
| `schoof_trace_stub(const EllipticCurveNode&)` | Placeholder that **always** returns `nullopt`; Schoof/SEA is not implemented. |

**`EllipticCurveNode(a, b, q)`**: AST node for $y^2 = x^3 + ax + b$ over $\mathbb{F}_q$ ($a$, $b$ reduced mod $q$).

| Member | Description |
|---|---|
| `a()`, `b()`, `q()`, `trace()` | Accessors; `trace()` is `std::optional<int64_t>` (empty = unresolved). |
| `valid()` | $3 < q \le 2^{62}$, $q$ odd, $q \bmod 3 \ne 0$, $4a^3 + 27b^2 \not\equiv 0$. Primality of $q$ is the caller's guarantee here. |
| `try_with_trace(t)` | `std::optional<EllipticCurveNode>`: the curve with trace `t` attached, or **`nullopt`** if `t` violates the Hasse–Weil bound $t^2 \le 4q$. |
| `with_trace(t)` | Fluent, `noexcept`: always returns a curve. If `t` violates the bound the trace is left **unresolved** (any previously attached trace is cleared), so `FrobeniusGenerator::lift()` then yields `nullopt`. Use `try_with_trace` when you need to detect the rejection. |
| `resolve(const TraceResolver&)` | Same validation applied to the resolver's result; an empty resolver, `nullopt` or an impossible value leaves the trace unresolved. |
| `count_points()` | `optional<int64_t>`: $\#E(\mathbb{F}_q)$ by the Legendre sum; **only for prime $5 \le q \le 10^6$** (`nullopt` otherwise, or for a singular curve). |
| `compute_trace()` | `optional<int64_t>`: $a_q = q + 1 - \#E(\mathbb{F}_q)$; same domain. |
| `with_computed_trace()` | The curve with `compute_trace()` attached; unresolved outside the domain. |

The Hasse–Weil check is necessary, not sufficient: it rejects impossible traces but cannot prove a supplied trace belongs to this curve.

```cpp
using namespace simmynet::ff;
EllipticCurveNode e(2, 3, 97);
e.count_points();                     // 100
e.compute_trace();                    // -2
e.try_with_trace(20);                 // nullopt: 400 > 4 * 97 = 388
e.with_trace(20).trace();             // nullopt (unresolved), no exception
auto node = FrobeniusGenerator::lift(e.with_computed_trace());   // F = [[0,-97],[1,-2]]
```

**Lifting**

| Symbol | Description |
|---|---|
| `FrobeniusNode` | `{ Mat2 F; std::int64_t q; }` |
| `FiniteFieldNode` | `std::variant<EllipticCurveNode, FrobeniusNode>` |
| `FrobeniusGenerator::lift(curve)` | → `optional<FrobeniusNode>`, $F = \begin{pmatrix}0 & -q\\ 1 & a_q\end{pmatrix}$; `nullopt` if invalid or the trace is unresolved. Also accepts a `FiniteFieldNode`. |
| `FrobeniusGenerator::lift_to_engine(curve)` | The same lift as `simmynet::Matrix2x2`; `nullopt` if an entry exceeds $2^{53}$. |

### `finite_field/LefschetzTracePass.hpp`: `LefschetzTracePass` (static members)

| Function | Returns | Description |
|---|---|---|
| `trace(F)` | `int64` | $\mathrm{Tr}\,F$ (AVX2 diagonal sum when available). |
| `determinant(F)` | `Checked<int64>` | $\det F$. |
| `characteristic_polynomial(F)` | `Checked<Poly2>` | $P(T) = 1 - tT + \det(F)\,T^2$. |
| `power(F, std::uint64_t n)` | `Checked<Mat2>` | $F^n$, binary exponentiation (≤ 64 steps), branch-free. |
| `trace_of_power(F, n)` | `optional<int64>` | $\mathrm{Tr}(F^n)$; `nullopt` on overflow. |
| `count_points(F, n)` | `optional<int64>` | $\#E(\mathbb{F}_{q^n}) = \det(I - F^n)$; `nullopt` for $n = 0$ or if not certifiable in `int64`. |

### `finite_field/ZetaFunctionPass.hpp`

`ZetaNode { Poly2 num, den; }` ($Z(T) = \text{num}/\text{den}$). `ZetaFunctionPass::collapse(F, q)` → `optional<ZetaNode>`: numerator $1 - tT + qT^2$, denominator $(1 - T)(1 - qT)$; rejects $\det F \ne q$ and overflow. `collapse_ast(F, q)` is the heap-allocated form (`unique_ptr`, null on failure). `to_string(Poly2)` and `to_string(ZetaNode)` pretty-print.

### `finite_field/WeilProver.hpp`

| Symbol | Description |
|---|---|
| `Verdict` | `REFUTED` / `PROVED`; `to_string(v)`. |
| `WeilProver::prove_hasse_weil(F, q)` | → `HasseWeilReport { verdict, trace, det_is_q, delta_negative, delta_zero, overflow }`. |
| `WeilProver::prove_functional_equation(F, q)` | → `FunctionalEquationReport { verdict, inverse_collapse, weil_pairing, numerator_reflects, denominator_reflects, overflow }`. |
| `WeilProver::certify(const FrobeniusNode&)` | → `WeilCertificate { hasse, functional_equation }`. |

All passes are `noexcept` and branch-free where it matters; a value that cannot be certified inside `int64` is reported (`overflow` / `nullopt`), never wrapped.

## Verified rules from Lean 4 (`simmynet::verified`, optional)

Header-only, C++17. See the [Lean 4 bridge](lean4-bridge.md) for the pipeline and its trust model.

### `verified/RuleKit.hpp` (hand-written)

| Symbol | Description |
|---|---|
| `TypeTag` | `FiniteField`, `Dihedral`, `So2Phasor`, `AlgebraicTuple`: the concrete carrier of a rule. |
| `Node`, `Param`, `Equation`, `Rule` | Flat, `constexpr`-friendly rule data: `Rule { lean_name, module, kind, tag, nodes, params, eqs }`. |
| `check_rule<Tag>(rule)` | → `Report { instances, failures, unsupported }`; evaluates both sides of every equation on a deterministic instance grid. `constexpr` for every tag except `So2Phasor` (it needs `std::cos` / `std::sin`). `Report::ok()` requires at least one instance and no failure or unsupported equation. |
| `check_rule_runtime(rule)` | The same, dispatching on `rule.tag`, for all four tags. |
| `powmod64(b, e, m)`, `mulmod64(a, b, m)` | `constexpr` modular arithmetic for any 64-bit modulus, without 128-bit types. |

Evaluation uses overflow-checked `int64` for `finite_field`, Mathlib's multiplication table for `dihedral` (`r i * sr j = sr (j - i)`, ...), `double` for `so2_phasor`, and `ring::AlgebraicTuple<double>` with exact `==` for `algebraic_tuple`. An instance that cannot be evaluated is counted as `unsupported`, never skipped.

### `verified/LeanVerifiedRules.hpp` (generated by `bridges/lean4/lean_to_cpp.py`)

| Symbol | Description |
|---|---|
| `kRules` | `std::array<Rule, N>` sorted by Lean theorem name; `kRuleCount`, `kSchema`, `kLeanVersion`, `kSourceSha256`. |
| `find_rule(name)` | The rule with that fully qualified Lean name, or `nullptr`. |
| `verify_all(Report* = nullptr)` | Re-checks every rule at run time. |
| `fold_phasor_angles(a, b)` | Exists iff `SIMMYNET_VERIFIED_HAS_SO2_ANGLE_ADD`: `AlgebraicTuple<T>` angle sum justified by $R(a)R(b) = R(a+b)$. |
| `frobenius_fold<P>(x)` | Exists iff `SIMMYNET_VERIFIED_HAS_FROBENIUS_FIXED`: $x^P \equiv x \pmod P$, with `static_assert(is_prime_trial(P))`. |
| `reduce_srs_to_rinv(w, n)` | Exists iff `SIMMYNET_VERIFIED_HAS_DIHEDRAL_SRS_INV`: in-place peephole $s\,r\,s \to r^{-1}$ on words over `{r, R, s}`. |

### C ABI (`bridges/lean4/simmy_lean_ffi.cpp`, shared library `simmynet_lean`)

`extern "C"`, unboxed `uint64_t` / `int64_t` / `double` only, pure, never throwing; errors are in-band (`INT64_MIN`, NaN, or bit 63 of the Weil mask).

| Function | Wraps |
|---|---|
| `simmy_legendre_count_points(a, b, p)` | `ff::legendre_count_points` |
| `simmy_frobenius_trace(a, b, p)` | `EllipticCurveNode::compute_trace` |
| `simmy_frobenius_trace_power(a, b, p, n)` | `LefschetzTracePass::trace_of_power` |
| `simmy_extension_point_count(a, b, p, n)` | `LefschetzTracePass::count_points` |
| `simmy_hasse_bound_holds(t, q)` | `ff::hasse_bound_holds` |
| `simmy_hasse_margin(t, q)`, `simmy_sato_tate_cos(t, q)` | $2\sqrt q - \lvert t\rvert$ and $t / (2\sqrt q)$ |
| `simmy_weil_certify(a, b, p)` | `WeilProver::certify` as a bit mask |

