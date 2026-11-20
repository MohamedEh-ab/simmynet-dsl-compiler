---
title: Architecture deep-dive
layout: default
nav_order: 2
---

# How SimmyNet works: a compiler for tiny exact algebra

*A technical walkthrough of `simmynet` v2.2.0, from the first character of source to the last nanosecond of the hot path.*

## The design point nobody was aiming at

Take a dihedral word like `r * s * r * s * ... * r` with 8,192 factors and ask one question: is it in the subgroup $\{e, s\}$? Or take the elliptic curve $y^2 = x^3 + 2x + 3$ over $\mathbb{F}_{97}$ and ask for $\#E(\mathbb{F}_{97^5})$ with a proof that the Hasse–Weil bound holds. Both problems are **tiny** ($2 \times 2$ matrices), **exact** (an angle identity is either true or it is not, a point count is an integer), and often **asked millions of times**. Three well-known tool families each miss that combination.

**ML and tensor compilers** (XLA, TorchInductor, TVM) are built for big dense matrix products. For a $2 \times 2$ generator, graph dispatch, kernel launch and Python-side overhead are orders of magnitude larger than the multiplication, and floating-point accumulation drifts when you compose generators thousands of times.

**Computer algebra systems** (SymPy, SageMath, GAP) are exact, and they know group theory and finite fields. But they interpret expressions at run time through trees of heap objects, so a sub-10-nanosecond decision is out of reach by construction.

**General ahead-of-time compilers** (C++/LLVM) schedule scalar and SIMD loops superbly, yet they have no idea that $R(a)R(b) = R(a+b) \pmod{2\pi}$, that a labelled point set may need a non-linear lift to be separable, or that a Frobenius trace must satisfy $t^2 \le 4q$. They cannot fold what they cannot prove.

SimmyNet occupies the gap. **All exact symbolic work happens at compile time**: exact angle algebra, word folding, lift discovery. What is left is a cache-resident, allocation-free kernel that only ever sees `double`s (or, for finite fields, overflow-checked `int64`s). The rest of this article follows the source through that pipeline.

```
DSL source ─► Lexer ─► Parser ─► AST (exact tuples) ─► SimmyNetCompiler ─► Network (unfolded IR)
                                                          │  closure(), KernelSynthesizer
                                                          ▼
                              foldNetwork()  ─► FoldedNetwork (W_compiled, RPN tapes, doubles only)
                                                          ▼
                                       SimmyNetRuntime::decideFast()   (the nanosecond path)
```

---

## 1. Frontend and exact algebra

### Lexing and parsing

`Lexer.hpp` is a single regular expression with six alternatives (comment, whitespace, number, identifier, symbol, mismatch). Anything that matches the last alternative raises `LexError` with the line number. `tokenize()` returns tokens of kind `Number`, `Ident`, `Symbol` and a final `Eof`.

`Parser.hpp` is a plain recursive-descent parser: `program := group*`, `group := 'group' IDENT '{' statement* '}'`, one `parseX()` function per statement keyword, and a three-level expression grammar (`expr` → `term` → `unary`/`atom`). The AST (`AST.hpp`) is a set of small structs and a `std::variant` of twelve statement kinds. Syntax errors are `ParseError`s that name the line and what was found. The complete grammar is in the [DSL reference](dsl-reference.md).

One detail matters for everything that follows: when the parser meets the identifier `pi` (not followed by `(`) it emits `mkPi()`, a number node holding the tuple $\{0, 1\}$. It never calls `std::stod` on a rounded approximation of $\pi$.

### `AlgebraicTuple`: the module $\mathrm{span}\{1, \pi\}$

`algebraic_ring.hpp` defines

```cpp
template <class T = double>
struct AlgebraicTuple { T c_base{}; T c_pi{}; /* = c_base + c_pi * pi */ };
```

and the frontend value type `Exact = ring::AlgebraicTuple<double>`. Addition, subtraction, negation, **scalar** multiplication and scalar division act on the two coefficients directly, and `==` compares the coefficients. So

$$\tfrac{\pi}{4} + \tfrac{\pi}{4} = \{0, \tfrac14\} + \{0, \tfrac14\} = \{0, \tfrac12\} = \tfrac{\pi}{2}$$

is true *by construction*; the library even checks it with a `static_assert`. There is no tolerance anywhere, and $\pi$ is never evaluated until the value is **lowered** with `to_double()` at the boundary of the compiled graph.

What the module cannot do is equally deliberate. There is **no tuple × tuple product**, because $(a + b\pi)(c + d\pi)$ needs $\pi^2$, and no `sin`/`cos` of a tuple, because $\cos(\pi/4) = \sqrt2/2$ is not in $\mathrm{span}\{1,\pi\}$. The expression layer (`exprops` in `AST.hpp`) handles those cases by lowering the operands to `double` and continuing with a plain number. `abs` and `relu` only select a sign, so they keep exactness; `sqrt`, `tanh`, `sin`, `cos` lower first.

Honesty about "exact": it means exact **on the stored coefficients**. Integer and dyadic multiples of $\pi$ ($\pi/2$, $3\pi/8$, ...) are exact in binary floating point, so those sums are exact (100% of the trials in `tests/cpp/test_algebraic_tuple.cpp`, against 26–68% for plain double accumulation). A non-dyadic coefficient such as `pi/3` stores $\mathrm{fl}(1/3)$ and rounds once per operation, like a double.

### Proving $SO(2)$ homomorphisms without a threshold

A phasor generator `R(theta) = phasor(theta)` maps an angle to the rotation matrix $\begin{pmatrix}\cos\theta & -\sin\theta\\ \sin\theta & \cos\theta\end{pmatrix}$, and $R(a)R(b) = R(a+b)$ modulo $2\pi$. When both sides of a `check` consist of phasor factors, the compiler does not multiply any matrices to decide the question. It keeps each factor's angle as an exact tuple (`resolveWord()` evaluates `pi/4` as `{0, 0.25}`), sums the angles per side, and calls

```cpp
ring::equal_mod_2pi(a, b)   // exact on the coefficients
```

which requires equal `c_base` and a difference of `c_pi` that is an **even integer**, with the subtraction itself verified exact by a TwoSum error term (`fmod` by 2 is exact). If it returns `true`, `check R(pi/4) * R(pi/4) == R(pi/2)` is *proved* as `0.5*pi == 0.5*pi`. If it returns `false`, the answer is "not provably equal", and the compiler falls back to a numeric comparison of the folded matrices. The interpreter reports which path decided it.

### Lowering into hyperplane weights and the `ExprProgram` tape

Exact values stay exact all the way through the intermediate representation. A hand-written classifier such as `separate C { weight = [pi/2, 1], bias = -0.5 }` keeps its weights as tuples in the unfolded `Network`; `foldNetwork()` lowers them once with `.to_double()`.

Lift expressions (`lift z = abs(x - y)`) become an `ExprProgram`: a flat reverse-Polish tape built by `ExprProgram::compile()`. Sub-trees with no free variables are folded at compile time **with tuple arithmetic** and only then lowered, so `2*pi/8` becomes a single `Const` instruction.

```cpp
enum class Op : unsigned char { Const, Var, Neg, Add, Sub, Mul, Div, Abs, Sqrt, Relu, Tanh, Sin, Cos };
struct Instr { Op op; int idx; double value; };      // sizeof == 16, static_assert-ed
```

`Instr` is 16 bytes (1-byte opcode, 4-byte index, 8-byte value after padding), so four instructions share a cache line and the tape is read strictly linearly. `run(const double*)` is the hot path: a fixed 64-slot stack (`kMaxStack`), no recursion, no map lookups, no tuples. `run_exact(const Exact*)` executes the same tape on tuples and is for compile-time use only.

---

## 2. Middle-end: closure and lift synthesis

### Subgroup closure

`subgroup H = span(r, s)` is a breadth-first search over right-multiplication by the generators, starting from the identity. Each product is cleaned (`cleanMatrix`, entries below $10^{-12}$ snap to zero) and keyed by its entries rounded to six decimals (`matrixKey`) so that numerically equal matrices are the same element. The search returns every element together with a word that reaches it, and aborts with a `CompileError` beyond `SimmyNetCompiler::MAX_GROUP_ORDER = 2048` elements, which is how an infinite group is detected.

### Is the data linearly separable? The perceptron cycling certificate

Membership questions are turned into labelled data. For `synthesize InH of H over D`, every element $M$ of the ambient group $D$ becomes a point $\mathrm{vec}(M) = (m_{00}, m_{01}, m_{10}, m_{11}) \in \mathbb{R}^4$, labelled 1 if $M \in H$. `KernelSynthesizer::perceptronSeparable()` runs the perceptron algorithm with cycle detection. Separable data make every update strictly increase $w \cdot w^*$ (the Novikoff argument), so a weight vector can never be revisited; if one **is** revisited (compared as an 8-decimal rounded key), the cycling theorem certifies that the data are **not** linearly separable. A cap of 5,000 updates guards the loop.

### Searching for the smallest lift

If the data are not separable, `synthesize()` searches for a feature map $\Phi: \mathbb{R}^n \to \mathbb{R}^{n+k}$:

1. `candidateLibrary()` builds basis functions on the current coordinates: $u v$, $\lvert u - v\rvert$, $\lvert u + v\rvert$, $u^2$, $\lvert u\rvert$, and (for $n \le 5$) cubic products.
2. `candidateColumns()` evaluates each on the data (through `ExprProgram::run`) and drops constants and duplicates.
3. A **flat search** tries every combination of $k = 1, 2, 3$ columns; for each combination `cleanHyperplane()` looks for a separating hyperplane (sparse-support search, then margin normalisation), and the winner has the fewest non-zero weights.
4. If no flat set works, a **composed search** builds a second lift on top of the first (for example $\lvert k_1 - w\rvert$).
5. `finish()` re-evaluates the final hyperplane on every labelled point and raises an internal `CompileError` if it does not reproduce the labels, so a wrong synthesis cannot escape.

This discovers $z = \lvert x - y\rvert$ for XOR and $z = m_{00} \cdot m_{11}$ for the Klein 4-group and $D_4$-centre partitions. If nothing works the compiler says so (`no separating lift found with at most 3 basis functions ...`) rather than returning a hyperplane that does not separate.

---

## 3. Backend: from $O(L)$ to one matrix pass

### Constant folding

`wordLayers()` turns a word into one `LinearLayer` per factor, and `foldNetwork()` is the constant-folding pass:

$$W_{\text{compiled}} = F_1 F_2 \cdots F_L \in \mathbb{R}^{k \times k}$$

computed once, at compile time, with the rightmost factor acting first. Lift expressions become tapes, hyperplane weights are lowered, and the result is a `FoldedNetwork` that contains no tuples and no per-factor layers. A "Linear layer after a non-linear layer" cannot be folded, and the compiler refuses it with a `CompileError` rather than silently producing something different.

Evaluation of a word on a *state* (element mode) is $X \leftarrow W X$ on the $k \times k$ state matrix. That is $O(k^3)$ per layer, instead of building $\mathrm{kron}(W, I)$ and paying $O(k^4)$. The original Kronecker-expanded path survives as `forwardReference()`, and the interpreter cross-checks it against the folded graph on **every** job (agreement to $10^{-9}$), so folding is verified continuously, not assumed.

### What the hot path actually is

For a word-membership decision the folded network reduces to $\mathrm{Step}(w \cdot \mathrm{vec}(W_{\text{compiled}}) + b)$, a $k^2$-element dot product (four multiplications for $k = 2$) and a compare. The pieces that keep it at nanoseconds:

- **Fixed-size kernels.** `gemm`/`gemv` dispatch on $k \in \{1,2,3,4\}$ to fully unrolled templates (`gemmFixed<N>`, `gemvFixed<N>`). `Mat<N>` (`Matrix2x2`, `Matrix3x3`, `Matrix4x4`) is `alignas(32)`.
- **Four independent accumulators** in `dot()`, so the compiler can vectorise and overlap latencies **without** `-ffast-math`.
- **`alignas(64)` buffers** in `WordEval.hpp` (`foldWord`, `evalWordCompiled`, and the `CompiledWord::wc_` member), one cache line, resident in L1.
- **No allocation.** `SimmyNetRuntime::decideFast(net, x, scratch)` reuses a caller-owned `Scratch`; `CompiledWord::decide` uses stack arrays.

Measured on the D4 word $(rs)^{L/2}$, the folded decision stays at about 9 ns from $L = 3$ to $L = 8192$ on the author's machine (6.9 ns on a 2.1 GHz Xeon in the verification run), while the per-factor $O(Lk^3)$ baseline (`decideUnfolded`) takes tens of microseconds and the dense-Kronecker $O(Lk^4)$ baseline (`decideKron`) around 100 µs. Reproduce with `build/tests/unit_tests`; absolute numbers depend on the machine.

By default local CMake builds use `-march=native`; wheels are built portable (`SIMMYNET_NATIVE_ARCH=OFF`), so the same code runs on any x86-64 or arm64 CPU, with the compiler's baseline SIMD.

---

## 4. The exact finite-field engine (`simmynet::ff`)

### Frobenius as a $2 \times 2$ integer matrix

For $E: y^2 = x^3 + ax + b$ over $\mathbb{F}_q$, the $q$-power Frobenius acts on $H^1_{et}(E, \mathbb{Q}_\ell)$, which is 2-dimensional. Its conjugacy class is determined by the trace $t = a_q = q + 1 - \#E(\mathbb{F}_q)$ and $\det = q$, and `FrobeniusGenerator::lift()` emits one representative, the companion matrix of $x^2 - tx + q$:

$$F_q = \begin{pmatrix} 0 & -q \\ 1 & t \end{pmatrix}$$

stored in `Mat2`, a 64-byte-aligned `int64[4]`. Its entries carry no meaning; its conjugation invariants (trace, determinant, characteristic polynomial, traces of powers, the zeta function) are the content.

### Lefschetz and zeta

The Lefschetz formula gives every extension-field count from the one matrix:

$$\#E(\mathbb{F}_{q^n}) = \det(I - F^n) = 1 - \mathrm{Tr}(F^n) + q^n .$$

`LefschetzTracePass::power` computes $F^n$ by branch-free binary exponentiation (at most 64 steps). `ZetaFunctionPass::collapse` never sums the series $\exp(\sum N_n T^n / n)$; it emits the closed form $Z(T) = (1 - tT + qT^2) / ((1 - T)(1 - qT))$ directly.

### Overflow discipline

Every kernel is branch-free and returns a `Checked<T>`: a value plus a **sticky overflow flag** OR-accumulated through the computation. `add_ovf` and `sub_ovf` detect overflow with branch-free sign-bit identities; `mul_ovf` uses `__builtin_mul_overflow` where available and a portable 128-bit path otherwise (`SIMMYNET_FF_PORTABLE_ARITH`). An `overflow` flag means "could not be certified inside `int64`", never a silently wrapped answer: the C++ API returns `nullopt`, Python raises `OverflowError` or returns `None`. (For $q = 97$, $\#E(\mathbb{F}_{97^n})$ stops being certifiable by $n = 40$.)

### Exact Hasse–Weil certification

For $P(T) = 1 - tT + qT^2$ the discriminant is $\Delta = t^2 - 4q$, and the zeros lie on $\lvert T\rvert = q^{-1/2}$ exactly when $\Delta \le 0$, i.e. $t^2 \le 4q$. `WeilProver::prove_hasse_weil` evaluates this with `mul64`, comparing $t^2$ and $4q$ as **128-bit integers**: no square root, no floating point, no overflow even at $q$ near $2^{62}$ (where $4q$ reaches $2^{64}$). It also demands $\det F = q$, because the inequality says nothing about a matrix that is not a Frobenius. `prove_functional_equation` checks the genus-1 identity $Z(1/(qT)) = Z(T)$ through the Weil pairing $F^T J F = qJ$ and the reflection identities for numerator and denominator.

These passes *check*, exactly, that a given $(F, q)$ satisfies Weil's statements; they do not prove anything new about curves.

### Where the trace comes from

The trace is the one input the Frobenius matrix cannot compute for itself, and the library is explicit about its three sources:

| Source | Behaviour |
|---|---|
| `with_trace(t)` | Attach a trace established elsewhere. It is validated with `hasse_bound_holds(t, q)`; an impossible trace is **rejected**. In C++ `with_trace` leaves the trace *unresolved* (so `lift()` returns `nullopt`) and `try_with_trace(t)` returns `nullopt`; in Python `with_trace` raises `ValueError`. The bound is necessary, not sufficient. |
| `count_points()` / `compute_trace()` / `with_computed_trace()` | **Exact $O(p)$ Legendre-symbol counting**, $\#E(\mathbb{F}_p) = 1 + \sum_{x} (1 + \chi(x^3 + ax + b))$, for primes $5 \le p \le 10^6$ only. Primality and nonsingularity are verified; outside the domain C++ returns `nullopt` and Python raises `ValueError`. |
| `resolve(TraceResolver)` | Any external source: a table, an external Schoof/SEA implementation, `legendre_trace_resolver`. |

Why not implement Schoof? It needs polynomial arithmetic modulo division polynomials over $\mathbb{F}_q$ for $q$ up to $2^{62}$ (128-bit modular multiplication, polynomial GCD). That would add the bulk and the overflow risk that the rest of the library is designed to avoid, so **Schoof/SEA is not implemented** (`schoof_trace_stub` is a placeholder that always declines). The Legendre counter chooses a different trade-off: a ~30-line algorithm in which every intermediate stays below $2^{41}$ (so it cannot overflow), one pass over $\mathbb{F}_p$ plus a $p$-byte table (about 10 ms at $p = 999983$), and verified against two independent oracles (a square tally and Euler's criterion) in the test suite.

---

## 5. The Python/C++ boundary

Crossing from Python into C++ costs far more than a $2 \times 2$ product, so the `simmynet.wrapper` layer is designed around **how rarely** it crosses, not how fast the core is once inside. (The bindings do not release the Python GIL: individual core calls last nanoseconds to microseconds, shorter than a release/reacquire cycle.) Three patterns, all observable through `simmynet.stats()`:

**Graph builder (lazy).** `Generator` and `Word` overload `*`, `@` and `+` (concatenation, the group operation; `+` is *not* matrix addition). They do no arithmetic and make no C++ call: concatenation allocates an O(1) rope node, `w ** n` repeats by squaring over shared sub-words, and flattening is iterative (no recursion limit). `(r * s) ** 4096` costs a few Python objects.

**JIT lowering with a cache.** `Word.decide(weight, bias)` flattens the rope once, hands the factors to the C++ `CompiledWord` (one fold into $W_{\text{compiled}}$) and caches it inside the `Word`. Words are immutable, so the cache cannot go stale; compilation is guarded by a lock, so concurrent first calls compile once. Every later call is one C++ call that is $O(1)$ in the word length and runs **0** matrix passes.

**Transactional DSL builders.** `DSLGroup`/`DSLProgram` assemble DSL text in memory (names and expressions are validated so no braces, commas, comments or newlines can be smuggled in) and enter the C++ interpreter exactly once, via `run()` or `compile()`. A *successful* flush freezes the builder (editing or flushing again raises `RuntimeError`); a *failed* flush, for instance a `CompileError`, holds no C++ resources and does not consume it. `run()` returns a small result object; `compile()` returns the C++ `Program` to the caller, who owns it, and the builder keeps no reference. The high-level helpers (`synthesize_space`, `synthesize_subgroup`) copy what they need into plain Python objects and drop the C++ snapshot before returning, so no C++ `Program` is ever orphaned.

---

## Summary

| Layer | Job | Exactness |
|---|---|---|
| Lexer / Parser | source → AST | `pi` is the tuple $\{0, 1\}$ |
| `AlgebraicTuple` | angle algebra | exact on stored coefficients; no tolerance |
| `closure` / `KernelSynthesizer` | subgroup enumeration, lift discovery | cycle certificate of non-separability; result re-verified on the data |
| `foldNetwork` | $F_1 \cdots F_L \to W_{\text{compiled}}$ | one matrix pass at run time |
| `decideFast` / `CompiledWord` | the hot path | doubles only, `alignas(64)`, no allocation |
| `simmynet::ff` | Frobenius, Lefschetz, zeta, Weil | `int64` with sticky overflow flag; 128-bit Hasse–Weil; Legendre count for $5 \le p \le 10^6$ |
| `simmynet.wrapper` | Python scratchpad | deferred: few boundary crossings |

Next: the [DSL reference](dsl-reference.md), the [Python API](python-api.md) and the [C++ API](cpp-api.md).

## Appendix: the formal boundary (optional Lean 4 bridge)

Everything above is *computation*: exact, but only for the parameters actually evaluated. The optional [Lean 4 bridge](lean4-bridge.md) adds the other half. A rewrite such as $R(a)R(b) = R(a+b)$ or $x^p = x$ in $\mathbb{Z}/p$ is proved once in Lean for all parameters, exported as data, and compiled into `static_assert`-guarded headers that exercise the same `AlgebraicTuple` and finite-field arithmetic the engine uses. In the other direction the finite-field engine is exposed to Lean as an **untrusted oracle**: its answer (say a Frobenius trace $t$) becomes usable only after Lean re-checks a certificate such as $t^2 \le 4q$ or a recount of $\#E(\mathbb{F}_p)$. The compile phase described in this document is unchanged; the bridge sits beside it.

