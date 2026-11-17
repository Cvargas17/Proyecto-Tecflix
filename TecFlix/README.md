# TecFlix (Versión 1.0)

Plataforma de alojamiento, transcodificación **MPEG-DASH** y streaming de video bajo demanda (VOD) desarrollada en **C++17** y **React + TypeScript**, con persistencia binaria indexada mediante **Árbol B en disco** para usuarios, **MongoDB** para metadatos, observabilidad distribuida mediante **OpenTelemetry (.NET Aspire Dashboard)** y validación automatizada en **GitHub Actions**.

* **Curso:** Algoritmos y Estructuras de Datos II (CE 2103) — Instituto Tecnológico de Costa Rica
* **Alcance de esta versión (`v1.0`):** Implementación funcional completa en instancia única con todos los componentes integrados de extremo a extremo.
* **Especificación Técnica y Contratos:** Consulta [`SPEC.md`](./SPEC.md) para revisar los esquemas de datos, invariantes de ingeniería, contratos REST y métricas de telemetría.

---

## Estructura del Repositorio

```text
Proyecto-Tecflix/
├── README.md                                  # Guía de arquitectura, compilación y ejecución (v1.0)
├── SPEC.md                                    # Especificación técnica de contratos e invariantes
├── docker-compose.yml                         # Orquestación de contenedores (servidor, MongoDB y Aspire)
├── .github/
│   └── workflows/
│       └── ci.yml                             # Pipeline CI/CD: Frontend, CMake, CTest, gcovr (>= 90%) y Playwright
└── TecFlix/
    ├── CMakeLists.txt                         # Configuración de compilación C++17, CTest y cobertura
    ├── Dockerfile                             # Build multi-etapa (Node 20 + Ubuntu 24.04 + mongo-cxx-driver + FFmpeg)
    ├── src/
    │   └── main.cpp                           # Servidor HTTP concurrente (Thread Pool), enrutamiento, Range y FFmpeg
    ├── common/
    │   ├── include/TelemetryManager.hpp       # Exportador OpenTelemetry OTLP/HTTP (W3C traceparent y percentiles)
    │   └── src/TelemetryManager.cpp
    ├── services/
    │   ├── metadata-api/                      # Gestión de metadatos (IMetadataRepository, Service, Controller, MongoDB)
    │   └── user-api/                          # Gestión de usuarios (IUserRepository, DiskBTree por username y cuotas)
    ├── frontend/                              # Aplicación web React + TypeScript + reproductor dash.js
    ├── tests/                                 # Pruebas unitarias C++ (Metadata, Usuarios/Árbol B y Telemetría)
    ├── data/                                  # Almacenamiento local de videos, segmentos DASH y archivos binarios
    └── web/dist/                              # Bundle estático compilado de la aplicación web React
```

---

## Componentes Principales (v1.0)

1. **Aplicación Web (`TecFlix/frontend`):** Interfaz en React + TypeScript con registro/inicio de sesión, catálogo paginado y ordenable (por fecha de carga, título y autor), filtro por tema, visualización de cuota fija disponible y reproducción adaptativa mediante `dash.js` (con respaldo directo MP4/WebM).
2. **API de Usuarios (`TecFlix/services/user-api`):** Persistencia binaria (`data/users.db`) indexada por `username` mediante un **Árbol B en disco** (`data/users.idx`) con complejidad $O(\log_t n)$, contraseñas almacenadas con `SHA-256 + salt` y validación atómica de cuota fija (`quota_bytes`) antes de escribir videos en disco.
3. **API de Metadata (`TecFlix/services/metadata-api`):** CRUD completo de metadatos con control de visibilidad (`public` / `private`), paginación, ordenamiento y almacenamiento en **MongoDB** (con repositorio embebido de respaldo cuando se compila localmente sin `libmongocxx`).
4. **API de Streaming e Ingesta (`TecFlix/src/main.cpp`):** Recepción de archivos multimedia con validación previa de cuota, transcodificación asíncrona a **MPEG-DASH** (`manifest.mpd` + segmentos `.m4s`) mediante `FFmpeg` (hasta 2 trabajos simultáneos) y entrega directa con soporte `HTTP Range` (`206 Partial Content`).
5. **Dashboard de Observabilidad (`TecFlix/common`):** Instrumentación RAII (`ScopedSpan`) con propagación de contexto W3C `traceparent`, exportación asíncrona OTLP/HTTP hacia **.NET Aspire Dashboard**, contadores de solicitudes HTTP y cálculo de latencias acumuladas ($p50, p95, p99$).

---

## Compilación y Ejecución Local (Nodo Único v1.0)

### Requisitos Previos
* **Node.js** $\ge 20$ y `npm` (para compilar el frontend React).
* **CMake** $\ge 3.16$ y compilador **C++17** (Visual Studio 2022 / MSVC en Windows, MinGW-w64 o `g++` $\ge 9$ en Linux/WSL2).
* *(Opcional)* `ffmpeg` disponible en el `PATH` del sistema para habilitar la transcodificación automática a MPEG-DASH.

### 1. Compilar el Frontend Web (React + TypeScript)
Desde la raíz `Proyecto-Tecflix/`:

```powershell
cd TecFlix/frontend
npm install
npm run build
New-Item -ItemType Directory -Force -Path ../web/dist | Out-Null
Copy-Item -Recurse -Force dist/* ../web/dist/
cd ..
```

### 2. Compilar el Backend C++17 y Ejecutar Pruebas Unitarias

#### En Windows con Visual Studio 2022 (MSVC)
```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

#### En Linux / macOS / Windows con MinGW + Ninja
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### 3. Iniciar el Servidor v1.0
Desde la carpeta `TecFlix/`:

```powershell
# Windows (MSVC)
.\build\Debug\tecflix-server.exe

# Linux / macOS / MinGW
./build/tecflix-server
```

Abre **`http://localhost:8080`** en tu navegador para registrar un usuario, subir videos, gestionar metadatos y reproducir contenido.

---

## Ejecución con Docker (v1.0 con MongoDB Real y Aspire Dashboard)

Para levantar el sistema completo con **MongoDB 7**, **FFmpeg** y **.NET Aspire Dashboard** desde la raíz `Proyecto-Tecflix/`:

```bash
docker compose up -d --build
```

| Servicio | URL / Puerto | Descripción |
| :--- | :--- | :--- |
| **Aplicación Web y API REST** | `http://localhost:8080` | Interfaz React y endpoints `/api/*`, `/dash/*`, `/video/*` |
| **Nodo Directo (Instancia 1)** | `http://localhost:8081` | Acceso directo a una sola instancia del servidor |
| **Healthcheck** | `http://localhost:8080/health` | Estado de salud del servidor (`{"status":"healthy"}`) |
| **Aspire Dashboard (OpenTelemetry)** | `http://localhost:18888` | Visualización en tiempo real de trazas (`Traces`) y métricas (`Metrics`) |
| **MongoDB** | `mongodb://localhost:27017` | Base de datos `tecflix`, colección `videos` |

---

## Contratos REST Principales

| Método | Endpoint | Descripción |
| :--- | :--- | :--- |
| `POST` | `/api/register` | Registra un usuario en el Árbol B en disco e inicializa su cuota fija |
| `POST` | `/api/login` / `/api/logout` | Inicia o cierra sesión mediante cookie `tf_session` |
| `GET` | `/api/me` | Devuelve `{ username, quota_bytes, used_bytes, remaining_bytes }` |
| `GET` / `PUT` / `DELETE` | `/api/users/{username}` | Consulta perfil/cuota, actualiza contraseña o elimina la cuenta |
| `GET` | `/api/videos` | Lista videos con soporte para `scope`, `sortBy`, `order`, `topic`, `tag`, `page` y `limit` |
| `GET` / `PATCH` / `DELETE` | `/api/videos/{id}` | Consulta, edita metadatos o elimina un video (liberando cuota del propietario) |
| `POST` | `/api/upload` | Valida cuota disponible, guarda el video y encola empaquetado DASH con FFmpeg |
| `GET` | `/dash/{id}/{file}` | Sirve el manifiesto `manifest.mpd` y los segmentos `.m4s` para `dash.js` |
| `GET` | `/video/{id}` | Streaming directo con soporte para cabecera `Range: bytes=start-end` |

---

## Variables de Entorno

| Variable | Valor por defecto | Descripción |
| :--- | :--- | :--- |
| `PORT` | `8080` | Puerto de escucha HTTP del servidor TecFlix |
| `DEFAULT_QUOTA_BYTES` | `1073741824` (1 GiB) | Cuota fija de almacenamiento asignada al registrar cada usuario |
| `TECFLIX_FFMPEG` | `ffmpeg` | Ruta al ejecutable de FFmpeg para transcodificación DASH |
| `MONGO_URI` | `mongodb://localhost:27017` | Cadena de conexión a MongoDB |
| `MONGO_DB` | `tecflix` | Nombre de la base de datos en MongoDB |
| `MONGO_COLLECTION` | `videos` | Colección donde se almacenan los metadatos de video |
| `OTLP_HOST` | `127.0.0.1` | Host del colector OpenTelemetry OTLP/HTTP |
| `OTLP_PORT` | `4318` | Puerto HTTP del colector OpenTelemetry |

---

## Integración Continua (CI/CD)

El workflow `.github/workflows/ci.yml` valida automáticamente cada cambio mediante:
1. Compilación del frontend React/TypeScript (`npm run build`).
2. Compilación de C++17 con instrumentación `--coverage` y ejecución de `CTest` (`MetadataApiUnitTests`, `UserApiUnitTests`, `TelemetryUnitTests` y `ServerCryptoSelfTest`).
3. Verificación estricta de cobertura de código **$\ge 90\%$** mediante `gcovr`.
4. Empaquetado de ejecutables descargables para Linux (`x64`) y Windows (`x64`).
5. Pruebas funcionales de extremo a extremo (E2E) sobre la interfaz web con **Playwright**.