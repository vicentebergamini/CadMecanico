# Testes sem Qt: as DLLs do OCCT são copiadas ao lado dos executáveis pelo applocal do vcpkg.

add_executable(test_geometria test_geometria.cpp TestCheck.h ExpectedTestPart.h)
target_link_libraries(test_geometria PRIVATE cad_modeling)
add_test(NAME geometria COMMAND test_geometria)

add_executable(test_step_roundtrip test_step_roundtrip.cpp TestCheck.h ExpectedTestPart.h)
target_link_libraries(test_step_roundtrip PRIVATE cad_modeling cad_io)
add_test(NAME step_roundtrip COMMAND test_step_roundtrip ${CMAKE_CURRENT_BINARY_DIR}/saida)

add_executable(test_selecao_info test_selecao_info.cpp TestCheck.h)
target_link_libraries(test_selecao_info PRIVATE cad_modeling)
add_test(NAME selecao_info COMMAND test_selecao_info)

add_executable(test_documento test_documento.cpp TestCheck.h ExpectedTestPart.h)
target_link_libraries(test_documento PRIVATE cad_document)
add_test(NAME documento COMMAND test_documento)

add_executable(test_esboco test_esboco.cpp TestCheck.h ExpectedTestPart.h)
target_link_libraries(test_esboco PRIVATE cad_document cad_sketch)
add_test(NAME esboco COMMAND test_esboco)

add_executable(test_extrusao test_extrusao.cpp TestCheck.h)
target_link_libraries(test_extrusao PRIVATE cad_document cad_io)
add_test(NAME extrusao COMMAND test_extrusao)

add_executable(test_stl test_stl.cpp TestCheck.h StlTestUtils.h)
target_link_libraries(test_stl PRIVATE cad_document cad_io TKDESTL)
add_test(NAME stl COMMAND test_stl)

add_executable(test_booleanas test_booleanas.cpp TestCheck.h)
target_link_libraries(test_booleanas PRIVATE cad_document cad_io)
add_test(NAME booleanas COMMAND test_booleanas)

add_executable(test_arcos_cantos2d test_arcos_cantos2d.cpp TestCheck.h)
target_link_libraries(test_arcos_cantos2d PRIVATE cad_document)
add_test(NAME arcos_cantos2d COMMAND test_arcos_cantos2d)

add_executable(test_acabamentos3d test_acabamentos3d.cpp TestCheck.h)
target_link_libraries(test_acabamentos3d PRIVATE cad_document cad_io)
add_test(NAME acabamentos3d COMMAND test_acabamentos3d)

add_executable(test_e56 test_e56.cpp TestCheck.h)
target_link_libraries(test_e56 PRIVATE cad_document cad_io)
add_test(NAME e56 COMMAND test_e56)

add_executable(test_e57 test_e57.cpp TestCheck.h)
target_link_libraries(test_e57 PRIVATE cad_document cad_document_io cad_io)
add_test(NAME e57 COMMAND test_e57)
add_executable(test_e571 test_e571.cpp TestCheck.h)
target_link_libraries(test_e571 PRIVATE cad_document cad_document_io cad_io)
add_test(NAME e571 COMMAND test_e571)
add_executable(test_e572 test_e572.cpp TestCheck.h)
target_link_libraries(test_e572 PRIVATE cad_discretization cad_document TKBO psapi)
add_test(NAME e572 COMMAND test_e572)

# E5.7.2.2: exportação ANSYS APDL (/PREP7); compara com o arquivo de referência do usuário (somente leitura).
add_executable(test_e5722 test_e5722.cpp TestCheck.h)
target_link_libraries(test_e5722 PRIVATE cad_discretization TKBO)
target_compile_definitions(test_e5722 PRIVATE CAD_APDL_REFERENCE="${PROJECT_SOURCE_DIR}/../DEM_DYNA.txt")
add_test(NAME e5722 COMMAND test_e5722)

# E5.7.2.3: projeção dos nós sobre o contorno (ajuste geométrico) e exportações com as novas coordenadas.
add_executable(test_e5723 test_e5723.cpp TestCheck.h)
target_link_libraries(test_e5723 PRIVATE cad_discretization TKBO)
add_test(NAME e5723 COMMAND test_e5723)
# QtCore (JSON do .cadmec) no PATH em tempo de execução.
set_tests_properties(e57 e571 PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")

set_tests_properties(geometria step_roundtrip selecao_info documento esboco extrusao stl booleanas arcos_cantos2d acabamentos3d e56 e57 e571 e572 e5722 e5723 PROPERTIES LABELS "core")

# Formato .cadmec (QtCore, sem interface gráfica).
qt_add_executable(test_arquivo_cadmec test_arquivo_cadmec.cpp)
set_target_properties(test_arquivo_cadmec PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_arquivo_cadmec PRIVATE cad_document_io cad_modeling Qt6::Test)
target_compile_definitions(test_arquivo_cadmec PRIVATE CADMEC_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/dados")
add_test(NAME arquivo_cadmec COMMAND test_arquivo_cadmec)
set_tests_properties(arquivo_cadmec PROPERTIES
    LABELS "core"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
)

# Teste de interface (Qt Test). As DLLs e plugins do Qt são localizados pelo ambiente
# do teste, sem copiar arquivos; a plataforma offscreen dispensa sessão interativa.
qt_add_executable(test_mainwindow test_mainwindow.cpp)
set_target_properties(test_mainwindow PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_mainwindow PRIVATE cad_app cad_io cad_modeling Qt6::Test)
add_test(NAME mainwindow COMMAND test_mainwindow)

qt_add_executable(test_documento_ui test_documento_ui.cpp)
set_target_properties(test_documento_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_documento_ui PRIVATE cad_app cad_document_io cad_io cad_modeling Qt6::Test)
add_test(NAME documento_ui COMMAND test_documento_ui)

set_tests_properties(mainwindow documento_ui PROPERTIES
    LABELS "gui"
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_PLUGIN_PATH=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
)

# Testes do visualizador 3D: exigem contexto OpenGL real (plataforma windows, sessão
# gráfica interativa). "viewer" usa a escala do Windows; "viewer_escala" repete os testes
# multiplicando-a por 1,25 (125% numa tela a 100%).
qt_add_executable(test_viewer test_viewer.cpp ViewerTestUtils.h)
set_target_properties(test_viewer PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_viewer PRIVATE cad_viewer cad_modeling Qt6::Test)

qt_add_executable(test_app_viewer test_app_viewer.cpp ViewerTestUtils.h)
set_target_properties(test_app_viewer PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_app_viewer PRIVATE cad_app cad_viewer cad_io cad_modeling Qt6::Test)

qt_add_executable(test_interacao test_interacao.cpp ViewerTestUtils.h)
set_target_properties(test_interacao PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_interacao PRIVATE cad_viewer cad_modeling Qt6::Test)

qt_add_executable(test_esboco_ui test_esboco_ui.cpp ViewerTestUtils.h)
set_target_properties(test_esboco_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_esboco_ui PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_documento_vazio test_documento_vazio.cpp ViewerTestUtils.h)
set_target_properties(test_documento_vazio PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_documento_vazio PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)
target_compile_definitions(test_documento_vazio PRIVATE CADMEC_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/dados")

qt_add_executable(test_extrusao_ui test_extrusao_ui.cpp ViewerTestUtils.h StlTestUtils.h)
set_target_properties(test_extrusao_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_extrusao_ui PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_booleanas_ui test_booleanas_ui.cpp ViewerTestUtils.h StlTestUtils.h)
set_target_properties(test_booleanas_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_booleanas_ui PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_e55_ui test_e55_ui.cpp ViewerTestUtils.h StlTestUtils.h)
set_target_properties(test_e55_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_e55_ui PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_e56_ui test_e56_ui.cpp ViewerTestUtils.h StlTestUtils.h)
set_target_properties(test_e56_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_e56_ui PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_e57_ui test_e57_ui.cpp ViewerTestUtils.h)
set_target_properties(test_e57_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_e57_ui PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_e571_ui test_e571_ui.cpp ViewerTestUtils.h)
set_target_properties(test_e571_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_e571_ui PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_e572_ui test_e572_ui.cpp ViewerTestUtils.h)
set_target_properties(test_e572_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_e572_ui PRIVATE cad_app cad_viewer cad_discretization cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_e5722_ui test_e5722_ui.cpp ViewerTestUtils.h)
set_target_properties(test_e5722_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_e5722_ui PRIVATE cad_app cad_viewer cad_discretization cad_document_io cad_io cad_modeling Qt6::Test)
target_compile_definitions(test_e5722_ui PRIVATE CAD_APDL_REFERENCE="${PROJECT_SOURCE_DIR}/../DEM_DYNA.txt")

qt_add_executable(test_e5723_ui test_e5723_ui.cpp ViewerTestUtils.h)
set_target_properties(test_e5723_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_e5723_ui PRIVATE cad_app cad_viewer cad_discretization cad_document_io cad_io cad_modeling Qt6::Test)

qt_add_executable(test_e561_ui test_e561_ui.cpp ViewerTestUtils.h)
set_target_properties(test_e561_ui PROPERTIES AUTOMOC ON WIN32_EXECUTABLE OFF)
target_link_libraries(test_e561_ui PRIVATE cad_app cad_viewer cad_document_io cad_io cad_modeling Qt6::Test)

add_test(NAME viewer COMMAND test_viewer)
add_test(NAME viewer_escala COMMAND test_viewer)
add_test(NAME app_viewer COMMAND test_app_viewer)
add_test(NAME interacao COMMAND test_interacao)
add_test(NAME interacao_escala COMMAND test_interacao)
add_test(NAME esboco_ui COMMAND test_esboco_ui)
add_test(NAME esboco_ui_escala COMMAND test_esboco_ui)
add_test(NAME documento_vazio COMMAND test_documento_vazio)
add_test(NAME extrusao_ui COMMAND test_extrusao_ui)
add_test(NAME booleanas_ui COMMAND test_booleanas_ui)
add_test(NAME e55_ui COMMAND test_e55_ui)
add_test(NAME e56_ui COMMAND test_e56_ui)
add_test(NAME e561_ui COMMAND test_e561_ui)
add_test(NAME e57_ui COMMAND test_e57_ui)
add_test(NAME e571_ui COMMAND test_e571_ui)
add_test(NAME e572_ui COMMAND test_e572_ui)
add_test(NAME e5722_ui COMMAND test_e5722_ui)
add_test(NAME e5723_ui COMMAND test_e5723_ui)
add_test(NAME e561_ui_escala COMMAND test_e561_ui)

set(_cad_gl_env "QT_QPA_PLATFORM=windows;QT_PLUGIN_PATH=${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}")
set_tests_properties(viewer app_viewer interacao esboco_ui documento_vazio extrusao_ui booleanas_ui e55_ui e56_ui e561_ui e57_ui e571_ui e572_ui e5722_ui e5723_ui PROPERTIES
    LABELS "gui;opengl"
    TIMEOUT 300
    ENVIRONMENT "${_cad_gl_env}"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
)
set_tests_properties(viewer_escala interacao_escala esboco_ui_escala e561_ui_escala PROPERTIES
    LABELS "gui;opengl"
    TIMEOUT 300
    ENVIRONMENT "${_cad_gl_env};QT_SCALE_FACTOR=1.25"
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
)
