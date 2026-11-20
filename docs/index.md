---
title: Home
layout: default
nav_order: 1
permalink: /
---

# SimmyNet (`simmynet`) v2.2.0

SimmyNet compiles symbolic group-theoretic boolean classifiers and exact finite-field zeta checks into constant-time, allocation-free kernels: a group word of any length becomes **one** $k \times k$ matrix pass (about 7–9 ns), phasor identities are proved **exactly** modulo $2\pi$, and elliptic-curve invariants are computed in overflow-checked `int64` arithmetic.

```bash
pip install simmynet          # Python >= 3.9
```

The documentation has two tracks.

## Track 1: how it works

| Page | What you get |
|---|---|
| [Architecture deep-dive](architecture.md) | A bottom-to-top technical walkthrough: why ML compilers, CAS systems and generic AOT compilers miss this design point, then the exact frontend, lift synthesis, the constant-folded backend, the finite-field engine and the Python/C++ boundary. |

## Track 2: reference

| Page | What you get |
|---|---|
| [DSL reference](dsl-reference.md) | Lexical rules, complete grammar, every statement (`group`, `gen`, `lift`, `subgroup`, `separate`, `sector`, `space`, `synthesize`, `eval`, `classify`, `verify`, `check`, `sweep`), generator kinds, expression functions, error types, exit codes. |
| [Python API](python-api.md) | `simmynet`: `Generator`, `Word`, `CompiledWord`, `DSLGroup`, `simmynet.dsl` (`Interpreter`, `Program` → `Group` → `Task` → `Job`) and `simmynet.finite_field`. |
| [C++ API](cpp-api.md) | The header-only `simmynet` and `simmynet::ff` namespaces: `AlgebraicTuple`, `ExprProgram`, `SimmyNetCompiler`, `SimmyNetInterpreter`, `Matrix2x2`/`Matrix3x3`, `EllipticCurveNode` and the finite-field passes. |
| [Lean 4 bridge](lean4-bridge.md) | The optional two-way pipeline between Lean 4 (`mathlib`) and the C++ engine: kernel-checked rewrite rules compiled into `static_assert`-guarded headers, and SimmyNet's point counter exposed to Lean as an untrusted oracle with certificate checking. |

## At a glance

| Invariant | Value |
|---|---|
| Folded word evaluation | one $k \times k$ pass, independent of word length ($k \le 4$) |
| `sizeof(ExprProgram::Instr)` | 16 bytes (`static_assert`) |
| Dyadic multiples of $\pi$ | angle sums exact in 100% of tested trials |
| `simmynet::ff` | overflow-checked `int64`; exact 128-bit Hasse–Weil comparison |
| Point counting | exact $O(p)$ Legendre count for primes $5 \le p \le 10^6$ (not Schoof) |

An optional bridge connects SimmyNet to the Lean 4 proof assistant (formal rules in, exact oracle out); see the [Lean 4 bridge](lean4-bridge.md). The quickstart, build instructions and tests are in the repository `README.md`. To cite SimmyNet, use `CITATION.cff`.
