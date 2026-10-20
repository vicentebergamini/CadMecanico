# Testes sem Qt: as DLLs do OCCT são copiadas ao lado dos executáveis pelo applocal do vcpkg.

add_executable(test_geometria test_geometria.cpp TestCheck.h ExpectedTestPart.h)
target_link_libraries(test_geometria PRIVATE cad_modeling)
add_test(NAME geometria COMMAND test_geometria)

add_executable(test_step_roundtrip test_step_roundtrip.cpp TestCheck.h ExpectedTestPart.h)
target_link_libraries(test_step_roundtrip PRIVATE cad_modeling cad_io)
add_test(NAME step_roundtrip COMMAND test_step_roundtrip ${CMAKE_CURRENT_BINARY_DIR}/saida)

set_tests_properties(geometria step_roundtrip PROPERTIES LABELS "core")

# Teste de interface (Qt Test). As DLLs e plugins do Qt são localizados pelo ambiente
# do teste, sem copiar arquivos; a plataforma offscreen dispensa sessão interativa.
qt_add_executable(test_mainwindow test_mainwindow.cpp)
set_target_properties(test_mainwindow PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_mainwindow PRIVATE cad_app cad_io cad_modeling Qt6::Test)
add_test(NAME mainwindow COMMAND test_mainwindow)

set_tests_properties(mainwindow PROPERTIES
    LABELS "gui"
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
)
