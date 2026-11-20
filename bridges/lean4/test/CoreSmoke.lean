/-
test/CoreSmoke.lean - Mathlib-FREE smoke test of the exporter in SimmyBridge.lean.

Stand-ins with the same constant names and shapes as Mathlib's `ZMod`, `DihedralGroup`, `Real`,
`Fact`, `Nat.Prime`, so the real lowering code path (carrier resolution, canonical-instance checks,
type tags, hypotheses, JSON emission) runs with only Lean core installed.  The "proofs" are a mock
axiom: this file tests the EXPORTER, not mathematics.  The kernel-checked theorems are in
SimmyBridge/Examples.lean.

    lake env lean test/CoreSmoke.lean          (writes test/core_smoke_rules.json)
-/
import SimmyBridge
open SimmyBridge

axiom mock {P : Prop} : P

class Fact (p : Prop) : Prop where out : p
def Nat.Prime (n : Nat) : Prop := 2 ≤ n

def ZMod (_n : Nat) : Type := Int
instance (n : Nat) : Add (ZMod n) := inferInstanceAs (Add Int)
instance (n : Nat) : Sub (ZMod n) := inferInstanceAs (Sub Int)
instance (n : Nat) : Mul (ZMod n) := inferInstanceAs (Mul Int)
instance (n : Nat) : Neg (ZMod n) := inferInstanceAs (Neg Int)
instance (n : Nat) : Pow (ZMod n) Nat := inferInstanceAs (Pow Int Nat)
instance (n k : Nat) : OfNat (ZMod n) k := inferInstanceAs (OfNat Int k)

inductive DihedralGroup (_n : Nat) : Type where
  | r : ZMod _n → DihedralGroup _n
  | sr : ZMod _n → DihedralGroup _n
instance (n : Nat) : Mul (DihedralGroup n) := ⟨fun x _ => x⟩
instance (n : Nat) : Inv (DihedralGroup n) := ⟨fun x => x⟩

axiom Real : Type
axiom Real.pi : Real
axiom Real.cos : Real → Real
axiom Real.sin : Real → Real
axiom Real.add : Real → Real → Real
axiom Real.sub : Real → Real → Real
axiom Real.mul : Real → Real → Real
axiom Real.div : Real → Real → Real
axiom Real.ofNat : Nat → Real
noncomputable instance : Add Real := ⟨Real.add⟩
noncomputable instance : Sub Real := ⟨Real.sub⟩
noncomputable instance : Mul Real := ⟨Real.mul⟩
noncomputable instance : Div Real := ⟨Real.div⟩
noncomputable instance (k : Nat) : OfNat Real k := ⟨Real.ofNat k⟩

@[simmy_rule] theorem frobenius_fixed (p : Nat) [Fact (Nat.Prime p)] (x : ZMod p) : x ^ p = x := mock

@[simmy_rule] theorem so2_angle_add (a b : Real) :
    Real.cos (a + b) = Real.cos a * Real.cos b - Real.sin a * Real.sin b ∧
    Real.sin (a + b) = Real.sin a * Real.cos b + Real.cos a * Real.sin b := mock

@[simmy_rule] theorem dihedral_srs (n : Nat) :
    (DihedralGroup.sr 0 : DihedralGroup n) * DihedralGroup.r 1 * DihedralGroup.sr 0 =
      (DihedralGroup.r 1 : DihedralGroup n)⁻¹ := mock

@[simmy_rule] theorem quarter_turns : (Real.pi / 4 : Real) + Real.pi / 4 = Real.pi / 2 := mock

#export_simmy_rules "test/core_smoke_rules.json"
