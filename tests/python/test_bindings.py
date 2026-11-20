"""Tests for the simmynet Python extension (pybind11).

Run through CTest (which sets PYTHONPATH to the build artifact directory) or directly:

    python3 tests/python/test_bindings.py [path/to/dir/containing/simmynet.so]

If the package is not importable, the script also searches common build directories next to the
repository root (build/python, build/*/python): the directory that contains the `simmynet` package.
"""
import glob
import math
import os
import re
import sys


def _import_module():
    try:
        import simmynet  # noqa: F401
        return simmynet
    except ImportError:
        pass
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    candidates = []
    if len(sys.argv) > 1:
        candidates.append(sys.argv[1])
    candidates += [os.environ.get("SIMMYNET_MODULE_DIR", "")]
    for pattern in ("build/python", "build", "build/*/python", "build/lib*", "python"):
        candidates += glob.glob(os.path.join(root, pattern))
    for d in candidates:
        if d and os.path.isdir(os.path.join(d, "simmynet")):
            sys.path.insert(0, d)
            try:
                import simmynet  # noqa: F401
                return simmynet
            except ImportError:
                sys.path.pop(0)
    raise SystemExit("cannot import simmynet: build the module and set PYTHONPATH "
                     "or pass the directory containing it as the first argument")


sn = _import_module()

R = [0, -1, 1, 0]   # rotation(90)
S = [-1, 0, 0, 1]   # reflection diag(-1, 1)
W = [0, 0, 0, 1]    # InH hyperplane over vec(W_compiled), bias -0.5


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


# --- fixed-size matrices -----------------------------------------------------------------
r, s = sn.Matrix2x2.from_list(R), sn.Matrix2x2.from_list(S)
check((r * s * r) == s, "Matrix2x2: r*s*r == s")
check((r @ r).to_list() == [-1, 0, 0, -1], "Matrix2x2: r@r == -I")
check(sn.Matrix2x2().to_list() == [1, 0, 0, 1], "Matrix2x2(): identity by default")
check(r[1, 0] == 1.0, "Matrix2x2: tuple indexing")
check(r.apply([1.0, 0.0]) == [0.0, 1.0], "Matrix2x2.apply rotates (1,0) to (0,1)")
c, d = math.cos(math.pi / 4), math.sin(math.pi / 4)
q = sn.Matrix2x2.from_list([c, -d, d, c])
h = sn.Matrix2x2.from_list([math.cos(math.pi / 2), -1.0, 1.0, math.cos(math.pi / 2)])
check((q * q).max_abs_diff(h) < 1e-15, "Matrix2x2: R(pi/4)^2 == R(pi/2) within 1e-15")
check(sn.Matrix3x3.dim == 3 and sn.Matrix4x4.dim == 4, "Matrix3x3 / Matrix4x4 report their dimension")
check(sn.Matrix4x4.zero().to_list() == [0.0] * 16, "Matrix4x4.zero()")
m3 = sn.Matrix3x3.from_list([1, 2, 3, 4, 5, 6, 7, 8, 9])
check((m3 * sn.Matrix3x3.identity()) == m3 and (m3 * m3)[0, 0] == 30.0, "Matrix3x3: identity and product")
m4 = sn.Matrix4x4.identity()
m4[2, 3] = 5.0
check((m4 * m4)[2, 3] == 10.0, "Matrix4x4: product and item assignment")
check(raises(ValueError, lambda: sn.Matrix2x2.from_list([1, 2, 3])), "from_list rejects wrong length")
check(raises(IndexError, lambda: r[2, 0]), "indexing rejects out-of-range")

# --- eval_word_compiled ------------------------------------------------------------------
word = R + S + R
check(sn.eval_word_compiled(2, word, W, -0.5) is True, "eval_word_compiled: r*s*r is in H")
check(sn.eval_word_compiled(2, R + S, W, -0.5) is False, "eval_word_compiled: r*s is not in H")
check(sn.eval_word_compiled(2, [], W, -0.5) is True, "eval_word_compiled: empty word is the identity (in H)")
check(raises(ValueError, lambda: sn.eval_word_compiled(5, [], [0] * 25, 0)), "rejects unsupported k")
check(raises(ValueError, lambda: sn.eval_word_compiled(2, [1, 2, 3], W, 0)), "rejects ragged factor list")
check(raises(ValueError, lambda: sn.eval_word_compiled(2, word, [1.0], 0)), "rejects wrong weight length")

# --- CompiledWord: the O(1) folded pass --------------------------------------------------
check(sn.CompiledWord(2, word).matrix == S and sn.CompiledWord(2, word).length == 3,
      "CompiledWord: folds r*s*r into the single matrix s")

L = 8192
long_word = (R + S) * (L // 2)
sn.reset_gemm_count()
cw = sn.CompiledWord(2, long_word)
check(cw.length == L and sn.gemm_count() == L, "CompiledWord: folding %d factors costs %d passes, once" % (L, L))

sn.reset_gemm_count()
decisions = {cw.decide(W, -0.5) for _ in range(10000)}
check(sn.gemm_count() == 0 and len(decisions) == 1,
      "CompiledWord.decide(): 10000 decisions use 0 matrix passes (O(1) in word length)")
check(cw.decide(W, -0.5) == sn.eval_word_compiled(2, long_word, W, -0.5),
      "CompiledWord agrees with eval_word_compiled on a long word")

sn.reset_gemm_count()
sn.eval_word_compiled(2, word, W, -0.5)
check(sn.gemm_count() == 3, "eval_word_compiled folds per call: 3 factors -> 3 passes (use CompiledWord to avoid it)")
check(raises(ValueError, lambda: sn.CompiledWord(2, [1.0, 2.0, 3.0])), "CompiledWord rejects ragged factor list")
check(raises(ValueError, lambda: cw.decide([1.0], 0.0)), "CompiledWord.decide rejects wrong weight length")


# =============================================================================================
# DSL interpreter / compiler  (simmynet.dsl)
# =============================================================================================
from simmynet import dsl                      # noqa: E402
import simmynet.finite_field                  # noqa: E402,F401  (sys.modules registration)
from simmynet import finite_field as ff       # noqa: E402

D4 = """
group D4 {
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
XOR = """
group XORSpace {
    space Points { (0, 0) class 0  (1, 0) class 1  (0, 1) class 1  (1, 1) class 0 }
    synthesize XorAuto on Points
    classify XorAuto on (1, 0) expect 1
    classify XorAuto on (1, 1) expect 0
}
"""
SO2 = """
group S {
    gen R(theta) = phasor(theta)
    check R(pi/4) * R(pi/4) == R(pi/2)
}
"""

interp = dsl.Interpreter()
check(interp.sharpness == 10.0 and dsl.Interpreter(sharpness=4.0).sharpness == 4.0, "dsl.Interpreter(sharpness)")

toks = dsl.tokenize("gen r = rotation(90)")
check(toks[0] == ("ident", "gen", 1) and toks[-1][0] == "eof" and len(toks) == 8, "dsl.tokenize")

res = interp.run(D4)
check(res.passed == 9 and res.failed == 0 and res.x_failed == 0 and res.ok and res.transcript == "",
      "Interpreter.run: D4 program passes 9/9, folded == reference on every job")
check("[runtime]" in interp.run(D4, transcript=True).transcript, "Interpreter.run(transcript=True) captures the transcript")
check(interp.run(XOR).passed == 6, "Interpreter.run: XOR program passes 6/6")

check(issubclass(sn.LexError, sn.SimmyNetError) and issubclass(sn.ParseError, sn.SimmyNetError)
      and issubclass(sn.CompileError, sn.SimmyNetError) and issubclass(sn.SimmyNetRuntimeError, sn.SimmyNetError),
      "exception hierarchy: LexError/ParseError/CompileError/SimmyNetRuntimeError < SimmyNetError")
check(raises(sn.LexError, lambda: interp.run("group G { @ }")), "LexError on a bad character")
check(raises(sn.ParseError, lambda: interp.run("group G {")), "ParseError on an unterminated group")
check(raises(sn.CompileError, lambda: interp.run("group G { classify Nope on (0, 0) }")), "CompileError on an unknown classifier")
check(raises(sn.SimmyNetError, lambda: interp.compile("group G { classify Nope on (0, 0) }")), "errors are catchable as SimmyNetError")

# --- compile + inspect the kernel-synthesis result ---------------------------------------------
prog = interp.compile(XOR)
grp = prog.groups[0]
clf = grp.classifiers[0]
check(grp.name == "XORSpace" and clf.name == "XorAuto" and not clf.is_sector, "Program/Group/Classifier navigation")
check(clf.lifts == ["k1 = abs(x - y)"] and clf.weight == [0.0, 0.0, 1.0] and clf.bias == -0.5,
      "kernel synthesizer found the lift k1 = abs(x - y) with w = [0, 0, 1], b = -0.5")
check(any(line.startswith("[synth]") for line in grp.declarations), "declaration log contains [synth] lines")
verify = grp.tasks[0]
check(verify.kind == "verify" and len(verify.jobs) == 4 and all(j.decide() == j.expected for j in verify.jobs),
      "XOR: 100% separation (4/4 points) through Job.decide()")

# --- constant folding is visible and costs one pass ------------------------------------------------
d4 = interp.compile(D4).groups[0]
jobs = [t.jobs[0] for t in d4.tasks if t.kind == "job"]
ev = next(j for j in jobs if j.kind == "eval")
cl = next(j for j in jobs if j.kind == "classify")
check(ev.folded_factors == 3 and ev.element_mode and ev.dim == 2, "eval r*s*r: 3 factors folded, element mode, k = 2")
check(ev.folded_matrix == [-1, 0, 0, 1], "W_compiled == [[-1, 0], [0, 1]] (r*s*r == s)")
check("L3:r" in ev.architecture and "3 factors folded" in ev.folded_architecture, "architecture vs folded_architecture")
sn.reset_gemm_count()
out = ev.evaluate()
check(sn.gemm_count() == 1 and out.final_vector == [-1, 0, 0, 1] and out.decision is None,
      "Job.evaluate(): the folded word costs exactly 1 matrix pass")
check(len(ev.evaluate(trace=True).trace) == 2 and len(ev.evaluate(reference=True).trace) == 4,
      "folded trace has 2 stages; the unfolded reference trace has input + 3 layers")
sn.reset_gemm_count()
check(cl.decide() is True and sn.gemm_count() == 1 and cl.expected is True, "classify r*s*r in H -> True in one pass")
fold, refr = cl.evaluate(), cl.evaluate(reference=True)
check(fold.decision == refr.decision is True and abs(fold.score - refr.score) < 1e-12 and fold.soft_score > 0.5,
      "folded result == Kronecker-expanded reference")
check(raises(sn.SimmyNetRuntimeError, lambda: ev.decide()), "Job.decide() on an eval job raises SimmyNetRuntimeError")

chk = next(t for t in dsl.Interpreter().compile(SO2).groups[0].tasks if t.kind == "check")
a, b = chk.jobs[0].evaluate().final_vector, chk.jobs[1].evaluate().final_vector
check(max(abs(x - y) for x, y in zip(a, b)) < 1e-15, "check R(pi/4)*R(pi/4) == R(pi/2) holds to < 1e-15")
check(chk.exact is True and chk.lhs_angle == "0.5*pi" and chk.rhs_angle == "0.5*pi",
      "the same check is PROVED symbolically: angle sums 0.5*pi == 0.5*pi (exact, no tolerance)")

# =============================================================================================
# Exact finite-field passes  (simmynet.finite_field)
# =============================================================================================
check(ff is sys.modules["simmynet.finite_field"], "finite_field importable as a submodule")

e = ff.EllipticCurve(2, 3, 97)
check(e.valid() and (e.a, e.b, e.q) == (2, 3, 97) and e.trace is None and ff.lift(e) is None,
      "EllipticCurve: valid, trace unresolved -> lift is None")
check(not ff.EllipticCurve(0, 0, 7).valid(), "singular curve is invalid")
node = ff.lift(e.with_trace(-2))
F = node.F
check(F.to_list() == [0, -97, 1, -2] and node.q == 97, "lift: F = [[0, -q], [1, a_q]]")
check(ff.trace(F) == -2 and ff.determinant(F) == 97 and F.det() == 97, "Tr(F) = a_q, det(F) = q")
P = ff.characteristic_polynomial(F)
check(P.coefficients == [1, 2, 97] and P == ff.Poly2(1, 2, 97) and str(P) == "1 + 2T + 97T^2",
      "P(T) = 1 + 2T + 97T^2")
check([ff.count_points(F, n) for n in (1, 2, 3)] == [100, 9600, 912100], "Lefschetz: #E(F_97^n) for n = 1, 2, 3")
check(ff.trace_of_power(F, 2) == (-2) ** 2 - 2 * 97, "Tr(F^2) = t^2 - 2q")
check(ff.count_points(F, 0) is None and ff.count_points(F, 40) is None, "count_points: None for n = 0 and when not certifiable")
check(str(ff.zeta_collapse(F, 97)) == "(1 + 2T + 97T^2) / (1 - 98T + 97T^2)", "zeta function closed form")
check(ff.zeta_collapse(ff.Mat2(0, -7, 1, 2), 5) is None, "zeta_collapse rejects det(F) != q")

cert = ff.certify(node)
check(cert.hasse.verdict == ff.Verdict.PROVED and bool(cert.hasse.verdict) and cert.hasse.delta_negative
      and cert.hasse.det_is_q and not cert.hasse.delta_zero, "Hasse-Weil PROVED (Delta = -384 < 0)")
fe = cert.functional_equation
check(fe.verdict == ff.Verdict.PROVED and fe.inverse_collapse and fe.weil_pairing and fe.numerator_reflects
      and fe.denominator_reflects and not fe.overflow, "functional equation PROVED (all three checks)")
check(ff.prove_hasse_weil(ff.Mat2(0, -5, 1, 100), 5).verdict == ff.Verdict.REFUTED, "t = 100, q = 5 is REFUTED")
check(ff.prove_functional_equation(ff.Mat2(0, -7, 1, 2), 5).verdict == ff.Verdict.REFUTED, "det(F) != q is REFUTED")
bigq = 2 ** 62 - 57
check(ff.prove_hasse_weil(ff.Mat2(0, -bigq, 1, 4294967294), bigq).verdict == ff.Verdict.PROVED,
      "q ~ 2^62: exact 128-bit comparison, no overflow")

check(e.resolve(lambda c: -2).trace == -2 and ff.lift(e.resolve(lambda c: -2)) is not None,
      "resolve(callable) attaches the trace the callable returns")
check(e.resolve(lambda c: None).trace is None and e.resolve(ff.schoof_trace_stub).trace is None,
      "resolver returning None / schoof_trace_stub leaves the trace unresolved")

# --- trace handling: Hasse-Weil validation + exact O(p) Legendre point counting (NOT Schoof) ---------
check(ff.MAX_LEGENDRE_MODULUS == 10 ** 6, "MAX_LEGENDRE_MODULUS is 10**6")
check(e.count_points() == 100 and e.compute_trace() == -2, "count_points / compute_trace: y^2 = x^3 + 2x + 3 over F_97 has 100 points, a_q = -2")
check(e.with_computed_trace().trace == -2 and ff.lift(e.with_computed_trace()) is not None,
      "with_computed_trace attaches the computed trace, which then lifts")
check(e.resolve(ff.legendre_trace_resolver).trace == -2 and ff.legendre_trace_resolver(e) == -2,
      "legendre_trace_resolver plugs into resolve()")
check(ff.EllipticCurve(1, 0, 999983).count_points() == 999984 and ff.EllipticCurve(1, 0, 999983).compute_trace() == 0,
      "supersingular y^2 = x^3 + x over F_999983 (p = 3 mod 4): #E = p + 1, trace 0")
# independent oracle: naive tally of squares, over every curve of a few primes
def _brute_count(a, b, p):
    sq = [0] * p
    for y in range(p):
        sq[y * y % p] += 1
    return 1 + sum(sq[(x ** 3 + a * x + b) % p] for x in range(p))
_n_checked = 0
for _p in (5, 7, 11, 13, 17, 19, 23, 97):
    for _a in range(_p):
        for _b in range(_p):
            _c = ff.EllipticCurve(_a, _b, _p)
            if _c.valid():
                _n_checked += 1
                if _c.count_points() != _brute_count(_a, _b, _p) or _c.compute_trace() != _p + 1 - _c.count_points():
                    raise AssertionError(f"count_points mismatch at ({_a}, {_b}, {_p})")
check(_n_checked > 1000, f"count_points == brute-force count on all {_n_checked} nonsingular curves (p in 5..97)")
check(raises(ValueError, lambda: ff.EllipticCurve(1, 1, 91).count_points()), "composite q: count_points raises ValueError")
check(raises(ValueError, lambda: ff.EllipticCurve(1, 1, 1000003).count_points()), "q > 10**6: count_points raises ValueError")
check(raises(ValueError, lambda: ff.EllipticCurve(0, 0, 1009).compute_trace()), "singular curve: compute_trace raises ValueError")
check(raises(ValueError, lambda: ff.EllipticCurve(1, 1, 1000003).with_computed_trace()), "outside the domain: with_computed_trace raises ValueError")
check(ff.legendre_trace_resolver(ff.EllipticCurve(1, 1, 1000003)) is None, "legendre_trace_resolver declines (None) outside its domain")
check(ff.EllipticCurve(1, 1, 1000003).resolve(ff.legendre_trace_resolver).trace is None, "declined resolver leaves the trace unresolved")

# Hasse-Weil: t^2 <= 4q, exact
check(all(ff.hasse_bound_holds(t, 97) == (t * t <= 4 * 97) for t in range(-100, 101)), "hasse_bound_holds(t, 97) exact for |t| <= 100 (accepts |t| <= 19)")
check(ff.hasse_bound_holds(4, 4) and not ff.hasse_bound_holds(5, 4) and not ff.hasse_bound_holds(0, 0) and not ff.hasse_bound_holds(0, -3),
      "hasse_bound_holds: boundary t^2 == 4q accepted; q <= 0 rejected")
check(ff.hasse_bound_holds(2 * 2147483647, bigq) and not ff.hasse_bound_holds(2 * 2147483647 + 100, bigq) and not ff.hasse_bound_holds(-2 ** 63, bigq),
      "hasse_bound_holds near q = 2^62 (128-bit compare, no overflow, INT64_MIN safe)")
check(e.with_trace(19).trace == 19 and e.with_trace(-19).trace == -19, "with_trace accepts |t| <= 19 for q = 97")
check(raises(ValueError, lambda: e.with_trace(20)) and raises(ValueError, lambda: e.with_trace(-20)),
      "with_trace rejects |t| = 20 > 2*sqrt(97) with ValueError")
check(raises(ValueError, lambda: e.with_trace(10 ** 6)) and raises(ValueError, lambda: e.with_trace(-2 ** 63)),
      "with_trace rejects absurd traces (including -2**63)")
check(raises(ValueError, lambda: e.resolve(lambda c: 1000)), "resolve() rejects a resolver result that violates Hasse-Weil")
check(raises(ValueError, lambda: ff.EllipticCurve(2, 3, 97).with_trace(100)) and ff.EllipticCurve(2, 3, 97).trace is None,
      "a rejected with_trace leaves the original curve untouched")

# version consistency: C++ macro (via _core) == pyproject.toml == CMakeLists.txt, when run from a source checkout
check(sn.__version__ == "2.2.0", "simmynet.__version__ == 2.2.0")
_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
for _fname, _pat in (("pyproject.toml", r'^version\s*=\s*"([^"]+)"'),
                     ("CMakeLists.txt", r"project\(\s*simmynet\s+VERSION\s+([0-9.]+)"),
                     ("CITATION.cff", r'^version:\s*"?([0-9.]+)"?')):
    _path = os.path.join(_root, _fname)
    if os.path.isfile(_path):
        with open(_path, encoding="utf-8") as _fh:
            _m = re.search(_pat, _fh.read(), re.M | re.I)
        check(_m is not None and _m.group(1) == sn.__version__, f"{_fname} version matches simmynet.__version__")

# --- Mat2 arithmetic, overflow, bridge ------------------------------------------------------------
check((F * F).to_list() == [-97, 194, -2, -93] and (F @ F) == F * F and ff.power(F, 2) == F * F, "Mat2 product / power")
check(F.adjugate().to_list() == [-2, 97, -1, 0] and F.transpose().to_list() == [0, 1, -97, -2], "adjugate / transpose")
check((ff.Mat2() - ff.Mat2()).to_list() == [0, 0, 0, 0] and F[1, 0] == 1, "identity, subtraction, indexing")
big = ff.Mat2(2 ** 63 - 1, 0, 0, 1)
check(raises(OverflowError, lambda: big * big) and big.mul_checked(big)[1] is True and F.mul_checked(F)[1] is False,
      "overflow raises OverflowError; mul_checked reports the flag")
check(raises(OverflowError, lambda: ff.power(F, 40)), "power(F, 40) raises OverflowError instead of wrapping")
check(raises(ValueError, lambda: ff.Mat2.from_list([1, 2, 3])) and raises(IndexError, lambda: F[2, 0]),
      "Mat2 validates sizes and indices")
check(raises((TypeError, OverflowError), lambda: ff.power(F, -1)), "negative exponent is rejected")

E = F.to_engine()
check(E.to_list() == [0.0, -97.0, 1.0, -2.0] and ff.Mat2.from_engine(E) == F, "Mat2 <-> Matrix2x2 round trip")
check(ff.Mat2(0, -(2 ** 62), 1, 3).to_engine() is None, "to_engine refuses entries beyond 2**53")
check(ff.Mat2.from_engine(sn.Matrix2x2.from_list([0.5, 0, 0, 1])) is None, "from_engine refuses non-integers")
check(ff.lift_to_engine(e) is None and ff.lift_to_engine(e.with_trace(-2)).to_list() == [0.0, -97.0, 1.0, -2.0],
      "lift_to_engine")

print("all simmynet tests passed")
