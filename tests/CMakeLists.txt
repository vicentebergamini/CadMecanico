# Testes sem Qt: as DLLs do OCCT são copiadas ao lado dos executáveis pelo applocal do vcpkg.

add_executable(test_geometria test_geometria.cpp TestCheck.h ExpectedTestPart.h)
target_link_libraries(test_geometria PRIVATE cad_modeling)
add_test(NAME geometria COMMAND test_geometria)

add_executable(test_step_roundtrip test_step_roundtrip.cpp TestCheck.h ExpectedTestPart.h)
target_link_libraries(test_step_roundtrip PRIVATE cad_modeling cad_io)
add_test(NAME step_roundtrip COMMAND test_step_roundtrip ${CMAKE_CURRENT_BINARY_DIR}/saida)

set_tests_properties(geometria step_roundtrip PROPERTIES LABELS "core")
