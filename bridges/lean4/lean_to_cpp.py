#!/usr/bin/env python3
"""lean_to_cpp.py - turn the Lean 4 export `rules.json` into include/simmynet/verified/LeanVerifiedRules.hpp.

Zero dependencies (standard library only, Python 3.12).  Deterministic: the same rules.json always
produces the same bytes (no timestamps, rules sorted by Lean name, LF line endings, ASCII output),
so the generated header can be committed and `--check` can guard against a stale copy in CI.

    python lean_to_cpp.py                         # rules.json -> ../../include/simmynet/verified/LeanVerifiedRules.hpp
    python lean_to_cpp.py my_rules.json -o out.hpp
    python lean_to_cpp.py --check                 # exit 1 if the committed header is stale
    python lean_to_cpp.py --stdout                # print instead of writing

The input schema ("simmynet.lean-rules/1") is produced by `#export_simmy_rules` in SimmyBridge.lean;
see bridges/lean4/README.md.  Every rule is validated before a single byte is emitted; any problem is
reported with the rule name and the path of the offending node, and nothing is written.

What the header contains
  * a flat, constexpr expression pool per rule (data only; the evaluator is include/simmynet/verified/RuleKit.hpp),
  * the Lean theorem name, module, concrete type tag and statement in comments AND as static_assert invariants,
  * a compile-time instance check (static_assert) of every rule whose arithmetic is constexpr,
  * rewrite hooks for the rule shapes the generator recognises (generated only when the rule is present).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

SCHEMA = "simmynet.lean-rules/1"
DEFAULT_OUT = Path(__file__).resolve().parent.parent.parent / "include" / "simmynet" / "verified" / "LeanVerifiedRules.hpp"
GUARD = "SIMMYNET_VERIFIED_LEAN_VERIFIED_RULES_HPP"

INT64_MAX = (1 << 63) - 1
MAX_PARAMS = 8

TYPE_TAGS = {
    "finite_field": "FiniteField",
    "dihedral": "Dihedral",
    "so2_phasor": "So2Phasor",
    "algebraic_tuple": "AlgebraicTuple",
}
DOMS = {"nat": "Nat", "int": "Int", "zmod": "ZMod", "dihedral": "Dihedral", "real": "Real"}
HYPS = {"prime": 1, "nonzero": 2}

# op -> (C++ enumerator, arity); var/lit/one/pi carry their payload in other fields.
OPS = {
    "var": ("Var", 0), "lit": ("Lit", 0), "one": ("One", 0), "pi": ("Pi", 0),
    "add": ("Add", 2), "sub": ("Sub", 2), "mul": ("Mul", 2), "div": ("Div", 2), "mod": ("Mod", 2),
    "pow": ("Pow", 2), "neg": ("Neg", 1), "inv": ("Inv", 1), "cast": ("Cast", 1), "val": ("Val", 1),
    "cos": ("Cos", 1), "sin": ("Sin", 1), "dih_r": ("DihR", 1), "dih_sr": ("DihSr", 1),
}

# Which ops / value domains each concrete type tag admits (mirrors the exporter and RuleKit).
ALLOWED_OPS = {
    "finite_field": {"var", "lit", "add", "sub", "mul", "neg", "pow", "inv", "mod", "cast", "val"},
    "dihedral": {"var", "lit", "one", "add", "sub", "mul", "neg", "pow", "inv", "cast", "dih_r", "dih_sr"},
    "so2_phasor": {"var", "lit", "pi", "add", "sub", "mul", "neg", "div", "inv", "pow", "cast", "cos", "sin"},
    "algebraic_tuple": {"var", "lit", "pi", "add", "sub", "mul", "neg", "div", "cast"},
}
ALLOWED_DOMS = {
    "finite_field": {"nat", "int", "zmod"},
    "dihedral": {"nat", "int", "zmod", "dihedral"},
    "so2_phasor": {"nat", "real"},
    "algebraic_tuple": {"nat", "real"},
}


class RuleError(Exception):
    """A malformed or unsupported rule; the message names the rule and the node path."""


# ------------------------------------------------------------------------------------------------
# Validation + flattening into a constexpr pool
# ------------------------------------------------------------------------------------------------
class Pool:
    """Hash-consed flat expression pool; children always precede their parents."""

    def __init__(self, rule: str, tag: str, slots: dict[str, int]) -> None:
        self.rule, self.tag, self.slots = rule, tag, slots
        self.rows: list[tuple] = []  # (op, dom, a, b, k, m)
        self.notes: list[str] = []
        self._index: dict[tuple, int] = {}

    def fail(self, path: str, msg: str) -> RuleError:
        return RuleError(f"{self.rule}: at {path}: {msg}")

    def add(self, node: object, path: str, limit_slots: int) -> int:
        if not isinstance(node, dict):
            raise self.fail(path, "expected an object")
        op, dom = node.get("op"), node.get("dom")
        if op not in OPS:
            raise self.fail(path, f"unknown op {op!r}")
        if dom not in DOMS:
            raise self.fail(path, f"unknown dom {dom!r}")
        if op not in ALLOWED_OPS[self.tag]:
            raise self.fail(path, f"op '{op}' is not supported for type tag '{self.tag}'")
        if dom not in ALLOWED_DOMS[self.tag]:
            raise self.fail(path, f"value domain '{dom}' is not supported for type tag '{self.tag}'")

        a = b = m = -1
        k = 0
        label = op
        if op == "var":
            name = node.get("name")
            if name not in self.slots or self.slots[name] >= limit_slots:
                raise self.fail(path, f"variable {name!r} is not a parameter declared before this point")
            a, label = self.slots[name], f"var {name}"
        elif op == "lit":
            try:
                k = int(node.get("value"))  # type: ignore[arg-type]
            except (TypeError, ValueError):
                raise self.fail(path, f"literal value {node.get('value')!r} is not an integer") from None
            if abs(k) > INT64_MAX:
                raise self.fail(path, f"literal {k} does not fit in int64")
            label = f"lit {k}"
        elif op not in ("one", "pi"):
            args = node.get("args")
            arity = OPS[op][1]
            if not isinstance(args, list) or len(args) != arity:
                raise self.fail(path, f"op '{op}' needs exactly {arity} argument(s)")
            idx = [self.add(x, f"{path}.{op}[{i}]", limit_slots) for i, x in enumerate(args)]
            a = idx[0]
            b = idx[1] if arity == 2 else -1
            label = f"{op}({', '.join(map(str, idx))})"
        if dom in ("zmod", "dihedral"):
            if "modulus" not in node:
                raise self.fail(path, f"a '{dom}' node needs a 'modulus' (the p of ZMod p / the n of DihedralGroup n)")
            m = self.add(node["modulus"], f"{path}.modulus", limit_slots)
            label += f"  [mod node {m}]"

        key = (op, dom, a, b, k, m)
        if key in self._index:
            return self._index[key]
        self._index[key] = len(self.rows)
        self.rows.append(key)
        self.notes.append(label)
        return self._index[key]


def tup(node: dict) -> tuple:
    """Canonical nested tuple of a JSON node (variables by name), for shape matching."""
    op = node["op"]
    if op == "var":
        return ("var", node["name"])
    if op == "lit":
        return ("lit", str(int(node["value"])))
    if op in ("one", "pi"):
        return (op,)
    return (op, *[tup(x) for x in node["args"]])


def classify(rule: dict) -> str:
    """Recognise the rewrite shapes the generator has a hook for; everything else is 'generic'."""
    tag, params, eqs = rule["type_tag"], rule["params"], rule["equations"]
    pnames = [p["name"] for p in params]

    if tag == "finite_field" and len(eqs) == 1 and len(params) == 2:
        p, x = params
        l, r = tup(eqs[0]["lhs"]), tup(eqs[0]["rhs"])
        if (p["dom"] == "nat" and "prime" in p.get("hyps", []) and x["dom"] == "zmod"
                and tup(x["modulus"]) == ("var", p["name"])
                and l == ("pow", ("var", x["name"]), ("var", p["name"])) and r == ("var", x["name"])):
            return "frobenius_fixed"

    if tag == "so2_phasor" and len(params) == 2 and len(eqs) == 2 and all(q["dom"] == "real" for q in params):
        a, b = ("var", pnames[0]), ("var", pnames[1])
        c = lambda t: ("cos", t)  # noqa: E731
        s = lambda t: ("sin", t)  # noqa: E731
        ab = ("add", a, b)
        want = {
            (c(ab), ("sub", ("mul", c(a), c(b)), ("mul", s(a), s(b)))),
            (s(ab), ("add", ("mul", s(a), c(b)), ("mul", c(a), s(b)))),
        }
        got = {(tup(q["lhs"]), tup(q["rhs"])) for q in eqs}
        if got == want:
            return "so2_angle_add"

    if tag == "dihedral" and len(eqs) == 1:
        sr0, r1 = ("dih_sr", ("lit", "0")), ("dih_r", ("lit", "1"))
        l, r = tup(eqs[0]["lhs"]), tup(eqs[0]["rhs"])
        if l == ("mul", ("mul", sr0, r1), sr0) and r in (("inv", r1), ("dih_r", ("neg", ("lit", "1")))):
            return "dihedral_srs_inv"

    return "generic"


def validate_document(doc: object) -> list[dict]:
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA:
        raise RuleError(f"input is not a '{SCHEMA}' document (found schema {doc.get('schema')!r})" if isinstance(doc, dict)
                        else "input is not a JSON object")
    rules = doc.get("rules")
    if not isinstance(rules, list):
        raise RuleError("'rules' must be a list")
    names = [r.get("name") if isinstance(r, dict) else None for r in rules]
    if any(not isinstance(n, str) or not n for n in names):
        raise RuleError("every rule needs a non-empty string 'name'")
    dups = sorted({n for n in names if names.count(n) > 1})
    if dups:
        raise RuleError(f"duplicate rule names: {', '.join(dups)}")
    return sorted(rules, key=lambda r: r["name"])


def lower_rule(rule: dict) -> dict:
    """Validate one rule and flatten it; returns everything the emitter needs."""
    name = rule["name"]
    for key in ("module", "type_tag", "statement", "params", "equations"):
        if key not in rule:
            raise RuleError(f"{name}: missing field '{key}'")
    tag = rule["type_tag"]
    if tag not in TYPE_TAGS:
        raise RuleError(f"{name}: unknown type_tag {tag!r} (expected one of {', '.join(TYPE_TAGS)})")
    params = rule["params"]
    if len(params) > MAX_PARAMS:
        raise RuleError(f"{name}: {len(params)} parameters; the C++ kit supports at most {MAX_PARAMS}")
    if not rule["equations"]:
        raise RuleError(f"{name}: no equations")

    slots: dict[str, int] = {}
    for i, p in enumerate(params):
        if p.get("name") in slots:
            raise RuleError(f"{name}: duplicate parameter name {p.get('name')!r}")
        slots[p["name"]] = i
    pool = Pool(name, tag, slots)

    prows = []
    for i, p in enumerate(params):
        dom = p.get("dom")
        if dom not in DOMS or dom not in ALLOWED_DOMS[tag]:
            raise RuleError(f"{name}: parameter {p.get('name')!r} has value domain {dom!r}, unsupported for '{tag}'")
        hyp = 0
        for h in p.get("hyps", []):
            if h not in HYPS:
                raise RuleError(f"{name}: unknown hypothesis {h!r} on parameter {p['name']!r}")
            hyp |= HYPS[h]
        m = -1
        if dom in ("zmod", "dihedral"):
            if "modulus" not in p:
                raise RuleError(f"{name}: parameter {p['name']!r} of domain '{dom}' needs a modulus")
            m = pool.add(p["modulus"], f"params[{i}].modulus", i)
        prows.append((DOMS[dom], hyp, m, p["name"], dom))

    erows = []
    for j, e in enumerate(rule["equations"]):
        erows.append((pool.add(e["lhs"], f"equations[{j}].lhs", len(params)),
                      pool.add(e["rhs"], f"equations[{j}].rhs", len(params))))

    kind = classify(rule)
    # Semantic guard: ZMod inverse is only modelled for primes, so demand the Prime hypothesis.
    if tag == "finite_field" and any(r[0] == "inv" for r in pool.rows):
        if not any(h & 1 for (_, h, _, _, _) in prows):
            raise RuleError(f"{name}: 'inv' in ZMod needs a [Fact (Nat.Prime p)] hypothesis (non-units are not modelled)")
    return {"rule": rule, "tag": tag, "kind": kind, "pool": pool, "params": prows, "eqs": erows}


# ------------------------------------------------------------------------------------------------
# C++ emission
# ------------------------------------------------------------------------------------------------
def ascii_comment(text: str) -> str:
    """Single-line, ASCII-only rendering of arbitrary text for use inside a // comment."""
    return " ".join(text.split()).encode("ascii", "backslashreplace").decode("ascii")


def cpp_string(text: str) -> str:
    out = ['"']
    for ch in text:
        if ch in '"\\':
            out.append("\\" + ch)
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:
            raise RuleError(f"non-ASCII or control character in identifier {text!r}")
    out.append('"')
    return "".join(out)


HOOK_FROBENIUS = '''\
// ---- hook: {name} ({kind}) ------------------------------------------------------------
// Lean:  forall p [Fact p.Prime] (x : ZMod p), x ^ p = x
// Frobenius fold: x^p (mod p) rewrites to x (mod p).  The hypothesis `p prime` is discharged HERE,
// at compile time, by trial division, so an instantiation with a composite P does not compile.
constexpr bool is_prime_trial(std::uint64_t n) noexcept {{
    if (n < 2) return false;
    for (std::uint64_t d = 2; d <= n / d; ++d)
        if (n % d == 0) return false;
    return true;
}}
template <std::uint64_t P>
constexpr std::uint64_t frobenius_fold(std::uint64_t x) noexcept {{
    static_assert(is_prime_trial(P), "{name}: the rewrite x^p = x needs a prime modulus P");
    return x % P;
}}
// Reference (un-rewritten) semantics, for cross-checks: x^e mod p by square-and-multiply.
constexpr std::uint64_t frobenius_reference(std::uint64_t x, std::uint64_t e, std::uint64_t p) noexcept {{
    return powmod64(x, e, p);
}}
'''

HOOK_SO2 = '''\
// ---- hook: {name} ({kind}) -------------------------------------------------------------
// Lean:  forall a b : Real, cos (a + b) = cos a * cos b - sin a * sin b  AND
//                           sin (a + b) = sin a * cos b + cos a * sin b
// i.e. R(a) R(b) = R(a + b) for the SO(2) rotation matrices R.  Folding two phasor generators
// therefore adds their angles, and with AlgebraicTuple the sum stays exact (pi/4 + pi/4 == pi/2).
template <class T>
constexpr ring::AlgebraicTuple<T> fold_phasor_angles(const ring::AlgebraicTuple<T>& a,
                                                     const ring::AlgebraicTuple<T>& b) noexcept {{
    return a + b;
}}
'''

HOOK_DIHEDRAL = '''\
// ---- hook: {name} ({kind}) ------------------------------------------------------
// Lean:  forall n, sr 0 * r 1 * sr 0 = (r 1)^-1   in DihedralGroup n
// Peephole rewrite on words over {{'r' = r 1, 'R' = (r 1)^-1, 's' = sr 0}}:  s r s  ->  R.
// Sound in every DihedralGroup n (a = b implies u a v = u b v); NOT claimed confluent or complete.
// In place, left to right; returns the new length.
constexpr std::size_t reduce_srs_to_rinv(char* w, std::size_t len) noexcept {{
    std::size_t top = 0;
    for (std::size_t i = 0; i < len; ++i) {{
        w[top++] = w[i];
        if (top >= 3 && w[top - 1] == 's' && w[top - 2] == 'r' && w[top - 3] == 's') {{
            top -= 3;
            w[top++] = 'R';
        }}
    }}
    return top;
}}
'''

HOOKS = {"frobenius_fixed": HOOK_FROBENIUS, "so2_angle_add": HOOK_SO2, "dihedral_srs_inv": HOOK_DIHEDRAL}


def emit(doc: dict, lowered: list[dict], canonical: str) -> str:
    sha = hashlib.sha256(canonical.encode("utf-8")).hexdigest()
    n = len(lowered)
    out: list[str] = []
    w = out.append

    w("// GENERATED FILE - DO NOT EDIT.")
    w("//")
    w("// Generator : bridges/lean4/lean_to_cpp.py   (regenerate with: python bridges/lean4/lean_to_cpp.py)")
    w(f"// Input     : rules.json, schema {doc['schema']}, Lean {ascii_comment(str(doc.get('lean_version', '?')))}")
    w(f"// SHA-256   : {sha}   (of the canonical JSON: sorted keys, no whitespace)")
    w(f"// Rules     : {n}")
    if doc.get("provenance"):
        w(f"// Provenance: {ascii_comment(str(doc['provenance']))}")
    w("//")
    if "PROVISIONAL" in str(doc.get("provenance", "")):
        w("// WARNING: PROVISIONAL INPUT.  This file was NOT produced by exporting kernel-checked theorems from")
        w("// Lean/Mathlib; regenerate it from a real rules.json before relying on it as 'verified'.")
        w("//")
        w("// Each rule below mirrors a Lean 4 theorem tagged @[simmy_rule], lowered")
    else:
        w("// Every rule below is a Lean 4 theorem (tagged @[simmy_rule], proof free of sorry, accepted by the Lean")
        w("// KERNEL), lowered")
    w("// to a flat constexpr expression pool.  This header only INGESTS them: the evaluator is")
    w("// simmynet/verified/RuleKit.hpp, the proofs live in Lean.  The static_asserts at the end of the file")
    w("// pin each rule's name, module and concrete type tag, and (for every tag whose arithmetic is constexpr)")
    w("// re-check the rule on a deterministic grid of instances, so a drift between Lean's semantics and the")
    w("// C++ evaluator is a compile error rather than a silent miscompile.")
    w("//")
    w("// Header-only, C++17, warning-free under -Wall -Wextra -Wpedantic, ASCII only.")
    w(f"#ifndef {GUARD}")
    w(f"#define {GUARD}")
    w("")
    w("#include <array>")
    w("#include <cstddef>")
    w("#include <cstdint>")
    w("#include <string_view>")
    w("")
    w('#include "simmynet/algebraic_ring.hpp"')
    w('#include "simmynet/verified/RuleKit.hpp"')
    w("")
    w("// Feature macros: defined exactly when the corresponding rule (and hence rewrite hook) is present.")
    for kind in sorted({lw["kind"] for lw in lowered if lw["kind"] in HOOKS}):
        w(f"#define SIMMYNET_VERIFIED_HAS_{kind.upper()} 1")
    w("")
    w("namespace simmynet::verified {")
    w("")
    w(f"inline constexpr std::string_view kSchema = {cpp_string(doc['schema'])};")
    w(f"inline constexpr std::string_view kLeanVersion = {cpp_string(str(doc.get('lean_version', '')))};")
    w(f"inline constexpr std::string_view kSourceSha256 = \"{sha}\";")
    w(f"inline constexpr std::size_t kRuleCount = {n};")
    w("")
    w("namespace generated {")

    for i, lw in enumerate(lowered):
        r, pool = lw["rule"], lw["pool"]
        w("")
        w("// " + "=" * 98)
        w(f"// [{i}] {ascii_comment(r['name'])}")
        w(f"//   Lean theorem : {ascii_comment(r['name'])}")
        w(f"//   Lean module  : {ascii_comment(r['module'])}")
        w(f"//   Type tag     : {lw['tag']}")
        w(f"//   Rule kind    : {lw['kind']}")
        w(f"//   Statement    : {ascii_comment(r['statement'])}")
        axioms = r.get("axioms")
        if axioms is None:
            w("//   Axioms       : (not recorded)")
        else:
            w(f"//   Axioms       : {ascii_comment(', '.join(axioms)) if axioms else '(none: kernel-only)'}")
        axioms = axioms or []
        if "Lean.ofReduceBool" in axioms:
            w("//   WARNING      : depends on Lean.ofReduceBool (native_decide): trusts the compiler, not just the kernel")
        w("// " + "=" * 98)
        w(f"inline constexpr Node kNodes{i}[] = {{")
        for j, (op, dom, a, b, k, m) in enumerate(pool.rows):
            w(f"    {{Op::{OPS[op][0]}, Dom::{DOMS[dom]}, {a}, {b}, {k}, {m}}},  // {j}: {ascii_comment(pool.notes[j])}")
        w("};")
        if lw["params"]:
            w(f"inline constexpr Param kParams{i}[] = {{")
            for (dom, hyp, m, pname, _) in lw["params"]:
                w(f"    {{Dom::{dom}, {hyp}, {m}}},  // {ascii_comment(pname)}")
            w("};")
        else:
            w(f"inline constexpr Param kParams{i}[1] = {{{{Dom::Nat, 0, -1}}}};  // placeholder: the rule has no parameters")
        w(f"inline constexpr Equation kEqs{i}[] = {{")
        for (lhs, rhs) in lw["eqs"]:
            w(f"    {{{lhs}, {rhs}}},")
        w("};")
    w("")
    w("}  // namespace generated")
    w("")
    w("// Rules sorted by Lean theorem name.")
    w(f"inline constexpr std::array<Rule, {n}> kRules = {{{{")
    for i, lw in enumerate(lowered):
        r, pool = lw["rule"], lw["pool"]
        w(f"    {{{cpp_string(r['name'])}, {cpp_string(r['module'])}, {cpp_string(lw['kind'])}, "
          f"TypeTag::{TYPE_TAGS[lw['tag']]},")
        w(f"     generated::kNodes{i}, {len(pool.rows)}, generated::kParams{i}, {len(lw['params'])}, "
          f"generated::kEqs{i}, {len(lw['eqs'])}}},")
    w("}};")
    w("")
    w("// Look a rule up by its fully qualified Lean name; nullptr if absent.")
    w("constexpr const Rule* find_rule(std::string_view lean_name) noexcept {")
    w("    for (const Rule& r : kRules)")
    w("        if (r.lean_name == lean_name) return &r;")
    w("    return nullptr;")
    w("}")
    w("")
    w("// Re-check every rule on its instance grid (including the So2Phasor rules, which need std::cos / std::sin")
    w("// and so cannot be static_asserted).  Returns true when every rule passes; fills `total` if given.")
    w("inline bool verify_all(Report* total = nullptr) noexcept {")
    w("    Report sum;")
    w("    bool all_ok = true;")
    w("    for (const Rule& r : kRules) {")
    w("        const Report rep = check_rule_runtime(r);")
    w("        sum.instances += rep.instances;")
    w("        sum.failures += rep.failures;")
    w("        sum.unsupported += rep.unsupported;")
    w("        all_ok = all_ok && rep.ok();")
    w("    }")
    w("    if (total != nullptr) *total = sum;")
    w("    return all_ok;")
    w("}")

    hooks = [lw for lw in lowered if lw["kind"] in HOOKS]
    if hooks:
        w("")
        w("// " + "=" * 98)
        w("// Rewrite hooks.  Generated only for rules that are present above; each is justified by the named")
        w("// Lean theorem and by nothing else.")
        w("// " + "=" * 98)
        for lw in hooks:
            w("")
            out.extend(HOOKS[lw["kind"]].format(name=ascii_comment(lw["rule"]["name"]), kind=lw["kind"]).rstrip("\n").split("\n"))


    w("")
    w("// " + "=" * 98)
    w("// Invariants: Lean theorem name, module and concrete type tag are part of the type system of this header.")
    w("// " + "=" * 98)
    for i, lw in enumerate(lowered):
        r = lw["rule"]
        tag = TYPE_TAGS[lw["tag"]]
        tag_s = f"TypeTag::{tag}"
        w(f"// [{i}] {ascii_comment(r['name'])} : {lw['tag']}  (module {ascii_comment(r['module'])})")
        w(f"static_assert(kRules[{i}].lean_name == {cpp_string(r['name'])}, \"Lean theorem name\");")
        w(f"static_assert(kRules[{i}].module == {cpp_string(r['module'])}, \"Lean module path\");")
        w(f"static_assert(kRules[{i}].tag == {tag_s}, {cpp_string(r['name'] + ': concrete type tag ' + lw['tag'])});")
        w(f"static_assert(kRules[{i}].n_eqs > 0 && kRules[{i}].n_nodes > 0, \"rule has a non-empty expression pool\");")
        if lw["tag"] != "so2_phasor":
            w(f"static_assert(check_rule<{tag_s}>(kRules[{i}]).ok(),")
            w(f"              {cpp_string(r['name'] + ': C++ evaluation disagrees with the Lean theorem on a checked instance')});")
        else:
            w("// (so2_phasor uses std::cos / std::sin: re-checked at run time by verify_all())")
    w("")
    w("}  // namespace simmynet::verified")
    w("")
    w(f"#endif  // {GUARD}")
    return "\n".join(out) + "\n"


def generate(doc_text: str) -> str:
    try:
        doc = json.loads(doc_text)
    except json.JSONDecodeError as e:
        raise RuleError(f"input is not valid JSON: {e}") from e
    rules = validate_document(doc)
    errors: list[str] = []
    lowered: list[dict] = []
    for r in rules:
        try:
            lowered.append(lower_rule(r))
        except RuleError as e:
            errors.append(str(e))
        except (KeyError, TypeError, ValueError) as e:
            errors.append(f"{r.get('name', '?')}: malformed rule ({type(e).__name__}: {e})")
    if errors:
        raise RuleError("\n".join(errors))
    canonical = json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    return emit(doc, lowered, canonical)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0], formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("rules", nargs="?", default="rules.json", help="input JSON written by #export_simmy_rules (default: rules.json)")
    ap.add_argument("-o", "--output", default=str(DEFAULT_OUT), help=f"output header (default: {DEFAULT_OUT})")
    ap.add_argument("--check", action="store_true", help="do not write; exit 1 if the output file differs from what would be generated")
    ap.add_argument("--stdout", action="store_true", help="print the header instead of writing it")
    args = ap.parse_args(argv)

    try:
        text = Path(args.rules).read_text(encoding="utf-8")
        header = generate(text)
    except OSError as e:
        print(f"lean_to_cpp: cannot read {args.rules}: {e}", file=sys.stderr)
        return 2
    except RuleError as e:
        print(f"lean_to_cpp: refusing to generate, {len(str(e).splitlines())} problem(s):\n{e}", file=sys.stderr)
        return 2

    if args.stdout:
        sys.stdout.write(header)
        return 0
    out = Path(args.output)
    if args.check:
        current = out.read_bytes().decode("utf-8") if out.exists() else None
        if current != header:
            print(f"lean_to_cpp: {out} is stale or missing; regenerate it with this script", file=sys.stderr)
            return 1
        print(f"lean_to_cpp: {out} is up to date")
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(header.encode("utf-8"))  # bytes: no platform newline translation
    print(f"lean_to_cpp: wrote {out} ({len(header.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
