"""simmynet.wrapper - a Pythonic scratchpad over the compiled core, with DEFERRED execution.

Nothing in this module does group math. Building an expression only records what was asked for;
the C++ core is entered when (and only when) a result is needed:

1. Graph builder (lazy)
   ``Generator`` and ``Word`` overload ``*``, ``@`` and ``+``. They do zero arithmetic and make zero
   C++ calls: concatenating two words is an O(1) rope-node allocation (``r * s * r`` is three
   leaves and two nodes), so even a loop that builds a word of a million factors stays linear.

2. JIT lowering with transparent caching
   ``Word.decide(weight, bias)`` flattens the rope once, hands the factors to the C++
   ``CompiledWord`` (which folds the whole word into ONE k x k matrix), caches that artifact inside
   the Word, and decides. Every later call on the same Word is a single C++ call that costs O(1) in
   the word length. Words are immutable, so the cache can never go stale.

3. DSL string builders (flush once)
   ``DSLGroup`` / ``DSLProgram`` assemble DSL text in memory (nothing is compiled while you build)
   and hand it to the C++ interpreter exactly once. ``run()`` returns only a small result object;
   ``compile()`` returns the C++ ``Program`` to the caller, who owns it (the builder keeps no
   reference), and the high-level helpers (``synthesize_space`` ...) copy what they need into plain
   Python objects and drop the C++ snapshot before returning, so no C++ ``Program`` is ever orphaned.

``stats()`` reports how often the boundary was actually crossed, which makes the guarantees testable.
"""
from __future__ import annotations

import math
import re
import threading
from dataclasses import dataclass, field
from typing import Dict, Iterable, Iterator, List, Mapping, Optional, Sequence, Tuple, Union

from . import _core

Number = Union[int, float]

__all__ = [
    "Generator", "Word", "GenRef", "DSLWord", "DSLGroup", "DSLProgram",
    "SynthesisResult", "CheckOutcome", "synthesize_space", "synthesize_subgroup", "phasor_check",
    "stats", "reset_stats",
]

# --------------------------------------------------------------------------------------------
# boundary-crossing counters (so "deferred" is observable and testable)
# --------------------------------------------------------------------------------------------
_STATS = {"word_compiles": 0, "word_decides": 0, "dsl_flushes": 0}
_COMPILE_LOCK = threading.Lock()


def stats() -> Dict[str, int]:
    """How many times this layer entered the C++ core: word compilations, word decisions, DSL flushes."""
    return dict(_STATS)


def reset_stats() -> None:
    for k in _STATS:
        _STATS[k] = 0


# ============================================================================================
# 1. Graph builder: Generator / Word (lazy, immutable)
# ============================================================================================
_IDENT = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\Z")


def _clean(x: float) -> float:
    """Zero out floating-point dust, like the C++ cleanMatrix (also normalises -0.0)."""
    return 0.0 if abs(x) < 1e-12 else x + 0.0


class Generator:
    """A named k x k generator matrix (k = 2, 3 or 4). Immutable.

    ``Generator("s", [[-1, 0], [0, 1]])``, or the helpers ``rotation``, ``reflection``, ``phasor``,
    ``identity``. Combining generators with ``*``, ``@`` or ``+`` yields a lazy :class:`Word`.
    """

    __slots__ = ("name", "k", "_flat")

    def __init__(self, name: str, matrix: Union[Sequence[Sequence[Number]], Sequence[Number]]):
        if not isinstance(name, str) or not _IDENT.match(name):
            raise ValueError(f"invalid generator name {name!r}")
        rows = list(matrix)
        if rows and isinstance(rows[0], (list, tuple)):
            k = len(rows)
            if any(len(r) != k for r in rows):
                raise ValueError("a generator matrix must be square")
            flat = [float(x) for r in rows for x in r]
        else:
            k = math.isqrt(len(rows))
            if k * k != len(rows):
                raise ValueError("a flat generator matrix needs k*k entries")
            flat = [float(x) for x in rows]
        if k not in (2, 3, 4):
            raise ValueError("generators must be 2x2, 3x3 or 4x4")
        self.name, self.k, self._flat = name, k, tuple(flat)

    # -- constructors -------------------------------------------------------------------------
    @classmethod
    def identity(cls, k: int, name: str = "e") -> "Generator":
        return cls(name, [[1.0 if i == j else 0.0 for j in range(k)] for i in range(k)])

    @classmethod
    def phasor(cls, name: str, radians: float) -> "Generator":
        """R(theta) = [[cos, -sin], [sin, cos]], theta in radians."""
        c, s = math.cos(radians), math.sin(radians)
        return cls(name, [[_clean(c), _clean(-s)], [_clean(s), _clean(c)]])

    @classmethod
    def rotation(cls, name: str, degrees: float) -> "Generator":
        return cls.phasor(name, math.radians(degrees))

    @classmethod
    def reflection(cls, name: str, degrees: float) -> "Generator":
        a = math.radians(degrees)
        c, s = math.cos(2 * a), math.sin(2 * a)
        return cls(name, [[_clean(c), _clean(s)], [_clean(s), _clean(-c)]])

    @property
    def matrix(self) -> Tuple[Tuple[float, ...], ...]:
        k = self.k
        return tuple(tuple(self._flat[i * k:(i + 1) * k]) for i in range(k))

    # -- lazy operators (no math, no C++) --------------------------------------------------------
    def _word(self) -> "Word":
        return Word._leaf(self)

    def __mul__(self, other):
        return self._word()._concat(other)

    __matmul__ = __mul__
    __add__ = __mul__

    def __pow__(self, n: int) -> "Word":
        return self._word() ** n

    def __repr__(self) -> str:
        return f"Generator({self.name!r}, {self.k}x{self.k})"


class Word:
    """A lazy product of generators ``g1 * g2 * ... * gL``. Immutable; built in O(1) per operator.

    ``+`` is accepted as an alias of ``*`` (additive notation for the group operation, convenient for
    SO(2) angles); it is NOT matrix addition.
    """

    __slots__ = ("_gen", "_left", "_right", "_len", "_k", "_flat", "_compiled")

    # -- construction (private) -------------------------------------------------------------------
    @classmethod
    def _leaf(cls, gen: Generator) -> "Word":
        w = cls.__new__(cls)
        w._gen, w._left, w._right = gen, None, None
        w._len, w._k, w._flat, w._compiled = 1, gen.k, None, None
        return w

    @classmethod
    def identity(cls, k: int) -> "Word":
        """The empty word over k x k matrices (the identity)."""
        if k not in (2, 3, 4):
            raise ValueError("k must be 2, 3 or 4")
        w = cls.__new__(cls)
        w._gen, w._left, w._right = None, None, None
        w._len, w._k, w._flat, w._compiled = 0, k, (), None
        return w

    def _concat(self, other):
        if isinstance(other, Generator):
            other = Word._leaf(other)
        elif not isinstance(other, Word):
            return NotImplemented
        if self._k != other._k:
            raise ValueError(f"cannot combine words over {self._k}x{self._k} and {other._k}x{other._k} matrices")
        if self._len == 0:
            return other
        if other._len == 0:
            return self
        w = Word.__new__(Word)
        w._gen, w._left, w._right = None, self, other
        w._len, w._k, w._flat, w._compiled = self._len + other._len, self._k, None, None
        return w

    __mul__ = _concat
    __matmul__ = _concat
    __add__ = _concat

    def __rmul__(self, other):
        return Word._leaf(other)._concat(self) if isinstance(other, Generator) else NotImplemented

    __rmatmul__ = __rmul__
    __radd__ = __rmul__

    def __pow__(self, n: int) -> "Word":
        if not isinstance(n, int) or isinstance(n, bool):
            raise TypeError("exponent must be an int")
        if n < 0:
            raise ValueError("negative exponents need an inverse; build the inverse generator explicitly")
        result: Optional[Word] = None
        base = self
        while n:
            if n & 1:
                result = base if result is None else result._concat(base)
            n >>= 1
            if n:
                base = base._concat(base)  # shares the sub-rope: O(log n) nodes
        return result if result is not None else Word.identity(self._k)

    # -- inspection ----------------------------------------------------------------------------------
    @property
    def dim(self) -> int:
        return self._k

    def __len__(self) -> int:
        return self._len

    @property
    def factors(self) -> Tuple[Generator, ...]:
        """The flattened factors in word order (iterative; cached)."""
        flat = self._flat
        if flat is None:
            out: List[Generator] = []
            stack = [self]
            while stack:
                w = stack.pop()
                if w._gen is not None:
                    out.append(w._gen)
                elif w._flat is not None:
                    out.extend(w._flat)
                elif w._len:
                    stack.append(w._right)
                    stack.append(w._left)
            flat = self._flat = tuple(out)
        return flat

    def __iter__(self) -> Iterator[Generator]:
        return iter(self.factors)

    def __repr__(self) -> str:
        names = [g.name for g in self.factors[:8]]
        more = " * ..." if self._len > 8 else ""
        return f"Word({' * '.join(names) if names else 'e'}{more}; length {self._len}, dim {self._k})"

    # -- JIT lowering + cache ---------------------------------------------------------------------------
    @property
    def is_compiled(self) -> bool:
        return self._compiled is not None

    def compile(self):
        """Lower the lazy word to a C++ ``CompiledWord`` (one fold, W_compiled = F1 * ... * FL) and cache it."""
        cw = self._compiled
        if cw is not None:
            return cw
        with _COMPILE_LOCK:
            cw = self._compiled
            if cw is None:
                flat = [x for g in self.factors for x in g._flat]
                cw = _core.CompiledWord(self._k, flat)  # the single crossing that compiles the word
                self._compiled = cw
                _STATS["word_compiles"] += 1
        return cw

    def decide(self, weight: Sequence[Number], bias: Number = 0.0) -> bool:
        """Step(weight . vec(W_compiled) + bias). ``weight`` has k*k entries.

        First call: compiles and caches the word, then decides. Every later call is ONE C++ call whose
        cost does not depend on the word length.
        """
        cw = self._compiled
        if cw is None:
            cw = self.compile()
        _STATS["word_decides"] += 1
        return cw.decide(weight if isinstance(weight, list) else list(weight), float(bias))

    @property
    def matrix(self) -> List[float]:
        """W_compiled, row-major (compiles and caches on first use)."""
        return list(self.compile().matrix)

    def decide_once(self, weight: Sequence[Number], bias: Number = 0.0) -> bool:
        """One-shot decision that does not cache a compiled artifact (folds per call; for throwaway words)."""
        flat = [x for g in self.factors for x in g._flat]
        _STATS["word_decides"] += 1
        return _core.eval_word_compiled(self._k, flat, list(weight), float(bias))


# ============================================================================================
# 3. DSL string builders (flush exactly once)
# ============================================================================================
_KEYWORDS = frozenset(
    "group gen lift subgroup separate sector space synthesize eval classify verify check sweep span on of over "
    "coset class expect from to steps weight bias matrix rotation reflection phasor e pi".split())
_EXPR_OK = re.compile(r"[0-9A-Za-z_+\-*/(). ]*\Z")


def _ident(name: str, what: str) -> str:
    if not isinstance(name, str) or not _IDENT.match(name):
        raise ValueError(f"invalid {what} {name!r}")
    if name in _KEYWORDS:
        raise ValueError(f"{what} {name!r} is a reserved DSL word")
    return name


def _expr(x) -> str:
    """A number or an expression string such as 'pi/4' (validated: no braces, commas, comments or newlines)."""
    if isinstance(x, bool):
        raise TypeError("expected a number or an expression string, got bool")
    if isinstance(x, int):
        return str(x)
    if isinstance(x, float):
        if not math.isfinite(x):
            raise ValueError("numbers in a DSL program must be finite")
        return repr(x)
    if isinstance(x, str):
        t = x.strip()
        if not t or not _EXPR_OK.match(t):
            raise ValueError(f"invalid DSL expression {x!r}")
        return t
    raise TypeError(f"expected a number or an expression string, got {type(x).__name__}")


def _vec(xs: Iterable) -> str:
    return "[" + ", ".join(_expr(x) for x in xs) + "]"


def _point(xs: Iterable) -> str:
    return "(" + ", ".join(_expr(x) for x in xs) + ")"


class DSLWord:
    """A lazily built DSL word such as ``r * s * R(pi/4)^-1``; rendered to text only on demand."""

    __slots__ = ("_parts",)

    def __init__(self, parts: Iterable[Tuple[str, Optional[str], int]]):
        self._parts = tuple(parts)

    def _concat(self, other):
        if isinstance(other, GenRef):
            other = DSLWord([(other.name, None, 1)])
        if not isinstance(other, DSLWord):
            return NotImplemented
        return DSLWord(self._parts + other._parts)

    __mul__ = _concat
    __matmul__ = _concat
    __add__ = _concat

    def __pow__(self, n: int) -> "DSLWord":
        if n < 0:
            raise ValueError("negative powers apply to a single generator: use g ** -1")
        return DSLWord(self._parts * n)

    def __str__(self) -> str:
        out = []
        for name, arg, exp in self._parts:
            out.append(name + (f"({arg})" if arg is not None else "") + (f"^{exp}" if exp != 1 else ""))
        return " * ".join(out) if out else "e"

    __repr__ = __str__


class GenRef:
    """Handle to a generator declared in a :class:`DSLGroup`. Combines into :class:`DSLWord`."""

    __slots__ = ("name", "family")

    def __init__(self, name: str, family: bool = False):
        self.name, self.family = name, family

    def __call__(self, angle) -> DSLWord:
        if not self.family:
            raise TypeError(f"generator {self.name!r} takes no argument")
        return DSLWord([(self.name, _expr(angle), 1)])

    def _word(self) -> DSLWord:
        if self.family:
            raise TypeError(f"{self.name!r} is a parametrised family: write {self.name}(angle)")
        return DSLWord([(self.name, None, 1)])

    def __mul__(self, other):
        return self._word()._concat(other)

    __matmul__ = __mul__
    __add__ = __mul__

    def __pow__(self, n: int) -> DSLWord:
        if not isinstance(n, int) or isinstance(n, bool):
            raise TypeError("exponent must be an int")
        return DSLWord([(self.name, None, n)])

    def __str__(self) -> str:
        return self.name

    __repr__ = __str__


def _word_text(w) -> str:
    if isinstance(w, (DSLWord, GenRef)):
        return str(w._word() if isinstance(w, GenRef) else w)
    if isinstance(w, str):
        return str(DSLWord([(_ident(w, "generator name") if w != "e" else "e", None, 1)]))
    raise TypeError("expected a DSLWord, GenRef or generator name")


def _name_of(x, what: str) -> str:
    return _ident(x.name if isinstance(x, GenRef) else x, what) if not (isinstance(x, str) and x == "e") else "e"


class _FlushOnce:
    """Mixin: a builder hands its text to the C++ compiler exactly once."""

    _flushed = False
    _run_result = None

    def _begin_flush(self) -> None:
        if self._flushed:
            raise RuntimeError("already flushed to the C++ compiler: a DSL builder flushes exactly once; build a new one")
        self._flushed = True
        _STATS["dsl_flushes"] += 1

    def _end_flush_failed(self) -> None:
        # nothing was allocated on the C++ side when compilation raised, so the builder may be retried
        self._flushed = False
        _STATS["dsl_flushes"] -= 1

    def source(self) -> str:  # pragma: no cover - overridden
        raise NotImplementedError

    def run(self, *, transcript: bool = False, sharpness: float = 10.0):
        """Compile and run every query; returns a small ``dsl.RunResult``. Flushes once; the result is cached."""
        if self._run_result is not None:
            return self._run_result
        self._begin_flush()
        try:
            self._run_result = _core.dsl.Interpreter(sharpness).run(self.source(), transcript)
        except BaseException:
            self._end_flush_failed()
            raise
        return self._run_result

    def compile(self, *, sharpness: float = 10.0):
        """Compile without running; returns the C++ ``dsl.Program``. The CALLER owns it: the builder keeps no reference."""
        self._begin_flush()
        try:
            return _core.dsl.Interpreter(sharpness).compile(self.source())
        except BaseException:
            self._end_flush_failed()
            raise


class DSLGroup(_FlushOnce):
    """Builds one ``group Name { ... }`` block as text. Every method appends a line and returns what you need next."""

    def __init__(self, name: str):
        self.name = _ident(name, "group name")
        self._lines: List[str] = []
        self._declared: Dict[str, str] = {}

    def _add(self, line: str) -> None:
        if self._flushed:
            raise RuntimeError("this group was already flushed to the C++ compiler; build a new DSLGroup")
        self._lines.append(line)

    def _declare(self, name: str, kind: str) -> str:
        _ident(name, f"{kind} name")
        if name in self._declared:
            raise ValueError(f"{name!r} is already declared as a {self._declared[name]}")
        self._declared[name] = kind
        return name

    # -- declarations ---------------------------------------------------------------------------
    def generator(self, name: str, *, matrix=None, rotation=None, reflection=None, phasor=None) -> GenRef:
        """``gen s = matrix([[...]])`` | ``rotation(deg)`` | ``reflection(deg)`` | ``phasor(radians)`` (give exactly one)."""
        given = [k for k, v in (("matrix", matrix), ("rotation", rotation), ("reflection", reflection), ("phasor", phasor)) if v is not None]
        if len(given) != 1:
            raise ValueError("give exactly one of matrix=, rotation=, reflection=, phasor=")
        self._declare(name, "generator")
        if matrix is not None:
            rows = [list(r) for r in matrix]
            if not rows or any(len(r) != len(rows) for r in rows):
                raise ValueError("matrix must be square")
            body = "matrix([" + ", ".join(_vec(r) for r in rows) + "])"
        else:
            body = f"{given[0]}({_expr(rotation if rotation is not None else reflection if reflection is not None else phasor)})"
        self._add(f"gen {name} = {body}")
        return GenRef(name)

    def param_generator(self, name: str, param: str = "theta", kind: str = "phasor", body=None) -> GenRef:
        """A continuous family: ``gen R(theta) = phasor(theta)``; use it as ``R('pi/4')``."""
        if kind not in ("phasor", "rotation", "reflection"):
            raise ValueError("kind must be phasor, rotation or reflection")
        self._declare(name, "generator")
        _ident(param, "parameter name")
        self._add(f"gen {name}({param}) = {kind}({_expr(param if body is None else body)})")
        return GenRef(name, family=True)

    def lift(self, name: str, expr) -> None:
        self._declare(name, "lift")
        self._add(f"lift {name} = {_expr(expr)}")

    def subgroup(self, name: str, generators: Sequence) -> str:
        self._declare(name, "subgroup")
        self._add(f"subgroup {name} = span({', '.join(_name_of(g, 'generator name') for g in generators)})")
        return name

    def separate(self, name: str, weight: Sequence, bias) -> str:
        self._declare(name, "classifier")
        self._add(f"separate {name} {{ weight = {_vec(weight)}, bias = {_expr(bias)} }}")
        return name

    def sector(self, name: str, lo, hi) -> str:
        self._declare(name, "classifier")
        self._add(f"sector {name} = [{_expr(lo)}, {_expr(hi)}]")
        return name

    def space(self, name: str, points: Iterable[Tuple[Sequence, int]]) -> str:
        """``space P { (0, 0) class 0 ... }`` from (coordinates, label) pairs."""
        self._declare(name, "space")
        rows = [f"    {_point(c)} class {int(bool(lbl))}" for c, lbl in points]
        if not rows:
            raise ValueError("a space needs at least one point")
        self._add(f"space {name} {{\n" + "\n".join(rows) + "\n}")
        return name

    def synthesize(self, name: str, *, on: Optional[str] = None, subgroup: Optional[str] = None,
                   over: Optional[str] = None, coset=None) -> str:
        """Ask the C++ kernel synthesizer for a classifier: on a point set, or a subgroup (or coset) of ``over``."""
        if (on is None) == (subgroup is None):
            raise ValueError("give either on=<space> or subgroup=<name> with over=<name>")
        self._declare(name, "classifier")
        if on is not None:
            self._add(f"synthesize {name} on {_ident(on, 'space name')}")
        else:
            if over is None:
                raise ValueError("subgroup= needs over=")
            tail = f" coset {_word_text(coset)}" if coset is not None else ""
            self._add(f"synthesize {name} of {_ident(subgroup, 'subgroup name')} over {_ident(over, 'subgroup name')}{tail}")
        return name

    # -- queries --------------------------------------------------------------------------------------
    def eval(self, word, point: Optional[Sequence] = None) -> None:
        self._add(f"eval {_word_text(word)}" + (f" on {_point(point)}" if point is not None else ""))

    def classify(self, classifier: str, *, word=None, point: Optional[Sequence] = None, expect: Optional[int] = None) -> None:
        if word is None and point is None:
            raise ValueError("classify needs word= and/or point=")
        text = f"classify {_ident(classifier, 'classifier name')}"
        if word is not None:
            text += f" of {_word_text(word)}"
            if point is not None:
                text += f" on {_point(point)}"
        else:
            text += f" on {_point(point)}"
        if expect is not None:
            text += f" expect {int(bool(expect))}"
        self._add(text)

    def verify(self, classifier: str, subgroup: str, over: str) -> None:
        self._add(f"verify {_ident(classifier, 'classifier name')} = {_ident(subgroup, 'subgroup name')} over {_ident(over, 'subgroup name')}")

    def check(self, lhs, rhs) -> None:
        self._add(f"check {_word_text(lhs)} == {_word_text(rhs)}")

    def sweep(self, classifier: str, family: GenRef, start, stop, steps: int) -> None:
        if not isinstance(steps, int) or steps < 1:
            raise ValueError("steps must be a positive int")
        self._add(f"sweep {_ident(classifier, 'classifier name')} over {family.name} from {_expr(start)} to {_expr(stop)} steps {steps}")

    # -- output ----------------------------------------------------------------------------------------
    def source(self) -> str:
        body = "\n".join("    " + physical for line in self._lines for physical in line.split("\n"))
        return f"group {self.name} {{\n{body}\n}}\n"

    __str__ = source


class DSLProgram(_FlushOnce):
    """Several groups flushed together, still exactly once."""

    def __init__(self, *groups: DSLGroup):
        if not groups:
            raise ValueError("a program needs at least one group")
        self.groups = groups

    def source(self) -> str:
        return "\n".join(g.source() for g in self.groups)

    def _begin_flush(self) -> None:
        super()._begin_flush()
        for g in self.groups:
            g._flushed = True  # the constituent groups can no longer be edited or flushed on their own


# --------------------------------------------------------------------------------------------
# high-level helpers: build text -> one flush -> plain Python result -> C++ snapshot dropped
# --------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class SynthesisResult:
    """What the C++ kernel synthesizer found. A plain Python object: it holds no reference to the C++ Program."""
    name: str
    lifts: List[str]            # e.g. ['k1 = abs(x - y)']
    weight: List[float]         # hyperplane over [original features, lifts...]
    bias: float
    log: List[str] = field(default_factory=list)   # the compiler's [synth] lines
    n_points: int = 0
    separation: float = 0.0     # fraction of labelled points the folded runtime classifies correctly

    def __str__(self) -> str:
        lifts = ", ".join(self.lifts) if self.lifts else "(none: linearly separable)"
        return (f"{self.name}: lifts {lifts}; w = {self.weight}, b = {self.bias}; "
                f"separation {self.separation:.0%} ({self.n_points} points)")


def _extract_synthesis(program, name: str) -> SynthesisResult:
    group = program.groups[0]
    clf = next(c for c in group.classifiers if c.name == name)
    verify = next(t for t in group.tasks if t.kind == "verify")
    good = sum(1 for j in verify.jobs if j.decide() == j.expected)
    n = len(verify.jobs)
    return SynthesisResult(
        name=name, lifts=list(clf.lifts), weight=list(clf.weight), bias=float(clf.bias),
        log=[d[len("[synth] "):] for d in group.declarations if d.startswith("[synth] ")],
        n_points=n, separation=(good / n if n else 1.0))


def synthesize_space(points: Iterable[Tuple[Sequence[Number], int]], *, name: str = "Auto", sharpness: float = 10.0) -> SynthesisResult:
    """Discover the minimal non-linear lift that separates labelled points (e.g. XOR -> ``abs(x - y)``)."""
    g = DSLGroup("SynthSpace")
    g.space("Points", points)
    g.synthesize(name, on="Points")
    program = g.compile(sharpness=sharpness)       # the ONE flush
    try:
        return _extract_synthesis(program, name)
    finally:
        del program                                # release the C++ snapshot immediately


def synthesize_subgroup(generators: Mapping[str, Sequence[Sequence[Number]]], subgroup: Sequence[str], over: Sequence[str],
                        *, coset: Optional[Sequence[str]] = None, name: str = "Auto", sharpness: float = 10.0) -> SynthesisResult:
    """Synthesize a classifier for ``span(subgroup)`` (or the coset ``coset * span(subgroup)``) inside ``span(over)``."""
    g = DSLGroup("SynthSubgroup")
    refs = {n: g.generator(n, matrix=m) for n, m in generators.items()}
    g.subgroup("Sub", [refs[n] for n in subgroup])
    g.subgroup("Amb", [refs[n] for n in over])
    word = None
    if coset:
        word = DSLWord([(refs[c].name, None, 1) for c in coset])
    g.synthesize(name, subgroup="Sub", over="Amb", coset=word)
    program = g.compile(sharpness=sharpness)
    try:
        return _extract_synthesis(program, name)
    finally:
        del program


@dataclass(frozen=True)
class CheckOutcome:
    passed: bool
    exact: Optional[bool]   # True: proved equal symbolically; False: not provable; None: no symbolic proof applies
    lhs_angle: str
    rhs_angle: str
    max_abs_diff: float


def phasor_check(lhs_angles: Sequence, rhs_angles: Sequence) -> CheckOutcome:
    """Check ``R(a1) * R(a2) * ... == R(b1) * ...`` for SO(2) phasors (angles in radians, e.g. ``'pi/4'``).

    Equal angle sums mod 2*pi are PROVED symbolically (exact, no tolerance); otherwise the numeric matrices decide.
    """
    g = DSLGroup("PhasorCheck")
    R = g.param_generator("R", "theta", "phasor")

    def side(angles):
        word = DSLWord([])
        for a in angles:
            word = word * R(a)
        return word

    g.check(side(lhs_angles), side(rhs_angles))
    program = g.compile()
    try:
        task = next(t for t in program.groups[0].tasks if t.kind == "check")
        a = task.jobs[0].evaluate().final_vector
        b = task.jobs[1].evaluate().final_vector
        diff = max((abs(x - y) for x, y in zip(a, b)), default=0.0)
        allclose = all(abs(x - y) <= 1e-9 + 1e-5 * abs(y) for x, y in zip(a, b))
        return CheckOutcome(passed=bool(task.exact) or allclose, exact=task.exact,
                            lhs_angle=task.lhs_angle, rhs_angle=task.rhs_angle, max_abs_diff=diff)
    finally:
        del program
