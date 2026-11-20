# SimmyNet (`simmynet`) v2.2.0

[![Documentation](https://img.shields.io/badge/docs-simmynet--compiler-2ea44f?style=for-the-badge&logo=readthedocs&logoColor=white)](https://mohamedeh-ab.github.io/simmynet-dsl-compiler/)

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![CMake 3.18+](https://img.shields.io/badge/CMake-3.18%2B-brightgreen.svg)](https://cmake.org/)
[![Python 3.9+](https://img.shields.io/badge/Python-3.9%2B-orange.svg)](https://www.python.org/)
[![License: MIT](https://img.shields.io/badge/License-MIT-green.svg)](LICENSE)
[![DOI](https://zenodo.org/badge/1453389569.svg)](https://doi.org/10.5281/zenodo.24123292)


**A compiler for tiny, exact, group-theoretic computations.** SimmyNet compiles symbolic group-theoretic boolean classifiers (words over $D_4$, the Klein 4-group, $SO(2)$ phasors, ...) and exact finite-field zeta checks (Frobenius / Lefschetz / Hasse–Weil) into constant-time, allocation-free kernels. An optional [Lean 4 bridge](bridges/lean4/README.md) connects it to the `mathlib` proof assistant in both directions.

## The problem and the solution

Deciding whether a group word $g_1 g_2 \cdots g_L$ lands in a subgroup normally costs $L$ small matrix products per query, and angle identities such as $R(\pi/4)R(\pi/4) = R(\pi/2)$ are checked with floating-point tolerances that drift as compositions pile up. Elliptic-curve invariants ($\\#E(\mathbb{F}_{q^n})$, the zeta function) have the opposite problem: they are exact integers that overflow silently in naive code.

SimmyNet moves all the algebra into the compile phase:

1. **Exact frontend.** The parser carries numbers as `AlgebraicTuple` $c_{base} + c_\pi\,\pi$, so `pi/4 + pi/4 == pi/2` holds by construction and phasor identities are proved modulo $2\pi$ with no tolerance.
2. **Lift synthesis.** If a labelled point set is not linearly separable, a perceptron cycling certificate proves it, and a search over a basis-function library finds the smallest non-linear lift (for example $z = \lvert x - y\rvert$ for XOR).
3. **Constant folding into fixed-size kernels.** The generator matrices of a word are multiplied once at compile time into a single $k \times k$ matrix ($k \le 4$, `alignas(64)` buffers, unrolled kernels), so a decision costs **one matrix pass whatever the word length**: roughly 7–9 ns from $L = 3$ to $L = 8192$.
4. **Exact finite-field engine (`simmynet::ff`).** The Frobenius of $y^2 = x^3 + ax + b$ becomes an `int64` $2 \times 2$ matrix; point counts over every $\mathbb{F}_{q^n}$, the zeta function and Hasse–Weil / functional-equation certificates follow in overflow-checked integer arithmetic.

## Why existing tools fall short

| Tool family | What it does well | Why it misses this design point |
|---|---|---|
| **ML / tensor compilers** (XLA, TorchInductor, TVM) | Large dense matmuls and fused kernels | Group generators are $2 \times 2$ to $4 \times 4$. At that size graph dispatch, kernel launch and Python-side overhead dwarf the arithmetic, and repeated floating-point compositions accumulate rounding error. |
| **Computer algebra systems** (SymPy, SageMath, GAP) | Exact symbolic group theory and finite fields | Expressions are interpreted at run time with heavy object/tree allocation, so a sub-10-nanosecond hot path is out of reach. |
| **General AOT compilers** (C++/LLVM) | Excellent scalar and SIMD loops | No notion of $R(a)R(b) = R(a+b) \pmod{2\pi}$, of perceptron separability, or of the Hasse–Weil bound $t^2 \le 4q$; they cannot fold what they cannot prove. |

**SimmyNet bridges both sides.** Exact symbolic algebra (`AlgebraicTuple`, word folding, lift discovery) runs strictly at compile time; what remains is a cache-resident, zero-allocation SIMD-friendly kernel that sees only doubles. The design is explained in [Architecture](docs/architecture.md).

## Quickstart

**Python**

```python
import simmynet as sn
from simmynet import Generator, finite_field as ff

r, s = Generator.rotation("r", 90), Generator("s", [[-1, 0], [0, 1]])
w = (r * s) ** 4096 * r * s * r        # lazy: 12,289 factors, zero C++ calls so far
print(w.decide([0, 0, 0, 1], -0.5))    # JIT: folds ONCE into one 2x2 matrix, caches it -> True
cw = sn.CompiledWord(2, [0, -1, 1, 0, -1, 0, 0, 1] * 4096)     # explicit: fold once ...
print(cw.decide([0, 0, 0, 1], -0.5), cw.length)               # ... decide with 0 matrix passes
print(sn.phasor_check(["pi/4", "pi/4"], ["pi/2"]).exact)       # True: proved exactly, no tolerance
e = ff.EllipticCurve(2, 3, 97).with_computed_trace()           # exact O(p) Legendre count, trace -2
print(e.trace, ff.certify(ff.lift(e)).hasse.verdict)           # -2 Verdict.PROVED
```

`Word` is the lazy word type (`Generator * Generator` returns one); `ff.EllipticCurve(2, 3, 97).with_trace(20)` raises `ValueError` because it violates the Hasse–Weil bound.

**C++17 (header-only)**

```cpp
#include <iostream>
#include <simmynet/Interpreter.hpp>
#include <simmynet/finite_field/WeilProver.hpp>

int main() {
  simmynet::SimmyNetInterpreter interp(10.0, &std::cout);
  simmynet::Report r = interp.run(R"(
    group D4 {
      gen r = rotation(90)
      gen s = matrix([[-1, 0], [0, 1]])
      subgroup D = span(r, s)
      subgroup H = span(s)
      separate InH { weight = [0, 0, 0, 1], bias = -0.5 }
      classify InH of r * s * r expect 1
      verify InH = H over D
    })");                                                 // r.passed == 9, r.failed == 0

  using namespace simmynet::ff;
  auto node = FrobeniusGenerator::lift(EllipticCurveNode(2, 3, 97).with_computed_trace());
  std::cout << to_string(WeilProver::certify(*node).hasse.verdict) << "\n";   // PROVED
}
```

Compile with `g++ -std=c++17 -I include main.cpp`, or link the CMake target `SimmyNet::native_cpp`. The command line tool: `simmynet_cli [-v|-q] [program.sym ...]`.

## Installation

```bash
pip install simmynet                       # portable prebuilt wheel, Python >= 3.9

# local build tuned for THIS CPU (AVX2/-march=native; not portable to older CPUs)
CMAKE_ARGS="-DSIMMYNET_NATIVE_ARCH=ON" pip install .

# from source with CMake (native arch is ON by default for local CMake builds)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
PYTHONPATH=build/python python -c "import simmynet; print(simmynet.__version__)"   # 2.2.0

# portable local wheel + audits
pip install build && python -m build
python scripts/check_wheel.py --allow-linux-tag dist/*
```

Portable wheels never use `-march=native`, link the C++ runtime statically (no VC++ redistributable, no `libstdc++` dependency) and are built, audited and tested for Linux, macOS and Windows by [`.github/workflows/wheels.yml`](.github/workflows/wheels.yml). CMake options: `SIMMYNET_NATIVE_ARCH`, `SIMMYNET_STATIC_RUNTIME`, `SIMMYNET_STRICT_FP`, `SIMMYNET_BUILD_PYTHON`, `SIMMYNET_BUILD_CLI`, `SIMMYNET_BUILD_TESTS`.

## Lean 4 (`mathlib`) ⇄ C++ SIMD ⇄ Python: a formal-to-native pipeline

SimmyNet computes fast, exact, *instance-level* answers; a proof assistant establishes statements for *all* parameters. The optional bridge in [`bridges/lean4/`](bridges/lean4/README.md) connects the two without touching the DSL, the parser, or the Python build:

```
 Lean 4 (mathlib)                      C++17 (SIMD)                              Python
 @[simmy_rule] theorems  ──────────▶  LeanVerifiedRules.hpp (constexpr)  ───▶  simmynet wheel
   kernel-checked, ∀ params            rewrite hooks + static_assert
 #eval / witness search  ◀──────────  libsimmynet_lean.so (C ABI)  ◀────────  simmynet::ff
   + certificate check in Lean         exact Legendre count, Hasse–Weil, Frobenius
```

- **Lean → SimmyNet.** A Mathlib theorem $\mathrm{lhs} = \mathrm{rhs}$ tagged `@[simmy_rule]` is lowered from its `Lean.Expr` to JSON; `lean_to_cpp.py` turns that into a deterministic, header-only rule table. Each rule records its Lean theorem name, module and concrete type tag (`finite_field`, `dihedral`, `so2_phasor`, `algebraic_tuple`) in comments and `static_assert`s, and is re-evaluated at compile time. Shipped examples: the dihedral relation $s\,r\,s = r^{-1}$, the $SO(2)$ angle addition $R(a)R(b) = R(a+b)$, and the Frobenius identity $x^p = x$ in $\mathbb{Z}/p$.
- **SimmyNet → Lean.** The Legendre point counter, the Hasse–Weil check and the Frobenius traces are exposed as unboxed C functions that Lean declares with `@[extern]`. They are **untrusted oracle primitives** for `#eval` and witness search; Lean accepts an answer only after re-checking a *certificate*: by the kernel (`decide` on a concrete instance) or by compiled Lean code at run time.

The bridge is opt-in (`-DSIMMYNET_BUILD_LEAN_FFI=ON`, default `OFF`) and leaves every default target unchanged. Lean complements SimmyNet rather than replacing it: Lean proves that a rewrite is valid, SimmyNet runs it in nanoseconds on compiled kernels, and Lean can audit what SimmyNet reports.

## Benchmarks and guarantees

| Invariant | Value | Enforced by |
|---|---|---|
| Folded word evaluation | one $k \times k$ pass, independent of word length $L$; about 9 ns on the author's x86_64 machine, 6.9 ns at $L = 8192$ on a 2.1 GHz Xeon (verification run); per-factor baseline 39–53 µs, dense Kronecker 101–145 µs | `tests/cpp/test_engine.cpp` (`unit_tests`) |
| Tape instruction size | `sizeof(ExprProgram::Instr) == 16` | `static_assert` in `AST.hpp` |
| Dyadic multiples of $\pi$ | angle sums exact in 100% of trials (plain double accumulation: 26–68%) | `tests/cpp/test_algebraic_tuple.cpp` |
| Finite-field arithmetic | overflow-checked `int64`; a result that cannot be certified is reported, never wrapped; Hasse–Weil bound compared exactly in 128 bits | `tests/cpp/finite_field_test.cpp` (649,395 checks, also under ASan/UBSan) |
| Point counting | exact $O(p)$ Legendre-symbol count for primes $5 \le p \le 10^6$ (about 10 ms at $p = 999983$); **not** Schoof, which is not implemented | `EllipticCurveNode::count_points()` |
| Trace validation | `with_trace(t)` rejects $t^2 > 4q$ (`ValueError` in Python; unresolved in C++, `try_with_trace` returns `nullopt`) | `tests/python/test_bindings.py` |

Timings depend on the machine; reproduce them with `build/tests/unit_tests`.

## Documentation

- [Documentation home](docs/index.md)
- [Architecture deep-dive](docs/architecture.md): how the compiler works, bottom to top
- [DSL reference](docs/dsl-reference.md) · [Python API](docs/python-api.md) · [C++ API](docs/cpp-api.md)
- [Lean 4 bridge](docs/lean4-bridge.md): the formal-to-native pipeline, its trust model, and [`bridges/lean4/README.md`](bridges/lean4/README.md) for setup and verification commands

## Limitations

- $F^n$ is not $O(1)$ in $n$ (the output has $\Theta(n \log q)$ bits); results leaving `int64` are reported as `nullopt` / `OverflowError`.
- `AlgebraicTuple` is exact on its stored coefficients: dyadic multiples of $\pi$ are exact, non-dyadic ones (`pi/3`) round like a double.
- Fixed-size kernels cover $k \in \{2, 3, 4\}$; subgroup closure stops at 2048 elements.
- The Legendre counter is limited to primes $\le 10^6$; for larger $q$ supply the trace yourself (`with_trace` or a `TraceResolver`).

## Citation

If you use SimmyNet in academic work, please cite it using [`CITATION.cff`](CITATION.cff).

## License

MIT, © 2026 MohamedEh/-/b. See [LICENSE](LICENSE).
