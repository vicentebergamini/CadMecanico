# Núcleo do documento paramétrico (sem Qt).
add_library(cad_document STATIC
    CadDocument.h CadDocument.cpp
)
target_include_directories(cad_document PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_document PUBLIC cad_modeling)

# Persistência .cadmec (JSON via QtCore).
add_library(cad_document_io STATIC
    DocumentFile.h DocumentFile.cpp
)
target_include_directories(cad_document_io PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_document_io PUBLIC cad_document Qt6::Core)
