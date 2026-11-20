/-
SimmyOracleDemo.lean - `lake exe simmy_oracle_demo`: oracle (UNTRUSTED) -> certificate (CHECKED).

Exit code 0 iff every check passes, so it doubles as a smoke test of the C++/Lean link.
-/
import SimmyFFI

open SimmyFFI

/-- Print one check; returns whether it passed. -/
def check (label : String) (ok : Bool) : IO Bool := do
  IO.println s!"  [{if ok then "PASS" else "FAIL"}] {label}"
  pure ok

def main : IO UInt32 := do
  IO.println "== SimmyNet oracle (UNTRUSTED C++ primitives)"
  let abi := Raw.abiVersion 0
  let ver := Raw.versionNumber 0
  IO.println s!"  C ABI version {abi}, SimmyNet version number {ver}"
  let mut ok := true
  ok := (← check "ABI version is 1" (abi == 1)) && ok

  -- Curve y^2 = x^3 + 2x + 3 over F_97 (the README example).
  let (a, b, p) : Int × Int × Nat := (2, 3, 97)
  let count := Oracle.countPoints a b p
  let trace := Oracle.frobeniusTrace a b p
  IO.println s!"  #E(F_97) = {count}, trace = {trace}"
  ok := (← check "oracle: #E(F_97) = 100" (count == some 100)) && ok
  ok := (← check "oracle: a_97 = -2" (trace == some (-2))) && ok
  ok := (← check "oracle: #E(F_97^2) = 9600 (Lefschetz)" (Oracle.extensionPointCount a b p 2 == some 9600)) && ok
  ok := (← check "oracle: Tr(F^2) = -190" (Oracle.traceOfPower a b p 2 == some (-190))) && ok
  ok := (← check "oracle: Weil certificate PROVED" (Oracle.weilProved a b p == some true)) && ok
  ok := (← check "oracle: domain error for p = 100" (Oracle.countPoints a b 100 == none)) && ok

  IO.println "== Lean re-checks the oracle's certificate"
  match certificate? a b p with
  | none => ok := (← check "certificate obtained" false) && ok
  | some c =>
    ok := (← check "Hasse bound (t^2 <= 4p)" c.hasseOk) && ok
    ok := (← check "recount in Lean agrees (O(p^2) definition)" c.recountOk) && ok
    ok := (← check "proof-carrying CheckedTrace constructed" (checkTrace? c.a c.b c.p c.t).isSome) && ok
  -- A forged certificate must be rejected: claim trace 0 for the same curve.
  ok := (← check "forged trace 0 REJECTED" (checkTrace? 2 3 97 0).isNone) && ok
  ok := (← check "impossible trace 20 over F_97 REJECTED by Hasse" (!(TraceCertificate.hasseOk ⟨2, 3, 97, 20⟩))) && ok

  IO.println "== Witness search (oracle proposes, Lean verifies)"
  match searchB? 1 31 0 with
  | some b' =>
    IO.println s!"  y^2 = x^3 + x + {b'} over F_31 has trace 0"
    ok := (← check "witness re-validated" (checkTrace? 1 b' 31 0).isSome) && ok
  | none => ok := (← check "found a supersingular witness over F_31" false) && ok

  -- Large p: only the cheap Hasse certificate (the O(p^2) recount would be far too slow).
  match certificate? 2 3 999983 with
  | some c => ok := (← check s!"p = 999983: trace {c.t} passes Hasse" c.hasseOk) && ok
  | none => ok := (← check "certificate for p = 999983" false) && ok

  IO.println (if ok then "ALL CHECKS PASSED" else "SOME CHECKS FAILED")
  pure (if ok then 0 else 1)
