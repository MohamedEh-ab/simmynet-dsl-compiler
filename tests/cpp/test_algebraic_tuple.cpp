// test_algebraic_tuple.cpp - tests for simmynet/algebraic_ring.hpp and its use in the frontend.
//
//   * AlgebraicTuple algebra (compile-time proofs, runtime laws, exactness vs plain doubles)
//   * lowering: pure numbers are bit-identical to plain doubles
//   * parser: `pi` becomes the tuple {0, 1}
//   * ExprProgram: hot path stays double-only; run_exact executes on tuples; constants fold exactly
//   * symbolic `check` on phasor words, and tuple coefficients in `separate` blocks
//   * boundary lowering inside foldNetwork()
//
// Exit status 0 iff every check passes.
#include <simmynet/Api.hpp>
#include <simmynet/Examples.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace simmynet;
using ring::AlgebraicTuple;
using T = AlgebraicTuple<double>;

// ---- compile-time proofs ---------------------------------------------------------------------
namespace {
constexpr T kQuarter = T::pi() / 4.0;
static_assert(kQuarter + kQuarter == T::pi() / 2.0, "pi/4 + pi/4 == pi/2");
static_assert(kQuarter * 3.0 == T(0.0, 0.75), "3 * (pi/4) == 3*pi/4");
static_assert(T::pi() - T::pi() == T(0.0), "pi - pi == 0");
static_assert(-(T::pi() / 2.0) + T::pi() / 2.0 == T{}, "x + (-x) == 0");
static_assert(T(3.0).is_pure() && !T::pi().is_pure(), "is_pure");
static_assert(T(2.5).to_double() == 2.5 && T::pi().to_double() == 3.14159265358979323846, "lowering");
static_assert(sizeof(ExprProgram::Instr) == 16, "the hot-path tape must not grow");
}  // namespace

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = "") {
    (ok ? g_pass : g_fail)++;
    std::printf("  [%s] %s%s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : "  (", detail.c_str(), detail.empty() ? "" : ")");
}
static void banner(const char* s) { std::printf("\n%s\n%s\n", s, std::string(std::strlen(s), '-').c_str()); }

template <class E, class Fn>
static bool throws(Fn&& fn) {
    try {
        fn();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

static std::string fmt_pct(int k, int n) {
    char b[64];
    std::snprintf(b, sizeof b, "%.1f%% exact", 100.0 * k / n);
    return b;
}

static std::string fmt_e(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.3e", v);
    return b;
}
static std::string fmt_counts(const api::RunResult& r) {
    char b[96];
    std::snprintf(b, sizeof b, "passed %d, failed %d", r.passed, r.failed);
    return b;
}

static bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int range(int lo, int hi) { return lo + static_cast<int>(next() % static_cast<std::uint64_t>(hi - lo + 1)); }
};

static const Expr& parse_lift_expr(const char* rhs, std::vector<GroupDecl>& keep) {
    keep = Parser(tokenize(std::string("group G { lift z = ") + rhs + " }")).parseProgram();
    return *std::get<LiftDecl>(keep[0].statements[0]).expr;
}

int main() {
    // ------------------------------------------------------------------------------------------
    banner("[1] AlgebraicTuple algebra");
    {
        Rng rng{7};
        bool laws = true;
        for (int i = 0; i < 20000; ++i) {  // dyadic coefficients k/64: all arithmetic below is exact
            auto rnd = [&] { return T(rng.range(-64, 64) / 64.0, rng.range(-64, 64) / 64.0); };
            const T a = rnd(), b = rnd(), c = rnd();
            const double s = rng.range(-8, 8);
            laws &= (a + b == b + a) && ((a + b) + c == a + (b + c)) && (a - a == T(0.0)) && (a + (-a) == T{}) &&
                    ((a + b) * s == a * s + b * s) && (a * 2.0 == a + a) && ((a - b) + b == a) && (a / 4.0 * 4.0 == a);
        }
        check(laws, "commutativity, associativity, inverses, scalar distributivity hold exactly (dyadic coefficients)");
        check(T(1.0, 2.0) != T(1.0, 2.5) && T(1.0, 2.0) == T(1.0, 2.0), "== compares coefficients directly (no tolerance)");

        // lowering: pure numbers are returned bit for bit
        const double specials[] = {0.0, -0.0, 1.0, -2.5, 1e-300, 1e300, std::numeric_limits<double>::infinity(),
                                   -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()};
        bool bits = true;
        for (double v : specials) bits &= same_bits(T(v).to_double(), v);
        check(bits, "to_double() of a pure number is bit-identical (incl. -0.0, inf, NaN)");

        // dyadic angle algebra: a tuple lowered ONCE equals the correctly rounded (sum of coefficients)*pi;
        // accumulating the radian value term by term does not.
        const int Ls[] = {2, 4, 8, 32};
        for (int L : Ls) {
            int dbl = 0, tup = 0;
            const int trials = 20000;
            for (int t = 0; t < trials; ++t) {
                double acc = 0.0, coef = 0.0;
                int ksum = 0;
                for (int i = 0; i < L; ++i) {
                    const int k = rng.range(-40, 40);
                    ksum += k;
                    acc += (k / 64.0) * ring::kPiValue<double>;  // double evaluation, term by term
                    coef += k / 64.0;                            // tuple coefficient (exact)
                }
                const double truth = (ksum / 64.0) * ring::kPiValue<double>;  // one rounding of the exact value
                dbl += acc == truth;
                tup += T(0.0, coef).to_double() == truth;
            }
            std::printf("    L=%-3d dyadic angle sums matching the single-rounded value: double %5.1f%%, tuple %5.1f%%\n", L,
                        100.0 * dbl / trials, 100.0 * tup / trials);
            if (L == 32) {
                check(tup == trials, "tuple: 100% exact on dyadic angle sums (L = 32)");
                check(dbl < trials * 0.9, "plain double accumulation is not exact (L = 32)", fmt_pct(dbl, trials));
            }
        }
        // honesty check: non-dyadic coefficients round just like doubles (no claim of exactness)
        int nd = 0;
        for (int n = 2; n < 60; ++n) nd += ((1.0 / n) * n != 1.0);
        std::printf("    note: for non-dyadic coefficients (pi/n) the coefficient arithmetic rounds too (%d of 58 cases (1/n)*n != 1)\n", nd);
    }

    // ------------------------------------------------------------------------------------------
    banner("[2] Expression semantics: what stays exact, what is lowered");
    {
        using exprops::div;
        using exprops::func;
        using exprops::mul;
        check(mul(T::pi(), T(3.0)) == T(0.0, 3.0) && mul(T(3.0), T::pi()) == T(0.0, 3.0), "pi * 3 is scalar multiplication: exact {0, 3}");
        const T pp = mul(T::pi(), T::pi());
        check(pp.is_pure() && pp.c_base == ring::kPiValue<double> * ring::kPiValue<double>, "pi * pi leaves the module: lowered to the plain number pi^2");
        check(div(T::pi(), T(4.0)) == T(0.0, 0.25), "pi / 4 is exact {0, 0.25}");
        check(div(T(1.0), T::pi()).is_pure(), "1 / pi lowers (not representable)");
        check(throws<SimmyNetRuntimeError>([] { div(T::pi(), T(0.0)); }) && throws<SimmyNetRuntimeError>([] { div(T(1.0), T(0.0)); }),
              "division by zero is reported, as before");
        check(func("abs", T(0.0, -0.25)) == T(-0.0, 0.25) && func("abs", T(-3.0)) == T(3.0), "abs(-pi/4) keeps exactness (sign flip); abs(-3) == 3");
        check(func("relu", T(0.0, -1.0)) == T(0.0) && func("relu", T(0.0, 0.5)) == T(0.0, 0.5), "relu keeps exactness");
        check(func("cos", T(0.0, 0.5)).is_pure() && std::fabs(func("cos", T(0.0, 0.5)).c_base) < 1e-15, "cos(pi/2) lowers its operand and returns a plain number");
        check(throws<SimmyNetRuntimeError>([] { func("sqrt", T(-1.0)); }), "sqrt of a negative raises");
    }

    // ------------------------------------------------------------------------------------------
    banner("[3] Parser handshake: the symbol `pi` is the tuple {0, 1}");
    {
        std::vector<GroupDecl> keep;
        const Expr& e = parse_lift_expr("pi", keep);
        check(std::holds_alternative<Num>(e.node) && std::get<Num>(e.node).value == T::pi(), "`pi` parses to Num{AlgebraicTuple{0.0, 1.0}} (not a Var)");
        const Expr& e2 = parse_lift_expr("3*pi/4", keep);
        check(evalExprExact(e2, Env{}) == T(0.0, 0.75), "3*pi/4 evaluates to exactly {0, 0.75}");
        check(same_bits(evalExpr(e2, Env{}), (3 * ring::kPiValue<double>) / 4), "evalExpr (lowered) is bit-identical to the old double result");
        check(fmtExpr(parse_lift_expr("pi / 4", keep)) == "pi / 4" && fmtExpr(parse_lift_expr("2*pi", keep)) == "2 * pi",
              "pretty-printing is unchanged: pi / 4, 2 * pi");
        check(freeVariables(parse_lift_expr("x * pi + y", keep)) == std::set<std::string>({"x", "y"}), "pi is not a free variable");
        check(throws<ParseError>([] { Parser(tokenize("group G { lift z = pi(2) }")).parseProgram(); }), "pi(...) is still an unknown function");
        check(throws<ParseError>([] { Parser(tokenize("group G { gen R(pi) = phasor(pi) }")).parseProgram(); }), "`pi` cannot be a generator parameter name");
    }

    // ------------------------------------------------------------------------------------------
    banner("[4] ExprProgram: double-only hot path, exact tuple path, exact constant folding");
    {
        std::vector<GroupDecl> keep;
        const std::vector<std::string> names{"x", "y"};
        const ExprProgram prog = ExprProgram::compile(parse_lift_expr("abs(x - y) * 2 + pi / 4", keep), names);
        const double vars[] = {0.3, 1.1};
        check(same_bits(prog.run(vars), std::fabs(0.3 - 1.1) * 2 + ring::kPiValue<double> / 4), "run(): double hot path matches the old evaluation bit for bit");
        const T tvars[] = {T(0.3), T(1.1)};
        check(same_bits(prog.run_exact(tvars).to_double(), prog.run(vars)), "run_exact() on pure tuples agrees with run()");
        const ExprProgram lin = ExprProgram::compile(parse_lift_expr("x + 3 * pi / 4", keep), names);
        const T tv[] = {T(0.0, 0.25), T(0.0)};
        check(lin.run_exact(tv) == T(0.0, 1.0), "run_exact keeps pi symbolic: pi/4 + 3*pi/4 == pi exactly");
        // constant folding with tuples: a pi-expression that cancels folds to ONE exact constant
        const std::string cancel = "11*pi/16 + 15*pi/16 - 26*pi/16";
        const ExprProgram folded = ExprProgram::compile(parse_lift_expr(cancel.c_str(), keep), names);
        check(folded.size() == 1 && folded.run(vars) == 0.0 && folded.run_exact(tv) == T(0.0), "constant folding is exact: the tape is one Const equal to 0");
        const double plain = (11 * ring::kPiValue<double>) / 16 + (15 * ring::kPiValue<double>) / 16 - (26 * ring::kPiValue<double>) / 16;
        volatile double sink = plain;
        check(sink != 0.0, "the same expression in plain doubles leaves a residue", fmt_e(sink));
    }

    // ------------------------------------------------------------------------------------------
    banner("[5] Symbolic `check` on phasor words, and exact coefficients in `separate`");
    {
        const char* src = R"(
group G {
    gen R(theta) = phasor(theta)
    gen s = matrix([[-1, 0], [0, 1]])
    check R(pi/4) * R(pi/4) == R(pi/2)
    check R(2*pi) == e
    check R(pi/4)^-1 * R(pi/4) == e
    check R(pi/4) * R(pi/4) == R(pi/3)
    check s * s == e
    separate C { weight = [11*pi/16 + 15*pi/16 - 26*pi/16, 1], bias = -0.5 }
    classify C on (5, 0.9) expect 1
    classify C on (5, 0.0) expect 0
}
)";
        const api::Interpreter interp;
        const api::Program prog = interp.compile(src);
        std::vector<const api::TaskHandle*> checks;
        for (const auto& t : prog.groups[0].tasks)
            if (t.kind == "check") checks.push_back(&t);
        check(checks.size() == 5, "five check statements compiled");
        if (checks.size() == 5) {
            check(checks[0]->exactEqual == std::optional<bool>(true), "R(pi/4)*R(pi/4) == R(pi/2): PROVED equal symbolically",
                  checks[0]->lhsAngle + " == " + checks[0]->rhsAngle);
            check(checks[1]->exactEqual == std::optional<bool>(true), "R(2*pi) == e: proved (angle 2*pi is 0 mod 2*pi)");
            check(checks[2]->exactEqual == std::optional<bool>(true), "R(pi/4)^-1 * R(pi/4) == e: proved (negative exponent negates the angle)");
            check(checks[3]->exactEqual == std::optional<bool>(false), "R(pi/4)*R(pi/4) == R(pi/3): not provable; numeric comparison decides");
            check(!checks[4]->exactEqual.has_value(), "s * s == e: matrix generators have no symbolic proof; numeric comparison only");
        }
        const api::RunResult r = interp.run(src);
        check(r.passed == 6 && r.failed == 1 && r.xFailed == 0, "run: 4 checks + 2 classifies pass, the one false check fails", fmt_counts(r));
        const auto& clf = prog.groups[0].classifiers;
        const api::ClassifierInfo* c = nullptr;
        for (const auto& ci : clf) if (ci.name == "C") c = &ci;
        check(c && c->weight[0] == 0.0, "separate: weight[0] = 11*pi/16 + 15*pi/16 - 26*pi/16 is exactly 0 after lowering");
        // the transcript shows the symbolic line
        const api::RunResult v = interp.run(src, true);
        check(v.transcript.find("equal mod 2*pi (exact, no tolerance)") != std::string::npos, "transcript reports the symbolic proof");
    }

    // ------------------------------------------------------------------------------------------
    banner("[6] Boundary lowering in foldNetwork()");
    {
        const char* src = R"(
group L {
    gen r = rotation(90)
    separate C { weight = [pi / 2, 1, 0, 0], bias = -2 * pi / 4 }
    classify C of r * r
}
)";
        SimmyNetCompiler comp;
        const auto groups = comp.compileProgram(Parser(tokenize(src)).parseProgram());
        const Job* job = nullptr;
        for (const Task& t : groups[0].tasks) if (const auto* j = std::get_if<Job>(&t)) job = j;
        check(job != nullptr, "job compiled");
        const auto* hp = std::get_if<HyperplaneLayer>(&job->network.layers.back());
        check(hp && hp->weight[0] == T(0.0, 0.5) && hp->bias == T(0.0, -0.5), "IR (before folding) keeps the exact tuples: weight[0] = {0, 0.5}, bias = {0, -0.5}");
        check(same_bits(job->folded.hyper.weight[0], ring::kPiValue<double> / 2) && same_bits(job->folded.hyper.bias, -ring::kPiValue<double> / 2),
              "FoldedNetwork holds the lowered doubles: weight[0] = pi/2, bias = -pi/2 (bit-identical)");
        const SimmyNetRuntime rt;
        const ForwardResult a = rt.forward(job->folded, job->inputVec, false), b = rt.forwardReference(job->network, job->inputVec);
        check(a.decision == b.decision && a.score && b.score && std::fabs(*a.score - *b.score) < 1e-12, "folded (lowered) and reference (lowers on use) agree");
    }

    // ------------------------------------------------------------------------------------------
    banner("[7] The runtime hot path is untouched (informational)");
    {
        const api::Program prog = api::Interpreter().compile(examples::D4_SOURCE);
        const api::JobHandle* j = nullptr;
        for (const auto& t : prog.groups[0].tasks) if (t.kind == "job" && t.jobs[0].kind() == "classify") j = &t.jobs[0];
        SimmyNetRuntime::Scratch scratch;
        SimmyNetRuntime rt;
        SimmyNetCompiler comp;
        const auto groups = comp.compileProgram(Parser(tokenize(examples::D4_SOURCE)).parseProgram());
        const Job* job = nullptr;
        for (const Task& t : groups[0].tasks) if (const auto* jj = std::get_if<Job>(&t)) if (jj->kind == "classify") job = jj;
        const long iters = 3000000;
        int acc = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < iters; ++i) {
            acc += rt.decideFast(job->folded, job->inputVec.data(), scratch);
            asm volatile("" : "+r"(acc) : : "memory");
        }
        const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / static_cast<double>(iters);
        std::printf("    decideFast on the folded D4 word: %.1f ns/pass (%d)\n", ns, acc > 0);
        check(j != nullptr && acc == iters && ns < 200.0, "folded pass is still nanoseconds (no tuple on the hot path)", fmt_e(ns));
    }

    std::printf("\n==============================================================================\n");
    std::printf("SUMMARY: %d checks passed, %d failed\n", g_pass, g_fail);
    std::printf("==============================================================================\n");
    return g_fail == 0 ? 0 : 1;
}
