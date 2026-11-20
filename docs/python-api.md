---
title: Python API
layout: default
nav_order: 4
---

# Python API (`simmynet`)

```python
import simmynet as sn
from simmynet import dsl, finite_field as ff
```

`simmynet` is a package: `simmynet/__init__.py` re-exports the compiled core `simmynet._core` and the pure-Python wrapper `simmynet.wrapper`. Requires Python ≥ 3.9. `sn.__version__` is `"2.2.0"`.

- [Top level](#top-level) · [Exceptions](#exceptions) · [Wrapper layer (lazy / deferred)](#wrapper-layer-deferred-execution) · [`simmynet.dsl`](#simmynetdsl) · [`simmynet.finite_field`](#simmynetfinite_field)

## Top level

| Name | Signature | Returns | Description |
|---|---|---|---|
| `eval_word_compiled` | `(k, factors, weight, bias)` | `bool` | $\mathrm{Step}(w \cdot \mathrm{vec}(W\_{\text{compiled}}) + b)$ for a word. `factors` is $L$ row-major $k \times k$ matrices concatenated in word order (empty list = identity); `weight` has $k^2$ entries; `k` ∈ {2, 3, 4}, otherwise `ValueError`. Folds on every call. |
| `CompiledWord` | `(k, factors)` | object | A word folded once into a single $k \times k$ matrix. |
| `Matrix2x2`, `Matrix3x3`, `Matrix4x4` | | classes | Fixed-size row-major `double` matrices. |
| `gemm_count()` / `reset_gemm_count()` | | `int` / `None` | Matrix-multiplication passes executed so far (shared by every submodule). |
| `dsl`, `finite_field`, `wrapper` | | submodules | Also importable as `simmynet.dsl`, `simmynet.finite_field`. |
| `__version__` | | `str` | `"2.2.0"` |

**`CompiledWord`**: properties `dim`, `length` (number of factors folded), `matrix` ($W\_{\text{compiled}}$, row-major list); method `decide(weight, bias) -> bool` with **0 matrix passes**.

**`Matrix2x2` / `Matrix3x3` / `Matrix4x4`** (replace $N$ by 2, 3 or 4)

| Member | Kind | Description |
|---|---|---|
| `Matrix()` | constructor | Identity matrix. |
| `identity()`, `zero()` | static | Identity / all-zero matrix. |
| `from_list(flat)` | static | Build from $N^2$ row-major numbers; `ValueError` otherwise. |
| `dim` | class property | $N$ (read from the class, e.g. `Matrix3x3.dim`). |
| `to_list()` | method | Row-major list of $N^2$ floats. |
| `m[i, j]`, `m[i, j] = v` | item access | Get/set; `IndexError` if out of range. |
| `a * b`, `a @ b` | operators | Matrix product (counts one pass). |
| `apply(x)` | method | Matrix-vector product with a length-$N$ list. |
| `max_abs_diff(other)` | method | $\max\_{ij} \lvert a\_{ij} - b\_{ij} \rvert$. |
| `a == b` | operator | Exact element-wise equality. |

## Exceptions

| Exception | Raised for |
|---|---|
| `SimmyNetError` | Base class of the four below. |
| `LexError` | Unexpected character in DSL source. |
| `ParseError` | Syntax error. |
| `CompileError` | Unknown names, dimension mismatches, no separating lift, ... |
| `SimmyNetRuntimeError` | Run-time errors, e.g. `Job.decide()` on a job with no hyperplane. |
| `ValueError`, `IndexError` | Bad sizes / indices on matrix types and `finite_field.Mat2`; **`EllipticCurve.with_trace` with a Hasse–Weil-violating trace**; Legendre counting outside its domain. |
| `OverflowError` | A `finite_field` result that does not fit in `int64`. |
| `RuntimeError` | Editing or re-flushing a `DSLGroup` after a successful flush. |

## Wrapper layer (deferred execution)

`simmynet.wrapper` (re-exported from `simmynet`) records what you ask for and enters the C++ core only when a result is needed.

| Pattern | What happens | C++ crossings |
|---|---|---|
| **Graph builder** | `*`, `@`, `+` on `Generator` / `Word` concatenate lazily (O(1) rope nodes, iterative flattening). No arithmetic. | 0 |
| **JIT lowering + cache** | `Word.decide(weight, bias)` flattens once, builds the C++ `CompiledWord` (one fold, $W\_{\text{compiled}}$), caches it in the `Word`; later calls reuse it. | first call: compile + decide; afterwards: 1 per call, O(1) in the word length |
| **DSL string builders** | `DSLGroup` / `DSLProgram` assemble DSL text; the C++ interpreter is entered once by `run()` or `compile()`. | 1 per builder |

### `Generator(name, matrix)`

An immutable $k \times k$ generator ($k = 2, 3, 4$); `matrix` is nested rows or a flat list.

| Member | Description |
|---|---|
| `Generator.identity(k)` | Identity generator. |
| `Generator.phasor(name, radians)`, `.rotation(name, degrees)`, `.reflection(name, degrees)` | Constructors for 2×2 generators. |
| `name`, `k`, `matrix` | Properties. |
| `*`, `@`, `+`, `** n` | Return a `Word`. |

### `Word`

An immutable lazy product; the result of combining generators.

| Member | Description |
|---|---|
| `a * b`, `a @ b`, `a + b` | Concatenation (the group operation; `+` is additive notation for it, **not** matrix addition). Mixing dimensions raises `ValueError`. |
| `w ** n` | Repetition by squaring over shared sub-words; `n = 0` is the identity, negative `n` raises `ValueError`. |
| `Word.identity(k)` | The empty word. |
| `len(w)`, `w.dim`, `w.factors`, iteration | Inspection (`factors` flattens iteratively and caches; no recursion limit). |
| `w.decide(weight, bias=0.0)` | Step(weight · vec(W_compiled) + bias); compiles and caches on first use; thread-safe (one compile even under races). |
| `w.compile()`, `w.is_compiled` | Explicit lowering to a `CompiledWord`; cache flag. |
| `w.matrix` | $W\_{\text{compiled}}$, row-major (cached). |
| `w.decide_once(weight, bias)` | One-shot decision through `eval_word_compiled`; caches nothing (folds per call). |

```python
r = Generator.rotation("r", 90)
s = Generator("s", [[-1, 0], [0, 1]])
w = r * s * r                    # lazy
w.decide([0, 0, 0, 1], -0.5)     # True: r*s*r == s; first call compiles once
```

### `DSLGroup(name)`

Builds `group name { ... }`. Every method appends one statement and validates names and expressions (no braces, commas, comments or newlines can be smuggled into the text).

| Method | DSL statement |
|---|---|
| `generator(name, matrix=… / rotation=… / reflection=… / phasor=…)` → `GenRef` | `gen name = …` |
| `param_generator(name, param="theta", kind="phasor", body=None)` → `GenRef` | `gen R(theta) = phasor(theta)`; use as `R("pi/4")` |
| `lift(name, expr)`, `subgroup(name, gens)`, `separate(name, weight, bias)`, `sector(name, lo, hi)`, `space(name, points)` | declarations |
| `synthesize(name, on=… / subgroup=…, over=…, coset=…)` | `synthesize …` |
| `eval(word, point=None)`, `classify(clf, word=…, point=…, expect=…)`, `verify(clf, subgroup, over)`, `check(lhs, rhs)`, `sweep(clf, family, start, stop, steps)` | queries |
| `source()` / `str(g)` | the DSL text (nothing is compiled) |
| `run(transcript=False, sharpness=10.0)` | flush once → `dsl.RunResult` (cached; a second call returns the same object) |
| `compile(sharpness=10.0)` | flush once → `dsl.Program`; the **caller** owns it, the builder keeps no reference |

A group flushes exactly once: after a successful flush, editing it or flushing it again raises `RuntimeError`. A *failed* flush (e.g. `CompileError`) holds no C++ resources and does not consume the builder. `DSLProgram(*groups)` flushes several groups together (`run`, `compile`, `source`) and freezes them. `GenRef` / `DSLWord` combine with `*`, `@`, `+`, `** n` (a single generator may take negative `n`: `g ** -1`) and render as `r * s^2 * R(pi/4)`.

```python
g = DSLGroup("D4")
rr = g.generator("r", rotation=90)
ss = g.generator("s", matrix=[[-1, 0], [0, 1]])
g.subgroup("D", [rr, ss]); g.subgroup("H", [ss])
g.separate("InH", [0, 0, 0, 1], -0.5)
g.classify("InH", word=rr * ss * rr, expect=1)
g.verify("InH", "H", "D")
g.run().passed                   # ONE flush to the C++ interpreter
```

### Helpers

| Helper | Returns | Description |
|---|---|---|
| `synthesize_space(points, name="Auto")` | `SynthesisResult` | One flush; `points` = `[((x, y), label), …]`. Fields `lifts` (e.g. `['k1 = abs(x - y)']`), `weight`, `bias`, `log`, `n_points`, `separation`. A plain Python object: the C++ `Program` is dropped before returning. |
| `synthesize_subgroup(generators, subgroup, over, coset=None, name="Auto")` | `SynthesisResult` | `generators` maps names to matrices; classifier for `span(subgroup)` (or `coset · span(subgroup)`) inside `span(over)`. |
| `phasor_check(lhs_angles, rhs_angles)` | `CheckOutcome` | Checks `R(a1)·R(a2)·… == R(b1)·…`; fields `passed`, `exact` (`True` proved symbolically, `False` not provable, `None` n/a), `lhs_angle`, `rhs_angle`, `max_abs_diff`. |
| `stats()` / `reset_stats()` | `dict` / `None` | Boundary-crossing counters: `word_compiles`, `word_decides`, `dsl_flushes`. |

## `simmynet.dsl`

| Name | Signature | Returns | Description |
|---|---|---|---|
| `Interpreter` | `(sharpness=10.0)` | object | Lex, parse, compile, fold and run DSL programs. `sharpness` is the slope of the soft sigmoid score (also a property). |
| `Interpreter.run` | `(source, transcript=False)` | `RunResult` | Compile and run every query. `transcript=True` captures the full transcript. |
| `Interpreter.compile` | `(source)` | `Program` | Compile without running. |
| `tokenize` | `(source)` | `list[(kind, text, line)]` | `kind` is `'number'`, `'ident'`, `'symbol'` or `'eof'`. |

| Class | Members |
|---|---|
| `RunResult` | `passed`, `failed`, `x_checked`, `x_failed` (folded-vs-reference equivalence checks / mismatches), `transcript`, `ok` |
| `Program` | `groups` → `list[Group]` |
| `Group` | `name`, `declarations` (compiler log incl. `[synth]` lines), `classifiers` → `list[Classifier]` (sorted by name), `tasks` → `list[Task]` |
| `Classifier` | `name`, `weight`, `bias`, `lifts` (e.g. `['k1 = abs(x - y)']`), `is_sector` |
| `Task` | `kind` (`'job'`, `'verify'` batch, `'check'`), `label`, `jobs` → `list[Job]`; for `check` tasks `exact` (`True` = phasor angle sums proved equal mod $2\pi$, `False` = not provable, `None` = no symbolic proof applies), `lhs_angle`, `rhs_angle` (e.g. `'0.5*pi'`) |
| `Job` | `kind` (`'eval'`/`'classify'`), `label`, `short_label`, `expected`, `input`, `element_mode`, `dim`, `matrix_shape`, `architecture` (unfolded), `folded_architecture`, `folded_factors`, `folded_matrix` ($W\_{\text{compiled}}$); methods `evaluate(trace=False, reference=False) -> ForwardResult`, `decide() -> bool` |
| `ForwardResult` | `input_vector`, `final_vector`, `decision`, `score`, `soft_score` (the last three are `None` for `eval` jobs), `trace` → `list[(label, vector)]` |

The hierarchy is **`Program` → `Group` → `Task` → `Job`**. `Job.evaluate()` runs the constant-folded graph (one $k \times k$ pass); `reference=True` runs the unfolded Kronecker-expanded oracle, which yields the per-layer trace.

```python
prog = dsl.Interpreter().compile(source)
job = prog.groups[0].tasks[0].jobs[0]
job.folded_factors, job.folded_matrix      # constant folding, visible
job.evaluate(trace=True).decision          # one k x k pass
```

## `simmynet.finite_field`

Exact `int64` arithmetic. A result that cannot be certified inside `int64` is never wrapped: functions returning a matrix or scalar raise `OverflowError`; those returning an optional return `None`.

### Elliptic curves and traces

| Name | Signature | Returns | Description |
|---|---|---|---|
| `EllipticCurve` | `(a, b, q)` | object | $y^2 = x^3 + ax + b$ over $\mathbb{F}\_q$ ($a$, $b$ reduced mod $q$); properties `a`, `b`, `q`, `trace` (`None` if unresolved). |
| `EllipticCurve.valid` | `()` | `bool` | $3 \lt q \le 2^{62}$, $q$ odd, $q \bmod 3 \ne 0$, and nonsingular mod $q$. Primality is **not** tested. |
| `EllipticCurve.with_trace` | `(a_q)` | `EllipticCurve` | Copy with a known trace attached. **Raises `ValueError`** if $a\_q^2 \gt 4q$ (Hasse–Weil bound; no elliptic curve over $\mathbb{F}\_q$ has such a trace). The bound is necessary, not sufficient: it does not prove $a\_q$ is the trace of *this* curve. The original curve is left untouched. |
| `EllipticCurve.resolve` | `(resolver)` | `EllipticCurve` | Copy with the trace from `resolver(curve) -> int \| None`. `None` leaves the trace unresolved; a Hasse-violating value **raises `ValueError`**. |
| `EllipticCurve.count_points` | `()` | `int` | $\\#E(\mathbb{F}\_q)$ including the point at infinity, by exact $O(q)$ Legendre-symbol summation $1 + \sum\_x (1 + \chi(x^3 + ax + b))$. **Only for prime $q$ with $5 \le q \le 10^6$** (`MAX_LEGENDRE_MODULUS`); raises `ValueError` for composite or out-of-range $q$ or a singular curve. Not Schoof's algorithm. |
| `EllipticCurve.compute_trace` | `()` | `int` | $a\_q = q + 1 - \\#E(\mathbb{F}\_q)$; same domain and errors. |
| `EllipticCurve.with_computed_trace` | `()` | `EllipticCurve` | Copy with `compute_trace()` attached; same domain and errors. |
| `legendre_trace_resolver` | `(curve)` | `int \| None` | `curve.resolve(legendre_trace_resolver)`; `None` outside the counter's domain. |
| `hasse_bound_holds` | `(t, q)` | `bool` | Exact $t^2 \le 4q$ in 128-bit arithmetic; `False` for $q \le 0$. |
| `MAX_LEGENDRE_MODULUS` | | `int` | `1000000`. |
| `schoof_trace_stub` | `(curve)` | `None` | Placeholder for a future Schoof/SEA resolver: **always** `None`. Schoof/SEA is not implemented. |

```python
e = ff.EllipticCurve(2, 3, 97)
e.count_points(), e.compute_trace()     # (100, -2)
e.with_computed_trace().trace           # -2
e.with_trace(-2).trace                  # -2
e.with_trace(20)                        # ValueError: trace 20 violates the Hasse-Weil bound t^2 <= 4q for q = 97
ff.EllipticCurve(1, 1, 1000003).count_points()   # ValueError: q > 10**6
```

### Frobenius lift and passes

| Name | Signature | Returns | Description |
|---|---|---|---|
| `lift` | `(curve)` | `FrobeniusNode \| None` | $F = \begin{pmatrix}0 & -q\\\\ 1 & a\_q\end{pmatrix}$; `None` if the curve is invalid or the trace is unresolved. |
| `lift_to_engine` | `(curve)` | `Matrix2x2 \| None` | Same lift as a double matrix; `None` if an entry exceeds $2^{53}$. |
| `trace` | `(F)` | `int` | $\mathrm{Tr}\,F$. |
| `determinant` | `(F)` | `int` | $\det F$; `OverflowError` if not exact. |
| `characteristic_polynomial` | `(F)` | `Poly2` | $P(T) = 1 - tT + \det(F)\,T^2$. |
| `power` | `(F, n)` | `Mat2` | $F^n$ (≤ 64 steps); `OverflowError` if a needed intermediate leaves `int64`. |
| `trace_of_power` | `(F, n)` | `int \| None` | $\mathrm{Tr}(F^n)$; `None` on overflow. |
| `count_points` | `(F, n)` | `int \| None` | $\\#E(\mathbb{F}\_{q^n}) = \det(I - F^n)$; `None` for $n = 0$ or if not certifiable. (Not the same as `EllipticCurve.count_points`.) |
| `zeta_collapse` | `(F, q)` | `ZetaNode \| None` | Closed-form zeta function; `None` if $\det F \ne q$ or on overflow. |
| `prove_hasse_weil` | `(F, q)` | `HasseWeilReport` | Exact check of $\det F = q$ and $t^2 \le 4q$. |
| `prove_functional_equation` | `(F, q)` | `FunctionalEquationReport` | Exact check of $Z(1/(qT)) = Z(T)$. |
| `certify` | `(node)` | `WeilCertificate` | Both checks for a `FrobeniusNode`. |

### Classes

| Class | Members |
|---|---|
| `Mat2` | `Mat2()` identity, `Mat2(m00, m01, m10, m11)`, `identity()`, `from_list(flat)`, `to_list()`, `m[i, j]`, `a * b` / `a @ b` / `a - b` (`OverflowError` on overflow), `mul_checked(other) -> (Mat2, overflowed)`, `det()`, `adjugate()`, `transpose()`, `to_engine() -> Matrix2x2 \| None`, `from_engine(m)` (static), `==` |
| `Poly2` | `Poly2(c0, c1, c2)`, `c0`, `c1`, `c2`, `coefficients`, `==`, `str()` |
| `FrobeniusNode` | `FrobeniusNode(F, q)`, `F`, `q` |
| `ZetaNode` | `num`, `den` (both `Poly2`), `str()` → `'(1 + 2T + 97T^2) / (1 - 98T + 97T^2)'` |
| `Verdict` | enum `REFUTED`, `PROVED`; `bool(v)` is `True` for `PROVED` |
| `HasseWeilReport` | `verdict`, `trace`, `det_is_q`, `delta_negative`, `delta_zero`, `overflow` |
| `FunctionalEquationReport` | `verdict`, `inverse_collapse`, `weil_pairing`, `numerator_reflects`, `denominator_reflects`, `overflow` |
| `WeilCertificate` | `hasse`, `functional_equation` |

```python
node = ff.lift(e.with_computed_trace())          # F = [[0, -97], [1, -2]]
ff.count_points(node.F, 3)                       # 912100  = #E(F_97^3)
ff.certify(node).hasse.verdict                   # Verdict.PROVED
```
