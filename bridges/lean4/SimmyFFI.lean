/-
SimmyFFI.lean - SimmyNet (C++17) as a native runtime ORACLE for Lean 4.

The `@[extern "simmy_..."]` declarations in `SimmyFFI.Raw` call the C ABI exported by
`libsimmynet_lean` (bridges/lean4/simmy_lean_ffi.cpp): exact O(p) Legendre point counting, the
Hasse-Weil bound and Frobenius traces of SimmyNet's `simmynet::ff` engine.  Only unboxed scalars
(`UInt64`, `Int64`, `Float`) cross the boundary; no `lean.h`.

══ TRUST MODEL ═══════════════════════════════════════════════════════════════════════════════════
  UNTRUSTED  Everything in `SimmyFFI.Raw` and `SimmyFFI.Oracle`.  Lean's kernel never sees the C++
             code, and the compiler/interpreter treat an `extern` as an opaque pure function.  Use
             these functions for `#eval`, witness search and testing ONLY.  A value they return is a
             candidate, not a theorem.
  CHECKED    `TraceCertificate` and `CheckedTrace`: the oracle's answer is re-validated by Lean
             definitions.  `decide` / `by decide +kernel` on a concrete certificate is kernel-checked;
             running `checkTrace?` at run time is checked by compiled Lean code (trusts the Lean
             compiler, not the C++ oracle).  Nothing here uses `native_decide`.

  The oracle can be wrong (a bug in C++, a mis-linked library) without compromising any theorem,
  because a wrong certificate fails the Lean check.  The converse never happens: a certificate that
  passes is correct regardless of where it came from.
═══════════════════════════════════════════════════════════════════════════════════════════════════

Elliptic curve convention: E : y² = x³ + a·x + b over F_p, p prime, 5 ≤ p ≤ 10⁶ (the oracle's domain).
-/

namespace SimmyFFI

/-! ### Raw C ABI (UNTRUSTED oracle primitives) -/

namespace Raw

/-- In-band error value of every `Int64`-returning oracle function (`INT64_MIN`). -/
def err : Int64 := Int64.minValue

/-- Version of the C ABI (1). The argument is ignored. -/
@[extern "simmy_lean_abi_version"] opaque abiVersion (unused : UInt64) : UInt64
/-- `SIMMYNET_VERSION_NUMBER` of the linked library (2.2.0 ↦ 20200). -/
@[extern "simmy_version_number"] opaque versionNumber (unused : UInt64) : UInt64
/-- `#E(F_p)` including the point at infinity, or `err`. -/
@[extern "simmy_legendre_count_points"] opaque legendreCountPoints (a b p : Int64) : Int64
/-- Frobenius trace `a_p = p + 1 - #E(F_p)`, or `err`. -/
@[extern "simmy_frobenius_trace"] opaque frobeniusTrace (a b p : Int64) : Int64
/-- `Tr(F^n)`, or `err` (outside the domain or int64 overflow). -/
@[extern "simmy_frobenius_trace_power"] opaque frobeniusTracePower (a b p : Int64) (n : UInt64) : Int64
/-- `#E(F_{p^n})` for `n ≥ 1`, or `err`. -/
@[extern "simmy_extension_point_count"] opaque extensionPointCount (a b p : Int64) (n : UInt64) : Int64
/-- `1` iff `t² ≤ 4q` (exact 128-bit comparison), else `0`. -/
@[extern "simmy_hasse_bound_holds"] opaque hasseBoundHolds (t q : Int64) : UInt64
/-- `2√q - |t|` (informational). NaN for `q ≤ 0`. -/
@[extern "simmy_hasse_margin"] opaque hasseMargin (t q : Int64) : Float
/-- `t / (2√q)`, the Sato–Tate cosine. NaN for `q ≤ 0`. -/
@[extern "simmy_sato_tate_cos"] opaque satoTateCos (t q : Int64) : Float
/-- Weil certificate bit mask (bit 0 Hasse, 1 det = q, 2 Δ < 0, 3 functional equation, 4 pairing,
5 overflow; bit 63 = outside the domain). -/
@[extern "simmy_weil_certify"] opaque weilCertify (a b p : Int64) : UInt64

end Raw

/-! ### Oracle: `Option`-returning wrappers (still UNTRUSTED) -/

namespace Oracle

private def toI64? (i : Int) : Option Int64 :=
  if -(2 ^ 63 : Int) < i ∧ i < 2 ^ 63 then some (Int64.ofInt i) else none

private def fromRaw (r : Int64) : Option Int :=
  if r == Raw.err then none else some r.toInt

/-- `#E(F_p)`, or `none` outside the oracle's domain. -/
def countPoints (a b : Int) (p : Nat) : Option Nat := do
  let r ← fromRaw (Raw.legendreCountPoints (← toI64? a) (← toI64? b) (← toI64? p))
  guard (0 ≤ r)
  pure r.toNat

/-- Frobenius trace `a_p`, or `none`. -/
def frobeniusTrace (a b : Int) (p : Nat) : Option Int := do
  fromRaw (Raw.frobeniusTrace (← toI64? a) (← toI64? b) (← toI64? p))

/-- `Tr(F^n)`, or `none`. -/
def traceOfPower (a b : Int) (p n : Nat) : Option Int := do
  guard (n < 2 ^ 64)
  fromRaw (Raw.frobeniusTracePower (← toI64? a) (← toI64? b) (← toI64? p) n.toUInt64)

/-- `#E(F_{p^n})`, or `none`. -/
def extensionPointCount (a b : Int) (p n : Nat) : Option Int := do
  guard (0 < n ∧ n < 2 ^ 64)
  fromRaw (Raw.extensionPointCount (← toI64? a) (← toI64? b) (← toI64? p) n.toUInt64)

/-- The oracle's opinion on the Hasse–Weil bound (the checked version is `TraceCertificate.hasseOk`). -/
def hasseBoundHolds (t : Int) (q : Nat) : Option Bool := do
  pure (Raw.hasseBoundHolds (← toI64? t) (← toI64? q) == 1)

/-- Does the oracle's Weil certificate say PROVED for both the Hasse–Weil bound and the functional
equation? `none` outside the domain. -/
def weilProved (a b : Int) (p : Nat) : Option Bool := do
  let m := Raw.weilCertify (← toI64? a) (← toI64? b) (← toI64? p)
  if m >>> 63 == 1 then none else pure (m &&& 1 == 1 && (m >>> 3) &&& 1 == 1)

end Oracle

/-! ### Certificates: Lean checks what the oracle claims -/

/-- Specification of the affine point count: `#{(x, y) ∈ [0,p)² | y² ≡ x³ + a·x + b (mod p)}`.
O(p²), deliberately naive: it is the DEFINITION that the oracle's answer is checked against. -/
def affineCount (a b p : Nat) : Nat :=
  (List.range p).foldl (fun acc x =>
    acc + ((List.range p).filter (fun y => (y * y) % p == (x * x * x + a * x + b) % p)).length) 0

/-- `#E(F_p)` by definition (affine points plus the point at infinity). -/
def pointCountSpec (a b p : Nat) : Nat := affineCount a b p + 1

/-- A lightweight certificate returned by the oracle: the claimed Frobenius trace `t` of
`y² = x³ + a·x + b` over `F_p` (coefficients reduced mod `p`). -/
structure TraceCertificate where
  a : Nat
  b : Nat
  p : Nat
  t : Int
  deriving Repr, DecidableEq

/-- Cheap necessary condition (Hasse–Weil): `t² ≤ 4p`. Sound for ANY `p`, no recount. -/
def TraceCertificate.hasseOk (c : TraceCertificate) : Bool := c.t * c.t ≤ 4 * (c.p : Int)

/-- Full check: the claimed count `p + 1 - t` equals the specification. O(p²). -/
def TraceCertificate.recountOk (c : TraceCertificate) : Bool :=
  (pointCountSpec c.a c.b c.p : Int) == (c.p : Int) + 1 - c.t

/-- Ask the oracle for a certificate for `y² = x³ + a x + b` over `F_p`. UNTRUSTED. -/
def certificate? (a b : Int) (p : Nat) : Option TraceCertificate := do
  let t ← Oracle.frobeniusTrace a b p
  pure { a := (a % (p : Int)).toNat, b := (b % (p : Int)).toNat, p, t }

/-- A trace together with PROOFS of the Hasse–Weil bound and of the point-count equation. -/
structure CheckedTrace (a b p : Nat) where
  t : Int
  hasse : t * t ≤ 4 * (p : Int)
  count : (pointCountSpec a b p : Int) = (p : Int) + 1 - t

/-- Turn a claimed trace into a proof-carrying `CheckedTrace`, or reject it. `some _` means the two
propositions were decided by Lean code. -/
def checkTrace? (a b p : Nat) (t : Int) : Option (CheckedTrace a b p) :=
  if h1 : t * t ≤ 4 * (p : Int) then
    if h2 : (pointCountSpec a b p : Int) = (p : Int) + 1 - t then some ⟨t, h1, h2⟩ else none
  else none

/-- Witness search driven by the oracle: the least `b < p` for which `y² = x³ + a x + b` over `F_p`
has trace `t`, each candidate re-validated by `checkTrace?`. -/
def searchB? (a : Nat) (p : Nat) (t : Int) : Option Nat :=
  (List.range p).find? fun b =>
    match Oracle.frobeniusTrace a b p with
    | some t' => t' == t && (checkTrace? a b p t').isSome
    | none => false

/-! ### Kernel-checked certificates (no oracle, no `native_decide`) -/

/-- The README curve `y² = x³ + 2x + 3` over `F_97` has trace `-2`, so `#E(F_97) = 100`. -/
def example97 : TraceCertificate := ⟨2, 3, 97, -2⟩

theorem example97_hasse : example97.hasseOk = true := by decide

/-- Full recount, evaluated by the KERNEL (`decide +kernel`): `#E(F_97) = 98 - (-2) = 100`. -/
theorem example97_recount : example97.recountOk = true := by decide +kernel

/-- Any `TraceCertificate` accepted by the recount satisfies the count equation (soundness). -/
theorem recountOk_sound (c : TraceCertificate) (h : c.recountOk = true) :
    (pointCountSpec c.a c.b c.p : Int) = (c.p : Int) + 1 - c.t := by
  simpa [TraceCertificate.recountOk] using h

end SimmyFFI
