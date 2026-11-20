/-
SimmyBridge/Examples.lean - kernel-checked Mathlib theorems tagged `@[simmy_rule]`.

Each theorem below is proved from Mathlib in the usual way, so the Lean KERNEL has certified it for
ALL values of its parameters.  `#export_simmy_rules` (see `Export.lean`) then lowers its statement to
JSON, `lean_to_cpp.py` turns that into `include/simmynet/verified/LeanVerifiedRules.hpp`, and
SimmyNet's C++ side re-checks it on a grid of instances at compile time.

  1. `dihedral_srs_inv`   dihedral / cyclic group WORD REDUCTION      type tag  dihedral
  2. `so2_angle_add`      SO(2) angle-addition FOLD  R(a) R(b) = R(a+b)  type tag  so2_phasor
  3. `frobenius_fixed`    finite-field Frobenius identity x^p = x     type tag  finite_field
  +  `quarter_turns`      bonus: pi/4 + pi/4 = pi/2 (exact span{1, pi})  type tag  algebraic_tuple
-/
import Mathlib.FieldTheory.Finite.Basic
import Mathlib.GroupTheory.SpecificGroups.Dihedral
import Mathlib.Analysis.SpecialFunctions.Trigonometric.Basic
import SimmyBridge

namespace SimmyBridge.Examples

/-- Word reduction in the dihedral group `D_n = ⟨r, s⟩` with `r = r 1`, `s = sr 0`:
the defining relation `s r s = r⁻¹`.  Valid for every `n` (including the infinite dihedral group at
`n = 0`).  SimmyNet uses it as the peephole rewrite `s r s → r⁻¹` on group words. -/
@[simmy_rule]
theorem dihedral_srs_inv (n : ℕ) :
    (DihedralGroup.sr 0 : DihedralGroup n) * DihedralGroup.r 1 * DihedralGroup.sr 0 =
      (DihedralGroup.r 1 : DihedralGroup n)⁻¹ := by
  first
    | simp [DihedralGroup.sr_mul_r, DihedralGroup.sr_mul_sr, DihedralGroup.inv_r]
    | (rw [DihedralGroup.inv_r, DihedralGroup.sr_mul_r, DihedralGroup.sr_mul_sr]; simp)

/-- SO(2) angle addition: the entries of `R(a) R(b)` are those of `R(a + b)` for
`R(θ) = [[cos θ, -sin θ], [sin θ, cos θ]]` (cosine entry and sine entry).  SimmyNet folds a product of
phasor generators into one generator whose angle is the (exact) sum of the angles. -/
@[simmy_rule]
theorem so2_angle_add (a b : ℝ) :
    Real.cos (a + b) = Real.cos a * Real.cos b - Real.sin a * Real.sin b ∧
    Real.sin (a + b) = Real.sin a * Real.cos b + Real.cos a * Real.sin b :=
  ⟨Real.cos_add a b, Real.sin_add a b⟩

/-- Frobenius identity (Fermat's little theorem): `x ^ p = x` in `ZMod p` for prime `p`. -/
@[simmy_rule]
theorem frobenius_fixed (p : ℕ) [Fact p.Prime] (x : ZMod p) : x ^ p = x :=
  ZMod.pow_card x

/-- Bonus: an exact identity in the module `span{1, π}` that SimmyNet's `AlgebraicTuple` represents. -/
@[simmy_rule]
theorem quarter_turns : (Real.pi / 4 : ℝ) + Real.pi / 4 = Real.pi / 2 := by ring

end SimmyBridge.Examples
