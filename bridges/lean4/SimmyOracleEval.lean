-- (see README.md "Oracle versus certificate": #eval results below come from the UNTRUSTED C++ oracle)
import SimmyFFI
open SimmyFFI

#eval Oracle.countPoints 2 3 97                -- some 100
#eval Oracle.frobeniusTrace 2 3 97             -- some (-2)
#eval (certificate? 2 3 97).map (·.recountOk)  -- some true
#eval Raw.hasseMargin (-2) 97                  -- 17.69...
