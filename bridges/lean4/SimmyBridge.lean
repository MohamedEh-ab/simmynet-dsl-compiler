/-
SimmyBridge.lean - Lean 4 (Mathlib) -> SimmyNet ingestion hook.

  * `@[simmy_rule]`            tags a THEOREM of the form `lhs = rhs` (or a conjunction of such
                               equations) as a rewrite rule SimmyNet may ingest.
  * `#export_simmy_rules "f"`  walks every tagged declaration visible in the current environment,
                               lowers its `Lean.Expr` statement to a structured JSON AST and writes
                               it, deterministically, to `f`.  `lean_to_cpp.py` turns that file into
                               `include/simmynet/verified/LeanVerifiedRules.hpp`.

This module imports only `Lean` (no Mathlib): it recognises Mathlib's `ZMod`, `DihedralGroup`, `Real`
by constant NAME, so it elaborates with or without Mathlib on the path.  The tagged theorems
themselves live in `SimmyBridge/Examples.lean`, which does import Mathlib.  (A tag attribute cannot
be used in the file that registers it, hence the split.)

TRUST MODEL
  The Lean kernel checks the theorem; this file only READS its statement.  A rule is refused when its
  proof depends on `sorryAx`.  Every operator in the statement must carry the canonical instance for
  its carrier type (checked with `isDefEq` against the synthesised instance), so a user-defined
  `Add` on `ZMod p` cannot masquerade as the standard one.

Supported statement language (anything else is rejected with an error naming the offending term):

  carrier types   ZMod n | DihedralGroup n | ℝ | ℕ | ℤ              (n a ℕ-expression over parameters)
  operations      + - * / ^ (ℕ exponent)  neg  inv  %  Nat.cast  ZMod.val
                  DihedralGroup.r / .sr   Real.pi  Real.cos  Real.sin   numerals
  hypotheses      [Fact (Nat.Prime p)]  [NeZero n]   (other instance arguments are ignored)

Concrete SimmyNet type tags (the exporter's "resolution" of the polymorphic Mathlib instances):

  finite_field     ZMod p arithmetic (or ℕ/ℤ with `%`)       dihedral          DihedralGroup n words
  so2_phasor       ℝ with cos/sin: SO(2) rotation identities   algebraic_tuple   ℝ in span{1, π}
-/
import Lean

open Lean Meta Elab Command

namespace SimmyBridge

/-- `@[simmy_rule]`: mark a theorem `lhs = rhs` for export to SimmyNet with `#export_simmy_rules`. -/
initialize simmyRuleAttr : TagAttribute ←
  registerTagAttribute `simmy_rule
    "Mark a theorem `lhs = rhs` as a rewrite rule that `#export_simmy_rules` exports to SimmyNet"
    (validate := fun declName => do
      let info ← getConstInfo declName
      unless info.isTheorem do
        throwError "@[simmy_rule] applies to theorems only, but '{declName}' is not a theorem")

/-! ### Value domains -/

/-- The value domain of a lowered term. `zmod`/`dihedral` carry their modulus (a ℕ-valued AST node). -/
inductive Dom where
  | nat | int | real
  | zmod (modulus : Json)
  | dihedral (n : Json)

def Dom.name : Dom → String
  | .nat => "nat" | .int => "int" | .real => "real" | .zmod _ => "zmod" | .dihedral _ => "dihedral"

def Dom.modulus? : Dom → Option Json
  | .zmod m => some m
  | .dihedral n => some n
  | _ => none

/-- Same domain, including equal moduli. -/
def Dom.same (a b : Dom) : Bool :=
  a.name == b.name && (a.modulus?.map (·.compress)) == (b.modulus?.map (·.compress))

/-- JSON node: `{op, dom, [modulus], ...extra}`. -/
def mkNode (op : String) (d : Dom) (extra : List (String × Json)) : Json :=
  Json.mkObj <| [("op", Json.str op), ("dom", Json.str d.name)] ++
    (match d.modulus? with | some m => [("modulus", m)] | none => []) ++ extra

/-- Lowering context: the parameters (fvar, user name, domain) bound so far. -/
abbrev LowerM := ReaderT (Array (FVarId × String × Dom)) MetaM

/-- The operator's instance argument must be the canonical instance for its type. -/
def checkInst (inst : Expr) : MetaM Unit := do
  let ty ← inferType inst
  match ← trySynthInstance ty with
  | .some expected =>
    unless ← withReducibleAndInstances <| isDefEq inst expected do
      throwError "simmy_rule: non-canonical instance {inst} for {ty}; SimmyNet only models the standard instance"
  | _ => throwError "simmy_rule: no instance of {ty} could be synthesised"

mutual

/-- Resolve a carrier type to a SimmyNet value domain (the "type tag" of the operands). -/
partial def lowerDom (ty : Expr) : LowerM Dom := do
  let ty ← whnfR (← instantiateMVars ty)
  match ty.getAppFnArgs with
  | (`Nat, #[]) => return .nat
  | (`Int, #[]) => return .int
  | (`Real, #[]) => return .real
  | (`ZMod, #[n]) => return .zmod (← lowerNat n)
  | (`DihedralGroup, #[n]) => return .dihedral (← lowerNat n)
  | _ =>
    let hint := if ty.isFVar then
      " (a generic carrier: state a specialised corollary on ZMod p / DihedralGroup n / ℝ and tag that instead)"
    else ""
    throwError "simmy_rule: carrier type {ty} has no concrete SimmyNet type tag{hint}"

/-- Lower a ℕ-valued term (moduli, exponents). -/
partial def lowerNat (e : Expr) : LowerM Json := do
  let (j, d) ← lower e
  unless d.name == "nat" do throwError "simmy_rule: expected a ℕ-valued term, got {e}"
  return j

partial def binop (op : String) (α inst a b : Expr) : LowerM (Json × Dom) := do
  checkInst inst
  let d ← lowerDom α
  let (ja, da) ← lower a
  let (jb, db) ← lower b
  unless da.same d && db.same d do
    throwError "simmy_rule: operands of '{op}' are not both in {α}"
  return (mkNode op d [("args", Json.arr #[ja, jb])], d)

partial def unop (op : String) (α inst a : Expr) : LowerM (Json × Dom) := do
  checkInst inst
  let d ← lowerDom α
  let (ja, da) ← lower a
  unless da.same d do throwError "simmy_rule: operand of '{op}' is not in {α}"
  return (mkNode op d [("args", Json.arr #[ja])], d)

/-- Lower an expression to `(json, domain)`. -/
partial def lower (e : Expr) : LowerM (Json × Dom) := do
  let e ← instantiateMVars e
  match_expr e with
  | HAdd.hAdd α _ _ inst a b => binop "add" α inst a b
  | HSub.hSub α _ _ inst a b => binop "sub" α inst a b
  | HMul.hMul α _ _ inst a b => binop "mul" α inst a b
  | HDiv.hDiv α _ _ inst a b => binop "div" α inst a b
  | HMod.hMod α _ _ inst a b => binop "mod" α inst a b
  | Neg.neg α inst a => unop "neg" α inst a
  | Inv.inv α inst a => unop "inv" α inst a
  | HPow.hPow α β _ inst a b =>
    checkInst inst
    let d ← lowerDom α
    unless (← lowerDom β).name == "nat" do
      throwError "simmy_rule: only ℕ exponents are supported, got an exponent in {β}"
    let (ja, da) ← lower a
    unless da.same d do throwError "simmy_rule: base of '^' is not in {α}"
    let jb ← lowerNat b
    return (mkNode "pow" d [("args", Json.arr #[ja, jb])], d)
  | OfNat.ofNat α n _ =>
    let d ← lowerDom α
    let some v := n.rawNatLit? | throwError "simmy_rule: non-literal numeral {e}"
    match d with
    | .dihedral _ =>
      unless v == 1 do throwError "simmy_rule: only the group identity `1` is a numeral of {α}"
      return (mkNode "one" d [], d)
    | _ => return (mkNode "lit" d [("value", Json.str (toString v))], d)
  | Nat.cast R _ n =>
    let d ← lowerDom R
    return (mkNode "cast" d [("args", Json.arr #[← lowerNat n])], d)
  | _ =>
    match e.getAppFnArgs with
    | (`ZMod.val, #[n, x]) =>
      let mj ← lowerNat n
      let (jx, dx) ← lower x
      unless dx.same (.zmod mj) do throwError "simmy_rule: argument of ZMod.val is not in ZMod {n}"
      return (mkNode "val" .nat [("args", Json.arr #[jx])], .nat)
    | (`Real.pi, #[]) => return (mkNode "pi" .real [], .real)
    | (`Real.cos, #[x]) => realFn "cos" x
    | (`Real.sin, #[x]) => realFn "sin" x
    | (`DihedralGroup.r, #[n, i]) => dihCtor "dih_r" n i
    | (`DihedralGroup.sr, #[n, i]) => dihCtor "dih_sr" n i
    | _ =>
    if let .fvar id := e then
      match (← read).find? (·.1 == id) with
      | some (_, name, d) => return (mkNode "var" d [("name", Json.str name)], d)
      | none => throwError "simmy_rule: variable {e} is not a parameter of the theorem"
    else
      let head := match e.getAppFn with
        | .const c _ => s!"constant '{c}'"
        | _ => "term"
      throwError "simmy_rule: unsupported {head} in {e}"

partial def realFn (op : String) (x : Expr) : LowerM (Json × Dom) := do
  let (jx, dx) ← lower x
  unless dx.name == "real" do throwError "simmy_rule: Real.{op} of a non-real term"
  return (mkNode op .real [("args", Json.arr #[jx])], .real)

partial def dihCtor (op : String) (n i : Expr) : LowerM (Json × Dom) := do
  let nj ← lowerNat n
  let d := Dom.dihedral nj
  let (ji, di) ← lower i
  unless di.same (.zmod nj) do
    throwError "simmy_rule: the index of DihedralGroup.{if op == "dih_r" then "r" else "sr"} is not in ZMod {n}"
  return (mkNode op d [("args", Json.arr #[ji])], d)

end

/-! ### Type-tag resolution -/

/-- All `op` strings occurring in a lowered AST (through `args`). -/
partial def opsOf (j : Json) : List String :=
  let here := match j.getObjValAs? String "op" with | .ok s => [s] | .error _ => []
  let kids := match j.getObjVal? "args" with
    | .ok (.arr xs) => xs.toList.flatMap opsOf
    | _ => []
  here ++ kids

/-- A ℝ-numeral: no variables and no π. -/
partial def isNumeral (j : Json) : Bool :=
  match j.getObjValAs? String "op" with
  | .ok "lit" => true
  | .ok "var" | .ok "pi" => false
  | .ok _ => match j.getObjVal? "args" with
    | .ok (.arr xs) => xs.all isNumeral
    | _ => false
  | .error _ => false

/-- Is this ℝ-term in span{1, π} with scalar `*` and `/` only? -/
partial def inTupleModule (j : Json) : Bool :=
  let op := (j.getObjValAs? String "op").toOption.getD ""
  let args := match j.getObjVal? "args" with | .ok (.arr xs) => xs.toList | _ => []
  match op with
  | "var" | "lit" | "pi" => true
  | "add" | "sub" | "neg" | "cast" => args.all inTupleModule
  | "mul" => args.all inTupleModule && args.any isNumeral
  | "div" => match args with
    | [a, b] => inTupleModule a && isNumeral b
    | _ => false
  | _ => false

/-- Resolve the concrete SimmyNet type tag of a rule from the domain and operators of its equations. -/
def resolveTag (d : Dom) (sides : List Json) : MetaM String := do
  let ops := sides.flatMap opsOf
  match d with
  | .zmod _ => return "finite_field"
  | .dihedral _ => return "dihedral"
  | .nat | .int =>
    if ops.contains "mod" || ops.contains "val" then return "finite_field"
    else throwError "simmy_rule: a ℕ/ℤ rule needs `%` to count as modular arithmetic"
  | .real =>
    if ops.contains "cos" || ops.contains "sin" then return "so2_phasor"
    else if sides.all inTupleModule then return "algebraic_tuple"
    else throwError "simmy_rule: a real rule must use cos/sin (so2_phasor) or stay in span\{1, π} (algebraic_tuple)"

/-! ### Rule lowering -/

private def collapse (s : String) : String := Id.run do
  let mut out := ""
  let mut pendingSpace := false
  for c in s.toList do
    if c.isWhitespace then pendingSpace := true
    else
      if pendingSpace && !out.isEmpty then out := out.push ' '
      pendingSpace := false
      out := out.push c
  return out

/-- Flatten `A ∧ B ∧ ...` into equations. -/
partial def lowerEqs (e : Expr) : LowerM (Array (Json × Json × Dom)) := do
  let e ← whnfR (← instantiateMVars e)
  match_expr e with
  | And p q => return (← lowerEqs p) ++ (← lowerEqs q)
  | Eq α a b =>
    let d ← lowerDom α
    let (ja, da) ← lower a
    let (jb, db) ← lower b
    unless da.same d && db.same d do throwError "simmy_rule: sides of `=` are not both in {α}"
    return #[(ja, jb, d)]
  | _ => throwError "simmy_rule: conclusion must be `lhs = rhs` (or a conjunction of such equations), got {e}"

/-- Lower one tagged theorem to its JSON rule. -/
def lowerRule (declName : Name) : MetaM Json := do
  let info ← getConstInfo declName
  unless info.isTheorem do throwError "not a theorem"
  let axioms ← collectAxioms declName
  if axioms.contains ``sorryAx then
    throwError "proof depends on `sorryAx`: only kernel-checked theorems may be exported"
  let env ← getEnv
  let modName ← match env.getModuleIdxFor? declName with
    | some idx => pure env.header.moduleNames[idx.toNat]!
    | none => getMainModule
  let statement := collapse (← ppExpr info.type).pretty
  forallTelescope info.type fun xs concl => do
    let mut ctx : Array (FVarId × String × Dom) := #[]
    let mut hyps : Array (Array String) := #[]
    for x in xs do
      let ld ← x.fvarId!.getDecl
      let ty ← instantiateMVars ld.type
      if ld.binderInfo.isInstImplicit then
        let slotOf (v : Expr) (h : String) : MetaM Nat := do
          unless v.isFVar do throwError "simmy_rule: hypothesis {h} must be about a parameter, got {v}"
          match ctx.findIdx? (·.1 == v.fvarId!) with
          | some i => pure i
          | none => throwError "simmy_rule: hypothesis {h} refers to {v}, which is not a data parameter"
        match ty.getAppFnArgs with
        | (`Fact, #[P]) =>
          match P.getAppFnArgs with
          | (`Nat.Prime, #[p]) =>
            hyps := hyps.modify (← slotOf p "prime") (·.push "prime")
          | _ => pure ()
        | (`NeZero, #[_, _, n]) =>
          hyps := hyps.modify (← slotOf n "nonzero") (·.push "nonzero")
        | _ => pure ()
      else
        if ← isProp ty then
          throwError "simmy_rule: explicit hypothesis ({ld.userName} : {ty}) is not supported; use [Fact (Nat.Prime p)] or [NeZero n]"
        if ty.isSort then
          throwError "simmy_rule: type parameter '{ld.userName}' is a generic carrier (Ring/Group/...): state a specialised corollary on ZMod p / DihedralGroup n / ℝ and tag that instead"
        let d ← (lowerDom ty).run ctx
        let nm := ld.userName.eraseMacroScopes.toString
        if ctx.any (·.2.1 == nm) then throwError "simmy_rule: duplicate parameter name '{nm}'"
        ctx := ctx.push (x.fvarId!, nm, d)
        hyps := hyps.push #[]
    let eqs ← (lowerEqs concl).run ctx
    let some (_, _, d0) := eqs[0]? | throwError "simmy_rule: no equations"
    let tag ← resolveTag d0 (eqs.toList.flatMap fun (a, b, _) => [a, b])
    for (_, _, d) in eqs do
      unless (← resolveTag d (eqs.toList.flatMap fun (a, b, _) => [a, b])) == tag do
        throwError "simmy_rule: equations of one rule must share a single type tag"
    let params := ctx.mapIdx fun i (_, nm, d) =>
      Json.mkObj <| [("name", Json.str nm), ("dom", Json.str d.name),
                     ("hyps", Json.arr (hyps[i]!.map Json.str))] ++
        (match d.modulus? with | some m => [("modulus", m)] | none => [])
    let axs := (axioms.map toString).qsort (· < ·)
    return Json.mkObj [
      ("name", Json.str declName.toString),
      ("module", Json.str modName.toString),
      ("type_tag", Json.str tag),
      ("statement", Json.str statement),
      ("axioms", Json.arr (axs.map Json.str)),
      ("params", Json.arr params),
      ("equations", Json.arr (eqs.map fun (a, b, _) => Json.mkObj [("lhs", a), ("rhs", b)]))]

/-! ### The export command -/

/-- Every `@[simmy_rule]` declaration in the environment, sorted by name (deterministic). -/
def taggedRules (env : Environment) : Array Name := Id.run do
  let mut acc : Array Name := #[]
  for i in [0:env.header.moduleNames.size] do
    for n in simmyRuleAttr.ext.getModuleEntries env i do
      acc := acc.push n
  for n in simmyRuleAttr.ext.getState env do
    acc := acc.push n
  return (acc.qsort fun a b => a.toString < b.toString).toList.eraseDups.toArray

/-- `#export_simmy_rules "rules.json"` writes every `@[simmy_rule]` theorem as structured JSON.
The path is relative to the working directory of the `lean` process. Nothing is written if any
rule is rejected (all rejections are reported together). -/
elab "#export_simmy_rules " path:str : command => do
  let file := path.getString
  let names := taggedRules (← getEnv)
  let (rules, errors) ← liftTermElabM do
    let mut rules : Array Json := #[]
    let mut errors : Array String := #[]
    for n in names do
      try
        rules := rules.push (← lowerRule n)
      catch ex =>
        errors := errors.push s!"  {n}: {← ex.toMessageData.toString}"
    return (rules, errors)
  if names.isEmpty then
    throwError "#export_simmy_rules: no @[simmy_rule] declarations in the environment (is the module that tags them imported?)"
  unless errors.isEmpty do
    throwError "#export_simmy_rules: {errors.size} rule(s) rejected, nothing written:\n{"\n".intercalate errors.toList}"
  let doc := Json.mkObj [("schema", Json.str "simmynet.lean-rules/1"),
                         ("lean_version", Json.str Lean.versionString),
                         ("rules", Json.arr rules)]
  if let some dir := (System.FilePath.mk file).parent then
    unless dir.toString.isEmpty do IO.FS.createDirAll dir
  IO.FS.writeFile file (doc.pretty ++ "\n")
  logInfo m!"simmy: exported {rules.size} rule(s) to {file}"

end SimmyBridge
