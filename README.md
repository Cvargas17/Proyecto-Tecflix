# TecFlix

Plataforma distribuida de alojamiento, transcodificación **MPEG-DASH** y streaming de video bajo demanda (VOD) desarrollada en **C++17** y **React + TypeScript**, con persistencia en **MongoDB**, observabilidad mediante **OpenTelemetry (.NET Aspire Dashboard)** y validación automatizada en **GitHub Actions**.

* **Curso:** Algoritmos y Estructuras de Datos II (CE 2103) — Instituto Tecnológico de Costa Rica
* **Especificación Técnica y Contratos:** Consulta [`SPEC.md`](./SPEC.md) para ver los esquemas de datos, invariantes de ingeniería, endpoints REST y contratos de telemetría.

---

## Estructura del Repositorio

```text
Proyecto-Tecflix/
├── README.md                                # Guía general, compilación y despliegue
├── SPEC.md                                  # Especificación técnica de contratos e invariantes
├── .gitignore                               # Exclusión de binarios, IDEs, *.db y medios locales
├── docker-compose.yml                       # Orquestación: tecflix-server + mongodb + aspire-dashboard
├── .github/
│   └── workflows/
│       └── ci.yml                           # Pipeline CI/CD: CMake, CTest, gcovr (>= 90%) y Playwright E2E
└── TecFlix/
    ├── CMakeLists.txt                       # Configuración de compilación C++17, CTest y cobertura
    ├── Dockerfile                           # Build multi-etapa (Ubuntu 24.04 + mongo-cxx-driver + FFmpeg)
    ├── src/
    │   └── main.cpp                         # Servidor HTTP concurrente y enrutamiento principal
    ├── common/
    │   ├── include/TelemetryManager.hpp     # Exportador OpenTelemetry (trazas y métricas OTLP/HTTP)
    │   └── src/TelemetryManager.cpp
    ├── services/
    │   ├── metadata-api/                    # Microservicio de metadatos (Dominio, Servicio, Controlador, MongoDB)
    │   ├── users-api/                       # Gestión de usuarios, Árbol B en disco y cuotas de almacenamiento
    │   └── streaming-api/                   # Ingesta multimedia, transcodificación FFmpeg y HTTP Range
    ├── frontend/                            # Aplicación web React + TypeScript + reproductor DASH
    └── tests/                               # Pruebas unitarias C++ (CTest)
```

---

## Despliegue Rápido con Docker Compose (Recomendado)

El entorno contenedorizado levanta el servidor compilado con soporte nativo para MongoDB (`-DTECFLIX_USE_MONGODB=ON`), `ffmpeg`, una instancia de MongoDB 7 y el panel de observabilidad **.NET Aspire Dashboard**.

### 1. Iniciar los servicios
Desde la raíz del repositorio (`Proyecto-Tecflix/`):

```bash
docker compose up -d --build
```

### 2. Accesos y Puertos Disponibles

| Servicio | URL / Endpoint | Descripción |
| :--- | :--- | :--- |
| **TecFlix Server & API** | `http://localhost:8080` | Aplicación web y API REST (`/api/videos`, `/api/users`, `/api/upload`) |
| **Healthcheck** | `http://localhost:8080/health` | Verificación de estado del servidor (`{"status":"healthy"}`) |
| **Aspire Dashboard** | `http://localhost:18888` | Panel visual de trazas distribuidas (`Traces`) y métricas (`Metrics`) |
| **Receptor OTLP/HTTP** | `http://localhost:4318` | Colector de telemetría interno para `TelemetryManager` |
| **MongoDB** | `mongodb://localhost:27017` | Base de datos `tecflix`, colección `videos` |

### 3. Detener los servicios
```bash
docker compose down
```
*(Para eliminar también los volúmenes persistentes de MongoDB y videos, ejecuta `docker compose down -v`).*

---

## Compilación Local y Pruebas Unitarias (CMake)

### Requisitos previos
* Compilador compatible con **C++17** (`g++` $\ge 9$, `clang++` $\ge 10$ o MinGW-w64 en Windows).
* **CMake** $\ge 3.16$.
* *(Opcional)* `mongo-cxx-driver` (`r3.10.1`) si se desea enlazar MongoDB fuera de Docker; de lo contrario el servidor compila automáticamente con almacenamiento embebido.
* *(Opcional)* `ffmpeg` en el `PATH` para transcodificación MPEG-DASH.

### Compilar y ejecutar pruebas en Linux / macOS / WSL2
```bash
cmake -S TecFlix -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Compilar y ejecutar pruebas en Windows (PowerShell + MinGW)
```powershell
cmake -S TecFlix -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### Ejecutar el servidor localmente
```bash
# Verificar módulos criptográficos y arranque rápido
./build/tecflix-server --selftest

# Iniciar servidor HTTP en el puerto 8080
./build/tecflix-server 8080
```

### Medición de Cobertura de Código ($\ge 90\%$)
```bash
cmake -S TecFlix -B build-cov -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON
cmake --build build-cov --parallel
ctest --test-dir build-cov --output-on-failure
gcovr -r TecFlix build-cov --filter "TecFlix/common/" --filter "TecFlix/services/metadata-api/" --fail-under-line 90 --print-summary
```

---

## Variables de Entorno

| Variable | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `PORT` | `8080` | Puerto de escucha HTTP del servidor TecFlix |
| `MONGO_URI` | `mongodb://mongodb:27017` | Cadena de conexión a la instancia o Replica Set de MongoDB |
| `MONGO_DB` | `tecflix` | Nombre de la base de datos en MongoDB |
| `MONGO_COLLECTION` | `videos` | Nombre de la colección de metadatos de videos |
| `OTLP_HOST` | `127.0.0.1` (`aspire-dashboard` en Docker) | Host del colector OpenTelemetry OTLP/HTTP |
| `OTLP_PORT` | `4318` | Puerto HTTP del colector OpenTelemetry |

---

## Integración Continua (CI/CD)

Cada *push* o *Pull Request* ejecuta automáticamente `.github/workflows/ci.yml`, validando:
1. Compilación limpia en C++17 y ejecución de pruebas unitarias con `CTest`.
2. Cobertura de código $\ge 90\%$ mediante `gcovr` con publicación de reporte HTML/XML.
3. Empaquetado de ejecutables descargables para **Linux (`x64`)** y **Windows (`MinGW x64`)** en *Artifacts*.
4. Pruebas funcionales de extremo a extremo (E2E) contra el servidor activo con **Playwright**.