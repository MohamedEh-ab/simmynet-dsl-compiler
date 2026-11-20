"""Tests for simmynet.wrapper: deferred execution, JIT caching, DSL string builders.

Run through CTest (PYTHONPATH = <build>/python) or directly:  python3 tests/python/test_wrapper.py [dir]
"""
import glob
import os
import sys
import threading
import time


def _import_package():
    try:
        import simmynet
        return simmynet
    except ImportError:
        pass
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    candidates = ([sys.argv[1]] if len(sys.argv) > 1 else []) + [os.environ.get("SIMMYNET_MODULE_DIR", "")]
    for pattern in ("build/python", "build/*/python", "python"):
        candidates += glob.glob(os.path.join(root, pattern))
    for d in candidates:
        if d and os.path.isdir(os.path.join(d, "simmynet")):
            sys.path.insert(0, d)
            try:
                import simmynet
                return simmynet
            except ImportError:
                sys.path.pop(0)
    raise SystemExit("cannot import simmynet: build it (cmake --build build) or pass the directory containing it")


sn = _import_package()
from simmynet import (DSLGroup, DSLProgram, DSLWord, Generator, Word, phasor_check, reset_stats,  # noqa: E402
                           stats, synthesize_space, synthesize_subgroup)


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print("PASS", msg)


def raises(exc, fn):
    try:
        fn()
    except exc:
        return True
    return False


W = [0, 0, 0, 1]  # InH hyperplane over vec(W_compiled), bias -0.5
r = Generator.rotation("r", 90)
s = Generator("s", [[-1, 0], [0, 1]])

# =============================================================================================
# 1. Graph builder: lazy, zero C++ crossings
# =============================================================================================
reset_stats()
sn.reset_gemm_count()
w = r * s * r
check(len(w) == 3 and [g.name for g in w] == ["r", "s", "r"], "r * s * r builds a lazy word of 3 factors")
check(stats() == {"word_compiles": 0, "word_decides": 0, "dsl_flushes": 0} and sn.gemm_count() == 0 and not w.is_compiled,
      "building words crosses the C++ boundary zero times (no compile, no matrix pass)")
check([g.name for g in (r @ s @ r)] == ["r", "s", "r"] and [g.name for g in (r + s + r)] == ["r", "s", "r"],
      "@ and + build the same word as *")
check(len((r * s) ** 4) == 8 and len(r ** 3) == 3 and len(r ** 0) == 0 and (r ** 0).dim == 2,
      "** repeats (exponentiation by squaring on shared sub-words); ** 0 is the identity word")
check(raises(ValueError, lambda: r ** -1), "negative exponents are rejected (no hidden inverse math)")
check(raises(ValueError, lambda: r * Generator.identity(3)), "mixing 2x2 and 3x3 generators is rejected at build time")
w2 = w * s
check(len(w) == 3 and len(w2) == 4, "words are immutable: extending one makes a new word")
t0 = time.perf_counter()
big = Word.identity(2)
for _ in range(200000):
    big = big * r
check(len(big) == 200000 and len(big.factors) == 200000 and time.perf_counter() - t0 < 3.0,
      "a 200000-factor word builds in O(1) per operator and flattens iteratively (no recursion limit)")
check(stats()["word_compiles"] == 0, "still no C++ compilation after building big words")
check("r * s * r" in repr(w) and "length 3" in repr(w), "repr shows the word")

# =============================================================================================
# 2. JIT lowering + transparent caching
# =============================================================================================
reset_stats()
sn.reset_gemm_count()
check(w.decide(W, -0.5) is True and stats()["word_compiles"] == 1 and w.is_compiled, "first decide() compiles the word once and decides")
check(sn.gemm_count() == 3, "the C++ fold of r*s*r cost exactly 3 matrix passes, once")
sn.reset_gemm_count()
results = {w.decide(W, -0.5) for _ in range(1000)}
check(results == {True} and stats()["word_compiles"] == 1 and stats()["word_decides"] == 1001 and sn.gemm_count() == 0,
      "1000 further decisions reuse the cached C++ artifact: 0 recompiles, 0 matrix passes")
check(w.matrix == [-1.0, 0.0, 0.0, 1.0], "W_compiled of r*s*r is s (cached; no recompile)")
check(stats()["word_compiles"] == 1, "reading .matrix did not recompile")
w3 = w * r  # r*s*r*r folds to s*r, which is not in H
check(not w3.is_compiled and w3.decide(W, -0.5) is False and stats()["word_compiles"] == 2 and w.is_compiled,
      "a derived word compiles on its own; the original cache is untouched")
check(w2.decide(W, -0.5) is True, "r*s*r*s folds to s*s = identity, which is in H")
check(w.decide(W, -0.5) == sn.eval_word_compiled(2, list(r._flat + s._flat + r._flat), W, -0.5) == w.decide_once(W, -0.5),
      "agrees with eval_word_compiled and decide_once")
check((Word.identity(2)).decide(W, -0.5) is True, "the identity word is in H = {e, s}")

long_word = (r * s) ** 4096
reset_stats()
sn.reset_gemm_count()
long_word.decide(W, -0.5)
check(len(long_word) == 8192 and sn.gemm_count() == 8192 and stats()["word_compiles"] == 1, "folding an 8192-factor word costs one compile")
sn.reset_gemm_count()
for _ in range(10000):
    long_word.decide(W, -0.5)
check(sn.gemm_count() == 0 and stats()["word_compiles"] == 1, "10000 decisions on it: 0 matrix passes, still 1 compile (O(1) in word length)")

fresh = (r * s) ** 50
reset_stats()
threads = [threading.Thread(target=lambda: [fresh.decide(W, -0.5) for _ in range(50)]) for _ in range(8)]
for t in threads:
    t.start()
for t in threads:
    t.join()
check(stats()["word_compiles"] == 1 and stats()["word_decides"] == 400, "8 threads racing on a fresh word compile it exactly once")
check(raises(ValueError, lambda: fresh.decide([1.0], 0.0)), "wrong weight length is reported by the core")

# =============================================================================================
# 3. DSL string builders: flush exactly once
# =============================================================================================
def d4_group():
    g = DSLGroup("D4")
    rr = g.generator("r", rotation=90)
    ss = g.generator("s", matrix=[[-1, 0], [0, 1]])
    g.subgroup("D", [rr, ss])
    g.subgroup("H", [ss])
    g.separate("InH", [0, 0, 0, 1], -0.5)
    g.eval(rr * ss * rr)
    g.classify("InH", word=rr * ss * rr, expect=1)
    g.verify("InH", "H", "D")
    return g


EXPECTED = """group D4 {
    gen r = rotation(90)
    gen s = matrix([[-1, 0], [0, 1]])
    subgroup D = span(r, s)
    subgroup H = span(s)
    separate InH { weight = [0, 0, 0, 1], bias = -0.5 }
    eval r * s * r
    classify InH of r * s * r expect 1
    verify InH = H over D
}
"""
reset_stats()
g = d4_group()
check(g.source() == EXPECTED and str(g) == EXPECTED and stats()["dsl_flushes"] == 0, "the builder renders exact DSL text and flushes nothing while building")
res = g.run()
check(res.passed == 9 and res.failed == 0 and res.x_failed == 0 and stats()["dsl_flushes"] == 1, "run(): the C++ interpreter passes 9/9 on the generated program (one flush)")
check(g.run() is res and stats()["dsl_flushes"] == 1, "run() again returns the cached result: no second flush")
check(raises(RuntimeError, g.compile), "compile() after a flush is refused (flush exactly once)")
check(raises(RuntimeError, lambda: g.eval("e")), "editing a flushed group is refused")
check("[runtime]" in d4_group().run(transcript=True).transcript, "run(transcript=True) captures the transcript")

reset_stats()
bad = DSLGroup("Bad")
bad.generator("r", rotation=90)
bad.classify("Nope", point=(0, 0))
check(raises(sn.CompileError, bad.run) and stats()["dsl_flushes"] == 0, "a compile error maps to CompileError and does not consume the flush")
check(raises(sn.CompileError, bad.run), "the builder can be retried after a failed flush")

# injection / validation guards
gg = DSLGroup("V")
check(raises(ValueError, lambda: gg.generator("bad name", rotation=1)), "generator names are validated")
check(raises(ValueError, lambda: gg.generator("e", rotation=1)) and raises(ValueError, lambda: gg.generator("class", rotation=1)),
      "reserved DSL words cannot be used as names")
check(raises(ValueError, lambda: gg.lift("z", "x}")) and raises(ValueError, lambda: gg.lift("z", "1, 2"))
      and raises(ValueError, lambda: gg.lift("z", "x # y")) and raises(ValueError, lambda: gg.lift("z", "x\ngroup")),
      "expression strings cannot smuggle braces, commas, comments or newlines")
check(raises(ValueError, lambda: DSLGroup("1x")) and raises(TypeError, lambda: gg.lift("w", True)), "group names and expression types are validated")
gg.generator("r", rotation=1)
check(raises(ValueError, lambda: gg.generator("r", rotation=2)), "duplicate declarations are rejected in Python")
check(raises(ValueError, lambda: gg.generator("q")) and raises(ValueError, lambda: gg.classify("C")) and raises(ValueError, lambda: gg.synthesize("S")),
      "missing arguments are reported")
check(raises(ValueError, lambda: gg.sweep("C", gg.param_generator("R"), 0, "pi", 0)), "sweep steps must be positive")

# words in DSL form
g2 = DSLGroup("W")
a = g2.generator("a", rotation=90)
b = g2.generator("b", rotation=45)
R = g2.param_generator("R", "theta", "phasor")
check(str(a * b ** 2 * a ** -1) == "a * b^2 * a^-1" and str(R("pi/4") * a) == "R(pi/4) * a" and str((a * b) ** 2) == "a * b * a * b",
      "DSLWord renders products, exponents and parametrised factors")
check(raises(TypeError, lambda: R * a), "a parametrised family needs an argument")

# sweeps through the builder
g3 = DSLGroup("Sweep")
R3 = g3.param_generator("R")
g3.sector("Wedge", "pi/4", "3*pi/4")
g3.sweep("Wedge", R3, 0, "2*pi", 8)
r3 = g3.run()
check(r3.passed == 8 and r3.failed == 0, "sweep via the builder: 8/8 angles agree with the analytic arc test")

# DSLProgram: several groups, still one flush
reset_stats()
pa, pb = d4_group(), DSLGroup("Second")
pb.generator("t", rotation=180)
pb.eval(pb.generator("u", rotation=90) * pb.generator("v", rotation=90))
prog = DSLProgram(pa, pb)
pres = prog.run()
check(pres.passed == 9 and stats()["dsl_flushes"] == 1 and raises(RuntimeError, lambda: pa.eval("e")), "DSLProgram flushes all its groups once and freezes them")

# =============================================================================================
# High-level helpers: one flush, plain Python result, C++ snapshot dropped
# =============================================================================================
reset_stats()
xor = synthesize_space([((0, 0), 0), ((1, 0), 1), ((0, 1), 1), ((1, 1), 0)])
check(xor.lifts == ["k1 = abs(x - y)"] and xor.weight == [0.0, 0.0, 1.0] and xor.bias == -0.5 and xor.separation == 1.0 and xor.n_points == 4,
      "synthesize_space(XOR): the C++ synthesizer found k1 = abs(x - y), w = [0, 0, 1], b = -0.5, 100% separation")
check(stats()["dsl_flushes"] == 1 and any("minimal feature map" in line for line in xor.log) and "abs(x - y)" in str(xor),
      "exactly one flush; the [synth] log is returned as plain strings")
check(synthesize_space([((0, 0), 0), ((1, 0), 0), ((0, 1), 1), ((1, 1), 1)]).lifts == [], "a linearly separable set needs no lift")

klein = synthesize_subgroup({"a": [[-1, 0], [0, 1]], "b": [[1, 0], [0, -1]], "c": [[-1, 0], [0, -1]]}, subgroup=["c"], over=["a", "b"])
check(len(klein.lifts) == 1 and klein.separation == 1.0 and klein.n_points == 4, "synthesize_subgroup: Klein 4-group needs one lift, 100% separation")
coset = synthesize_subgroup({"a": [[-1, 0], [0, 1]], "b": [[1, 0], [0, -1]], "c": [[-1, 0], [0, -1]]}, subgroup=["c"], over=["a", "b"], coset=["a"], name="Coset")
check(coset.name == "Coset" and coset.separation == 1.0, "synthesize_subgroup with a left coset")

o = phasor_check(["pi/4", "pi/4"], ["pi/2"])
check(o.passed and o.exact is True and o.lhs_angle == "0.5*pi" and o.rhs_angle == "0.5*pi", "phasor_check: R(pi/4)*R(pi/4) == R(pi/2) is PROVED exactly (0.5*pi == 0.5*pi)")
o = phasor_check(["pi/4", "pi/4"], ["pi/3"])
check(not o.passed and o.exact is False, "phasor_check: R(pi/4)^2 vs R(pi/3): not provable, numerically different -> fails")
o = phasor_check(["2*pi"], [])
check(o.passed and o.exact is True, "phasor_check: R(2*pi) == identity is proved (angle 2*pi is 0 mod 2*pi)")

print("all simmynet wrapper tests passed")
