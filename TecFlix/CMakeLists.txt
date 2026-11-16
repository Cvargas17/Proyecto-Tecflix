cmake_minimum_required(VERSION 3.16)
project(TecFlix VERSION 1.0.0 LANGUAGES CXX)

# Estándar C++17 obligatorio
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Opciones de compilación
option(ENABLE_COVERAGE "Habilita instrumentación de cobertura de código (--coverage)" OFF)
option(TECFLIX_USE_MONGODB "Enlaza el driver oficial mongocxx/bsoncxx de MongoDB" OFF)
option(BUILD_TESTING "Compila y registra las pruebas unitarias en CTest" ON)

# Instrumentación de cobertura para GCC/Clang
if(ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(STATUS "Cobertura de código habilitada (--coverage)")
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# Dependencias de red e hilos multiplataforma
find_package(Threads REQUIRED)
set(TECFLIX_PLATFORM_LIBS Threads::Threads)
if(WIN32)
    list(APPEND TECFLIX_PLATFORM_LIBS ws2_32)
endif()

# Fuentes del núcleo de Metadata y Observabilidad
set(TECFLIX_CORE_SOURCES
        common/src/TelemetryManager.cpp
        services/metadata-api/src/VideoMetadata.cpp
        services/metadata-api/src/MetadataService.cpp
        services/metadata-api/src/MetadataController.cpp
)

if(TECFLIX_USE_MONGODB)
    list(APPEND TECFLIX_CORE_SOURCES services/metadata-api/src/MongoMetadataRepository.cpp)
endif()

# Biblioteca estática compartida entre el servidor y las pruebas unitarias
add_library(tecflix_metadata_core STATIC ${TECFLIX_CORE_SOURCES})

target_include_directories(tecflix_metadata_core PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/common/include
        ${CMAKE_CURRENT_SOURCE_DIR}/services/metadata-api/include
)

target_link_libraries(tecflix_metadata_core PUBLIC ${TECFLIX_PLATFORM_LIBS})

if(TECFLIX_USE_MONGODB)
    target_compile_definitions(tecflix_metadata_core PUBLIC TECFLIX_USE_MONGODB)
    find_package(mongocxx QUIET)
    find_package(bsoncxx QUIET)
    if(mongocxx_FOUND AND bsoncxx_FOUND)
        target_link_libraries(tecflix_metadata_core PUBLIC mongo::mongocxx_shared mongo::bsoncxx_shared)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(MONGOCXX REQUIRED libmongocxx)
        target_include_directories(tecflix_metadata_core PUBLIC ${MONGOCXX_INCLUDE_DIRS})
        target_link_libraries(tecflix_metadata_core PUBLIC ${MONGOCXX_LIBRARIES})
    endif()
endif()

# Ejecutable principal del servidor TecFlix
add_executable(tecflix-server src/main.cpp)
target_link_libraries(tecflix-server PRIVATE tecflix_metadata_core)

# Pruebas unitarias y registro en CTest
if(BUILD_TESTING)
    enable_testing()

    add_executable(test_metadata_api tests/test_metadata_api.cpp)
    target_link_libraries(test_metadata_api PRIVATE tecflix_metadata_core)
    add_test(NAME MetadataApiUnitTests COMMAND test_metadata_api)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_telemetry.cpp")
        add_executable(test_telemetry tests/test_telemetry.cpp)
        target_link_libraries(test_telemetry PRIVATE tecflix_metadata_core)
        add_test(NAME TelemetryUnitTests COMMAND test_telemetry)
    endif()

    add_test(NAME ServerCryptoSelfTest COMMAND tecflix-server --selftest)
endif()

# Reglas de instalación para empaquetado en CI/CD
install(TARGETS tecflix-server RUNTIME DESTINATION bin)