"""SimmyNet: a symbolic DSL compiler for group-theoretic classifiers, with an exact finite-field engine.

``simmynet`` is a package: the compiled C++ core lives in ``simmynet._core`` and is
re-exported here (``Matrix2x2``, ``CompiledWord``, ``eval_word_compiled``, the ``dsl`` and
``finite_field`` submodules, the exception hierarchy), and ``simmynet.wrapper`` adds the
Pythonic deferred-execution layer (``Generator``, ``Word``, ``DSLGroup``, ...).
"""
import sys as _sys

from . import _core
from ._core import *  # noqa: F401,F403  (matrices, CompiledWord, eval_word_compiled, gemm_count, exceptions, dsl, finite_field)
from ._core import __version__, dsl, finite_field  # noqa: F401

from . import wrapper
from .wrapper import (  # noqa: F401
    CheckOutcome, DSLGroup, DSLProgram, DSLWord, GenRef, Generator, SynthesisResult, Word,
    phasor_check, reset_stats, stats, synthesize_space, synthesize_subgroup,
)

# `import simmynet.dsl` / `from simmynet.finite_field import lift` work as submodule imports
_sys.modules.setdefault(__name__ + ".dsl", dsl)
_sys.modules.setdefault(__name__ + ".finite_field", finite_field)
