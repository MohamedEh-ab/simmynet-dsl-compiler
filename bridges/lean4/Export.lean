/-
Export.lean - write every `@[simmy_rule]` theorem of the imported modules to rules.json.

    lake env lean Export.lean          # run from bridges/lean4; writes ./rules.json
    python lean_to_cpp.py              # rules.json -> include/simmynet/verified/LeanVerifiedRules.hpp

Add `import YourModule` below for your own `@[simmy_rule]` theorems.  The path is relative to the
directory `lean` is run from.  Running this through `lake env lean` (not `lake build`) makes it
execute every time instead of being cached.
-/
import SimmyBridge
import SimmyBridge.Examples

open SimmyBridge

#export_simmy_rules "rules.json"
