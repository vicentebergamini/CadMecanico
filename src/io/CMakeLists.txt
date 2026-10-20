add_library(cad_io STATIC
    StepExporter.h StepExporter.cpp
    StepImporter.h StepImporter.cpp
    StlExporter.h StlExporter.cpp
)
target_include_directories(cad_io PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(cad_io PUBLIC
    TKernel TKMath TKBRep TKTopAlgo TKMesh TKDESTEP TKDESTL TKXSBase
)
