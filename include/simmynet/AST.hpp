// AST.hpp - abstract syntax tree for the SimmyNet DSL, plus expression evaluation.
//
// Values in the frontend are ring::AlgebraicTuple (c_base + c_pi * pi), not raw doubles: the parser
// turns the symbol `pi` into the tuple {0, 1}, +, -, scalar *, / and == stay exact, and the value is
// LOWERED to a double (to_double()) only where a number is needed (matrix entries, trig of an angle,
// the final folded graph).
//
// ExprProgram compiles an expression tree to a flat RPN tape with constant sub-trees folded
// (with tuple arithmetic) at compile time, so lift layers evaluate without map lookups or
// recursion at run time. The hot path (run) uses doubles only; run_exact uses tuples.
#pragma once

#include "Common.hpp"
#include "algebraic_ring.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace simmynet {

// ---- expressions ------------------------------------------------------------------
using Exact = ring::AlgebraicTuple<double>;  // value type of the frontend: c_base + c_pi * pi

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

struct Num { Exact value; };
struct Var { std::string name; };
struct Neg { ExprPtr operand; };
struct BinOp { char op; ExprPtr left, right; };
struct Call { std::string func; ExprPtr arg; };

struct Expr { std::variant<Num, Var, Neg, BinOp, Call> node; };

inline ExprPtr mkNum(double v) { return std::make_shared<const Expr>(Expr{Num{Exact{v}}}); }
inline ExprPtr mkNum(const Exact& t) { return std::make_shared<const Expr>(Expr{Num{t}}); }
inline ExprPtr mkPi() { return mkNum(Exact::pi()); }  // what the parser emits for the symbol `pi`
inline ExprPtr mkVar(std::string n) { return std::make_shared<const Expr>(Expr{Var{std::move(n)}}); }
inline ExprPtr mkNeg(ExprPtr e) { return std::make_shared<const Expr>(Expr{Neg{std::move(e)}}); }
inline ExprPtr mkBin(char op, ExprPtr l, ExprPtr r) {
    return std::make_shared<const Expr>(Expr{BinOp{op, std::move(l), std::move(r)}});
}
inline ExprPtr mkCall(std::string f, ExprPtr a) {
    return std::make_shared<const Expr>(Expr{Call{std::move(f), std::move(a)}});
}

inline bool isKnownFunc(const std::string& f) {
    return f == "abs" || f == "sqrt" || f == "relu" || f == "tanh" || f == "sin" || f == "cos";
}

inline double applyFunc(const std::string& f, double t) {
    if (f == "abs") return std::fabs(t);
    if (f == "sqrt") {
        if (t < 0.0) throw SimmyNetRuntimeError("math domain error in sqrt");
        return std::sqrt(t);
    }
    if (f == "relu") return std::max(0.0, t);
    if (f == "tanh") return std::tanh(t);
    if (f == "sin") return std::sin(t);
    return std::cos(t);  // "cos"
}

using Env = std::unordered_map<std::string, Exact>;  // double -> Exact converts implicitly

// Operation semantics on tuples. The module {1, pi} is closed under +, -, scalar *, and / by a
// scalar; everything else is LOWERED to double and continues as the plain number {x, 0}. Operands
// without a pi component follow the plain double rules bit for bit.
namespace exprops {
inline Exact mul(const Exact& a, const Exact& b) {
    if (a.is_pure() && b.is_pure()) return Exact{a.c_base * b.c_base};
    if (a.is_pure()) return b * a.c_base;  // scalar multiplication: exact
    if (b.is_pure()) return a * b.c_base;
    return Exact{a.to_double() * b.to_double()};  // pi * pi needs pi^2: leaves the module
}
inline Exact div(const Exact& a, const Exact& b) {
    if (b.is_pure()) {
        if (b.c_base == 0.0) throw SimmyNetRuntimeError("division by zero in expression");
        return a.is_pure() ? Exact{a.c_base / b.c_base} : a / b.c_base;  // exact for powers of two
    }
    const double d = b.to_double();
    if (d == 0.0) throw SimmyNetRuntimeError("division by zero in expression");
    return Exact{a.to_double() / d};
}
// abs / relu keep exactness (they only select a sign); sqrt, tanh, sin, cos lower their operand.
inline Exact func(const std::string& name, const Exact& a) {
    if (a.is_pure()) return Exact{applyFunc(name, a.c_base)};
    if (name == "abs") return a.to_double() < 0.0 ? -a : a;
    if (name == "relu") return a.to_double() > 0.0 ? a : Exact{0.0};
    return Exact{applyFunc(name, a.to_double())};
}
}  // namespace exprops

// Evaluate an expression tree exactly (tuple arithmetic).
inline Exact evalExprExact(const Expr& node, const Env& env) {
    return std::visit(
        overloaded{
            [&](const Num& n) -> Exact { return n.value; },
            [&](const Var& v) -> Exact {
                auto it = env.find(v.name);
                if (it != env.end()) return it->second;
                if (v.name == "pi") return Exact::pi();  // programmatic mkVar("pi"); the parser emits Num
                throw CompileError("unknown variable '" + v.name + "'");
            },
            [&](const Neg& n) -> Exact { return -evalExprExact(*n.operand, env); },
            [&](const Call& c) -> Exact { return exprops::func(c.func, evalExprExact(*c.arg, env)); },
            [&](const BinOp& b) -> Exact {
                const Exact l = evalExprExact(*b.left, env);
                const Exact r = evalExprExact(*b.right, env);
                switch (b.op) {
                    case '+': return l + r;
                    case '-': return l - r;
                    case '*': return exprops::mul(l, r);
                    default: return exprops::div(l, r);
                }
            }},
        node.node);
}

// Evaluate numerically: exact evaluation followed by the boundary lowering to double.
inline double evalExpr(const Expr& node, const Env& env) { return evalExprExact(node, env).to_double(); }

// Names an expression depends on (the constant pi excluded).
inline void collectFreeVariables(const Expr& node, std::set<std::string>& out) {
    std::visit(overloaded{[&](const Num&) {},
                          [&](const Var& v) {
                              if (v.name != "pi") out.insert(v.name);
                          },
                          [&](const Neg& n) { collectFreeVariables(*n.operand, out); },
                          [&](const Call& c) { collectFreeVariables(*c.arg, out); },
                          [&](const BinOp& b) {
                              collectFreeVariables(*b.left, out);
                              collectFreeVariables(*b.right, out);
                          }},
               node.node);
}
inline std::set<std::string> freeVariables(const Expr& node) {
    std::set<std::string> s;
    collectFreeVariables(node, s);
    return s;
}

// "pi", "0.25*pi", "(1 + 0.5*pi)" or a plain number.
inline std::string fmtExact(const Exact& t) {
    if (t.c_pi == 0.0) return fmtG(t.c_base);
    const std::string pi = t.c_pi == 1.0 ? "pi" : (t.c_pi == -1.0 ? "-pi" : fmtG(t.c_pi) + "*pi");
    if (t.c_base == 0.0) return pi;
    return "(" + fmtG(t.c_base) + (t.c_pi < 0 ? " - " : " + ") + (t.c_pi == 1.0 || t.c_pi == -1.0 ? "pi" : fmtG(std::fabs(t.c_pi)) + "*pi") + ")";
}

// Pretty-print: abs(x - y), x * t, pi / 4, ...
inline std::string fmtExpr(const Expr& node, bool top = true) {
    return std::visit(
        overloaded{[&](const Num& n) { return fmtExact(n.value); },
                   [&](const Var& v) { return v.name; },
                   [&](const Neg& n) { return "-" + fmtExpr(*n.operand, false); },
                   [&](const Call& c) { return c.func + "(" + fmtExpr(*c.arg) + ")"; },
                   [&](const BinOp& b) {
                       std::string text =
                           fmtExpr(*b.left, false) + " " + std::string(1, b.op) + " " + fmtExpr(*b.right, false);
                       return top ? text : "(" + text + ")";
                   }},
        node.node);
}

// ---- compiled expression tape ----------------------------------------------------------
class ExprProgram {
public:
    enum class Op : unsigned char { Const, Var, Neg, Add, Sub, Mul, Div, Abs, Sqrt, Relu, Tanh, Sin, Cos };
    // value: for Const, the constant already LOWERED to double (the hot path never sees a tuple).
    struct Instr { Op op; int idx; double value; };
    static constexpr int kMaxStack = 64;

    // Resolve variable names against `names` (the last occurrence wins, like dict(zip(...)));
    // sub-trees with no variables are folded with exact tuple arithmetic and lowered once.
    static ExprProgram compile(const Expr& root, const std::vector<std::string>& names) {
        ExprProgram p;
        bool piShadowed = std::find(names.begin(), names.end(), "pi") != names.end();
        int depth = 0, maxDepth = 0;
        p.emit(root, names, piShadowed, depth, maxDepth);
        if (maxDepth > kMaxStack) throw CompileError("expression is too deeply nested");
        return p;
    }

    // Hot path: doubles only, no tuple anywhere.
    double run(const double* vars) const {
        double st[kMaxStack];
        int sp = 0;
        for (const Instr& in : code_) {
            switch (in.op) {
                case Op::Const: st[sp++] = in.value; break;
                case Op::Var: st[sp++] = vars[in.idx]; break;
                case Op::Neg: st[sp - 1] = -st[sp - 1]; break;
                case Op::Add: --sp; st[sp - 1] = st[sp - 1] + st[sp]; break;
                case Op::Sub: --sp; st[sp - 1] = st[sp - 1] - st[sp]; break;
                case Op::Mul: --sp; st[sp - 1] = st[sp - 1] * st[sp]; break;
                case Op::Div:
                    --sp;
                    if (st[sp] == 0.0) throw SimmyNetRuntimeError("division by zero in expression");
                    st[sp - 1] = st[sp - 1] / st[sp];
                    break;
                case Op::Abs: st[sp - 1] = std::fabs(st[sp - 1]); break;
                case Op::Sqrt:
                    if (st[sp - 1] < 0.0) throw SimmyNetRuntimeError("math domain error in sqrt");
                    st[sp - 1] = std::sqrt(st[sp - 1]);
                    break;
                case Op::Relu: st[sp - 1] = std::max(0.0, st[sp - 1]); break;
                case Op::Tanh: st[sp - 1] = std::tanh(st[sp - 1]); break;
                case Op::Sin: st[sp - 1] = std::sin(st[sp - 1]); break;
                case Op::Cos: st[sp - 1] = std::cos(st[sp - 1]); break;
            }
        }
        return st[0];
    }

    // Exact path: the same tape executed on a stack of AlgebraicTuple. Constants keep their
    // {c_base, c_pi} form; vars are tuples too. Compile-time use only.
    Exact run_exact(const Exact* vars) const {
        Exact st[kMaxStack];
        int sp = 0;
        for (std::size_t pc = 0; pc < code_.size(); ++pc) {
            const Instr& in = code_[pc];
            switch (in.op) {
                case Op::Const: st[sp++] = exact_[pc]; break;
                case Op::Var: st[sp++] = vars[in.idx]; break;
                case Op::Neg: st[sp - 1] = -st[sp - 1]; break;
                case Op::Add: --sp; st[sp - 1] = st[sp - 1] + st[sp]; break;
                case Op::Sub: --sp; st[sp - 1] = st[sp - 1] - st[sp]; break;
                case Op::Mul: --sp; st[sp - 1] = exprops::mul(st[sp - 1], st[sp]); break;
                case Op::Div: --sp; st[sp - 1] = exprops::div(st[sp - 1], st[sp]); break;
                case Op::Abs: st[sp - 1] = exprops::func("abs", st[sp - 1]); break;
                case Op::Sqrt: st[sp - 1] = exprops::func("sqrt", st[sp - 1]); break;
                case Op::Relu: st[sp - 1] = exprops::func("relu", st[sp - 1]); break;
                case Op::Tanh: st[sp - 1] = exprops::func("tanh", st[sp - 1]); break;
                case Op::Sin: st[sp - 1] = exprops::func("sin", st[sp - 1]); break;
                case Op::Cos: st[sp - 1] = exprops::func("cos", st[sp - 1]); break;
            }
        }
        return st[0];
    }

    std::size_t size() const { return code_.size(); }

private:
    std::vector<Instr> code_;
    std::vector<Exact> exact_;  // parallel to code_; only Const entries are meaningful

    void push(Op op, int idx, const Exact& c, int& depth, int& maxDepth, int delta) {
        code_.push_back({op, idx, c.to_double()});  // boundary lowering: the tape stores doubles
        exact_.push_back(c);
        depth += delta;
        maxDepth = std::max(maxDepth, depth);
    }

    void emit(const Expr& e, const std::vector<std::string>& names, bool piShadowed, int& depth, int& maxDepth) {
        if (!piShadowed && !std::holds_alternative<Num>(e.node) && freeVariables(e).empty()) {
            try {
                push(Op::Const, 0, evalExprExact(e, Env{}), depth, maxDepth, +1);  // fold with tuples
                return;
            } catch (const SimmyNetError&) { /* keep the tree so the error surfaces at run time */
            }
        }
        std::visit(
            overloaded{
                [&](const Num& n) { push(Op::Const, 0, n.value, depth, maxDepth, +1); },
                [&](const Var& v) {
                    for (int i = static_cast<int>(names.size()) - 1; i >= 0; --i)
                        if (names[static_cast<std::size_t>(i)] == v.name) {
                            push(Op::Var, i, Exact{}, depth, maxDepth, +1);
                            return;
                        }
                    if (v.name == "pi") {
                        push(Op::Const, 0, Exact::pi(), depth, maxDepth, +1);
                        return;
                    }
                    throw CompileError("unknown variable '" + v.name + "'");
                },
                [&](const Neg& n) {
                    emit(*n.operand, names, piShadowed, depth, maxDepth);
                    push(Op::Neg, 0, Exact{}, depth, maxDepth, 0);
                },
                [&](const Call& c) {
                    emit(*c.arg, names, piShadowed, depth, maxDepth);
                    Op op = c.func == "abs" ? Op::Abs
                          : c.func == "sqrt" ? Op::Sqrt
                          : c.func == "relu" ? Op::Relu
                          : c.func == "tanh" ? Op::Tanh
                          : c.func == "sin" ? Op::Sin : Op::Cos;
                    push(op, 0, Exact{}, depth, maxDepth, 0);
                },
                [&](const BinOp& b) {
                    emit(*b.left, names, piShadowed, depth, maxDepth);
                    emit(*b.right, names, piShadowed, depth, maxDepth);
                    Op op = b.op == '+' ? Op::Add : b.op == '-' ? Op::Sub : b.op == '*' ? Op::Mul : Op::Div;
                    push(op, 0, Exact{}, depth, maxDepth, -1);
                }},
            e.node);
    }
};

// The tape is read linearly on the hot path: 16 bytes/instruction keeps four instructions per cache line.
static_assert(sizeof(ExprProgram::Instr) == 16, "ExprProgram::Instr must stay 16 bytes");

// ---- statements ----------------------------------------------------------------------
// One factor of a group word: name, optional argument R(theta), exponent.
struct Factor {
    std::string name;
    int exp = 1;
    ExprPtr arg;  // null when absent
};
using Word = std::vector<Factor>;

inline std::string fmtWordItems(const Word& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const Factor& f = items[i];
        if (i) out += " * ";
        out += f.name;
        if (f.arg) out += "(" + fmtExpr(*f.arg) + ")";
        if (f.exp != 1) out += "^" + std::to_string(f.exp);
    }
    return out;
}

struct GenDecl {
    std::string name;
    std::string kind;  // matrix | rotation | reflection | phasor
    std::vector<std::vector<ExprPtr>> rows;
    ExprPtr angle;
    int line = 0;
    std::optional<std::string> param;
};
struct LiftDecl { std::string name; ExprPtr expr; int line = 0; };
struct SubgroupDecl { std::string name; std::vector<std::string> generators; int line = 0; };
struct SeparateDecl { std::string name; std::vector<ExprPtr> weight; ExprPtr bias; int line = 0; };
struct SectorDecl { std::string name; ExprPtr lo, hi; int line = 0; };
struct SpaceDecl {
    std::string name;
    std::vector<std::vector<ExprPtr>> points;
    std::vector<int> labels;
    int line = 0;
};
struct SynthesizeStmt {
    std::string name;
    std::optional<std::string> space, subgroup, ambient;
    std::optional<Word> coset;
    int line = 0;
};
struct EvalStmt {
    Word word;
    std::optional<std::vector<ExprPtr>> point;
    int line = 0;
};
struct ClassifyStmt {
    std::string classifier;
    std::optional<std::vector<ExprPtr>> point;
    std::optional<Word> word;
    std::optional<int> expect;
    int line = 0;
};
struct VerifyStmt { std::string classifier, subgroup, ambient; int line = 0; };
struct CheckStmt { Word lhs, rhs; int line = 0; };
struct SweepStmt {
    std::string classifier, gen;
    ExprPtr start, stop;
    int steps = 1;
    int line = 0;
};

using Statement = std::variant<GenDecl, LiftDecl, SubgroupDecl, SeparateDecl, SectorDecl, SpaceDecl,
                               SynthesizeStmt, EvalStmt, ClassifyStmt, VerifyStmt, CheckStmt, SweepStmt>;

inline int statementLine(const Statement& s) {
    return std::visit([](const auto& st) { return st.line; }, s);
}

struct GroupDecl {
    std::string name;
    std::vector<Statement> statements;
    int line = 0;
};

}  // namespace simmynet
