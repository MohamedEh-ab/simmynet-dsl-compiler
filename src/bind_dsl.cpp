// bind_dsl.cpp - Python bindings for the SimmyNet DSL compiler and runtime.
//
//   simmynet.dsl.Interpreter(sharpness=10.0)   .run(source, transcript=False) -> RunResult
//                                                   .compile(source)               -> Program
//   Program.groups -> Group -> .tasks -> Task -> .jobs -> Job -> .evaluate() -> ForwardResult
//   simmynet.dsl.tokenize(source)              -> [(kind, text, line), ...]
//   Exceptions (top level): SimmyNetError > LexError, ParseError, CompileError, SimmyNetRuntimeError
//
// All logic lives in simmynet/Api.hpp; this file only registers it.
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <simmynet/Api.hpp>

#include "bindings.hpp"

#include <string>

namespace py = pybind11;
namespace api = simmynet::api;

void bind_dsl(py::module_& root) {
    // ---- exception hierarchy (registered base-first so derived translators win) -----------
    auto& base = py::register_exception<simmynet::SimmyNetError>(root, "SimmyNetError");
    py::register_exception<simmynet::LexError>(root, "LexError", base);
    py::register_exception<simmynet::ParseError>(root, "ParseError", base);
    py::register_exception<simmynet::CompileError>(root, "CompileError", base);
    py::register_exception<simmynet::SimmyNetRuntimeError>(root, "SimmyNetRuntimeError", base);

    py::module_ m = make_submodule(root, "dsl", "SimmyNet DSL: lexer, parser, compiler (constant folding, kernel synthesis) and runtime.");

    // ---- results ------------------------------------------------------------------------
    py::class_<simmynet::ForwardResult>(m, "ForwardResult",
                                      "Result of running a job. `decision`, `score` and `soft_score` are None "
                                      "for jobs without a hyperplane (eval).")
        .def_readonly("input_vector", &simmynet::ForwardResult::inputVector)
        .def_readonly("final_vector", &simmynet::ForwardResult::finalVector,
                      "State fed into the first decision layer (the group element for eval jobs).")
        .def_readonly("decision", &simmynet::ForwardResult::decision)
        .def_readonly("score", &simmynet::ForwardResult::score)
        .def_readonly("soft_score", &simmynet::ForwardResult::softScore)
        .def_readonly("trace", &simmynet::ForwardResult::trace, "List of (layer label, vector).");

    py::class_<api::RunResult>(m, "RunResult", "Outcome of Interpreter.run.")
        .def_readonly("passed", &api::RunResult::passed)
        .def_readonly("failed", &api::RunResult::failed)
        .def_readonly("x_checked", &api::RunResult::xChecked, "Folded-vs-reference equivalence checks performed.")
        .def_readonly("x_failed", &api::RunResult::xFailed, "Folded-vs-reference mismatches (should be 0).")
        .def_readonly("transcript", &api::RunResult::transcript)
        .def_property_readonly("ok", &api::RunResult::ok)
        .def("__repr__", [](const api::RunResult& r) {
            return "RunResult(passed=" + std::to_string(r.passed) + ", failed=" + std::to_string(r.failed) +
                   ", x_checked=" + std::to_string(r.xChecked) + ", x_failed=" + std::to_string(r.xFailed) + ")";
        });

    // ---- compiled program ---------------------------------------------------------------
    py::class_<api::JobHandle>(m, "Job", "One compiled query (eval or classify) with its constant-folded graph.")
        .def_property_readonly("kind", &api::JobHandle::kind, "'eval' or 'classify'.")
        .def_property_readonly("label", &api::JobHandle::label)
        .def_property_readonly("short_label", &api::JobHandle::shortLabel)
        .def_property_readonly("expected", &api::JobHandle::expected, "Expected decision, or None.")
        .def_property_readonly("input", &api::JobHandle::input, "Input vector (vec(I) in element mode).")
        .def_property_readonly("element_mode", &api::JobHandle::elementMode)
        .def_property_readonly("dim", &api::JobHandle::dim, "Generator dimension k.")
        .def_property_readonly("matrix_shape", &api::JobHandle::matrixShape)
        .def_property_readonly("architecture", &api::JobHandle::architecture, "Unfolded IR, one Linear per factor.")
        .def_property_readonly("folded_architecture", &api::JobHandle::foldedArchitecture)
        .def_property_readonly("folded_factors", &api::JobHandle::foldedFactors,
                               "Number of generator factors collapsed into W_compiled.")
        .def_property_readonly("folded_matrix", &api::JobHandle::foldedMatrix, "W_compiled, row-major.")
        .def("evaluate", &api::JobHandle::evaluate, py::arg("trace") = false, py::arg("reference") = false,
             "Run the job. Folded graph by default (one k x k pass); reference=True runs the unfolded "
             "Kronecker-expanded oracle.")
        .def("decide", &api::JobHandle::decide, "Decision on the folded graph; SimmyNetRuntimeError if the job has no hyperplane.")
        .def("__repr__", [](const api::JobHandle& j) { return "Job(" + j.label() + ")"; });

    py::class_<api::TaskHandle>(m, "Task", "A statement-level task: 'job', 'verify' (batch) or 'check' (lhs, rhs).")
        .def_readonly("kind", &api::TaskHandle::kind)
        .def_readonly("label", &api::TaskHandle::label)
        .def_readonly("jobs", &api::TaskHandle::jobs)
        .def_readonly("exact", &api::TaskHandle::exactEqual,
                      "check tasks: True = phasor angle sums proved equal symbolically (exact, mod 2*pi), False = "
                      "not provable (numeric comparison decides), None = no symbolic proof applies.")
        .def_readonly("lhs_angle", &api::TaskHandle::lhsAngle, "Summed phasor angle of the left side, e.g. '0.5*pi'.")
        .def_readonly("rhs_angle", &api::TaskHandle::rhsAngle)
        .def("__repr__", [](const api::TaskHandle& t) { return "Task(" + t.kind + ": " + t.label + ")"; });

    py::class_<api::ClassifierInfo>(m, "Classifier", "A declared or synthesized classifier.")
        .def_readonly("name", &api::ClassifierInfo::name)
        .def_readonly("weight", &api::ClassifierInfo::weight)
        .def_readonly("bias", &api::ClassifierInfo::bias)
        .def_readonly("lifts", &api::ClassifierInfo::lifts, "e.g. ['k1 = abs(x - y)'].")
        .def_readonly("is_sector", &api::ClassifierInfo::isSector)
        .def("__repr__", [](const api::ClassifierInfo& c) { return "Classifier(" + c.name + ")"; });

    py::class_<api::GroupHandle>(m, "Group", "A compiled `group { ... }` block.")
        .def_readonly("name", &api::GroupHandle::name)
        .def_readonly("declarations", &api::GroupHandle::declarations, "Compiler log, including [synth] lines.")
        .def_readonly("classifiers", &api::GroupHandle::classifiers)
        .def_readonly("tasks", &api::GroupHandle::tasks)
        .def("__repr__", [](const api::GroupHandle& g) { return "Group(" + g.name + ")"; });

    py::class_<api::Program>(m, "Program", "Compiled DSL program.")
        .def_readonly("groups", &api::Program::groups);

    // ---- entry points -------------------------------------------------------------------
    py::class_<api::Interpreter>(m, "Interpreter", "Lex, parse, compile, fold and run SimmyNet DSL programs.")
        .def(py::init<double>(), py::arg("sharpness") = 10.0, "sharpness: slope of the soft sigmoid score.")
        .def_property_readonly("sharpness", &api::Interpreter::sharpness)
        .def("run", &api::Interpreter::run, py::arg("source"), py::arg("transcript") = false,
             "Compile and run every query; transcript=True captures the full Python-style transcript.")
        .def("compile", &api::Interpreter::compile, py::arg("source"),
             "Compile without running; returns a Program whose jobs can be evaluated individually.");

    m.def("tokenize", &api::tokenizeSource, py::arg("source"),
          "Token stream as (kind, text, line); kind is 'number', 'ident', 'symbol' or 'eof'.");
}
