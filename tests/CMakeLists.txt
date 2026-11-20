# Test registration: C++ unit tests + benchmark, finite-field suites, facade tests,
# a single-header sanity build, and the Python bindings tests.

add_executable(unit_tests cpp/test_engine.cpp)
target_link_libraries(unit_tests PRIVATE SimmyNet::native_cpp)
add_test(NAME simmynet_unit_tests COMMAND unit_tests --quick)

# Exact finite-field passes (brute-force oracles and engine cross-checks).
add_executable(finite_field_test cpp/finite_field_test.cpp)
target_link_libraries(finite_field_test PRIVATE SimmyNet::native_cpp)
add_test(NAME simmynet_finite_field COMMAND finite_field_test)

add_executable(engine_integration_test cpp/engine_integration_test.cpp)
target_link_libraries(engine_integration_test PRIVATE SimmyNet::native_cpp)
add_test(NAME simmynet_engine_integration COMMAND engine_integration_test)

# The binding facade (simmynet/Api.hpp): everything the Python API does, tested natively.
add_executable(test_api cpp/test_api.cpp)
target_link_libraries(test_api PRIVATE SimmyNet::native_cpp)
add_test(NAME simmynet_api COMMAND test_api)

# Exact algebraic tuples ({c_base + c_pi*pi}) in the frontend: parser, ExprProgram, symbolic check, lowering.
add_executable(test_algebraic_tuple cpp/test_algebraic_tuple.cpp)
target_link_libraries(test_algebraic_tuple PRIVATE SimmyNet::native_cpp)
add_test(NAME simmynet_algebraic_tuple COMMAND test_algebraic_tuple)

# The amalgamated single file must keep building and passing (guards against a stale copy).
add_executable(single_header_check ${PROJECT_SOURCE_DIR}/single_header/simmynet_single.cpp)
target_link_libraries(single_header_check PRIVATE simmynet_native_flags)
target_compile_features(single_header_check PRIVATE cxx_std_17)
add_test(NAME simmynet_single_header COMMAND single_header_check --quick)

# Python package (simmynet/{__init__,wrapper}.py + the compiled _core extension), only when built.
if(TARGET simmynet_core)
  foreach(_py_test test_bindings test_wrapper)
    add_test(NAME simmynet_python_${_py_test}
             COMMAND "${Python_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/python/${_py_test}.py")
    set_tests_properties(simmynet_python_${_py_test} PROPERTIES
        ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}/python")
  endforeach()
endif()
