# TecFlix (Versión 2.0 — Alta Disponibilidad y Rendimiento Optimizado)

Plataforma distribuida de alojamiento, transcodificación **MPEG-DASH** y streaming de video bajo demanda (VOD) desarrollada en **C++17** y **React + TypeScript**. Implementa persistencia binaria indexada mediante **Árbol B en disco** para usuarios, **MongoDB Replica Set** para metadatos, balanceo de carga con **NGINX** sin *single-points of failure*, observabilidad distribuida con **OpenTelemetry + .NET Aspire Dashboard** y validación automatizada en **GitHub Actions**[cite: 40, 46, 47].

* **Curso:** Algoritmos y Estructuras de Datos II (CE 2103) — Instituto Tecnológico de Costa Rica
* **Especificación Técnica y Contratos:** Consulta [`SPEC.md`](./SPEC.md) para ver los modelos de datos, invariantes de ingeniería, contratos REST y métricas de telemetría.

---

## Arquitectura y Estructura del Repositorio

```text
Proyecto-Tecflix/
├── README.md                                  # Guía de arquitectura, despliegue V2.0 y pruebas
├── SPEC.md                                    # Especificación técnica de contratos e invariantes
├── docker-compose.yml                         # Orquestación HA: NGINX + 2 nodos + MongoDB Replica Set + OTel + Aspire
├── nginx/
│   └── nginx.conf                             # Balanceador de carga (least_conn) y failover automático
├── otel/
│   └── collector-config.yaml                  # Puente OTLP/HTTP (JSON) -> OTLP/gRPC (Aspire Dashboard)
├── scripts/
│   └── load_test.py                           # Benchmark concurrente (1 servidor vs. 2 servidores)
├── .github/
│   └── workflows/
│       └── ci.yml                             # Pipeline CI/CD: Frontend, CMake, CTest, gcovr (>= 90%) y Playwright E2E
└── TecFlix/
    ├── CMakeLists.txt                         # Configuración de compilación C++17, CTest y cobertura
    ├── Dockerfile                             # Build multi-etapa (Node 20 + Ubuntu 24.04 + mongo-cxx-driver + FFmpeg)
    ├── src/
    │   └── main.cpp                           # Servidor HTTP concurrente (Thread Pool, stateless auth, Range, FFmpeg)
    ├── common/
    │   ├── include/TelemetryManager.hpp       # Exportador OpenTelemetry OTLP/HTTP (W3C traceparent y percentiles)
    │   └── src/TelemetryManager.cpp
    ├── services/
    │   ├── metadata-api/                      # Microservicio de metadatos (IMetadataRepository, Service, Controller, MongoDB)
    │   └── user-api/                          # Gestión de usuarios (IUserRepository, DiskBTree por username y cuotas)
    ├── frontend/                              # Aplicación web React + TypeScript + reproductor dash.js
    ├── tests/                                 # Pruebas unitarias C++ (Metadata, Usuarios/Árbol B y Telemetría)
    ├── data/                                  # Almacenamiento compartido de videos, segmentos DASH y archivos binarios
    └── web/dist/                              # Bundle estático compilado de la aplicación web React
```

---

## Despliegue de Alta Disponibilidad V2.0 (Docker Compose)

La topología V2.0 elimina los *single-points of failure* desplegando dos instancias independientes del backend detrás de **NGINX**, sesiones sin estado firmadas con `SHA-256`, bloqueo multi-proceso sobre el **Árbol B en disco**, un **Replica Set de MongoDB (`rs0`)** y recolección centralizada de telemetría[cite: 46, 47].

### 1. Iniciar el clúster completo
Desde la raíz `Proyecto-Tecflix/`:

```bash
docker compose up -d --build
```

### 2. Servicios y Puertos Disponibles

| Servicio | URL / Puerto | Rol en la Arquitectura |
| :--- | :--- | :--- |
| **NGINX Load Balancer** | `http://localhost:8080` | Punto de entrada principal con balanceo `least_conn` y *failover* entre nodos |
| **TecFlix Node 1 (Directo)** | `http://localhost:8081` | Acceso directo a una única instancia (referencia para pruebas de carga)[cite: 46] |
| **TecFlix Node 2 (Redundante)** | Red interna Docker (`8080`) | Segunda instancia activa del backend compartiendo almacenamiento y sesión[cite: 46] |
| **Aspire Dashboard (OpenTelemetry)** | `http://localhost:18888` | Panel visual de trazas distribuidas (`Traces`) y métricas (`Metrics`)[cite: 46] |
| **OTel Collector** | `otel-collector:4318` | Recibe OTLP/HTTP JSON de los nodos y exporta vía gRPC a Aspire (`4317`)[cite: 46, 47] |
| **MongoDB Replica Set (`rs0`)** | `localhost:27017` | Clúster primario/secundario (`mongodb-primary` + `mongodb-secondary`)[cite: 46] |

### 3. Verificación de Tolerancia a Fallos (*Failover*)
Con una reproducción activa en `http://localhost:8080`, es posible detener un nodo de aplicación y el nodo primario de MongoDB sin interrumpir el servicio ni perder la sesión del usuario:

```bash
docker stop tecflix-node-2 tecflix-mongo-primary
```

Para restaurar todos los nodos:
```bash
docker start tecflix-node-2 tecflix-mongo-primary
```

---

## Compilación Local y Pruebas Unitarias (CMake)

### 1. Compilar el Frontend Web (React + TypeScript)
```powershell
cd TecFlix/frontend
npm ci
npm run build
New-Item -ItemType Directory -Force -Path ../web/dist | Out-Null
Copy-Item -Recurse -Force dist/* ../web/dist/
cd ..
```

### 2. Compilar Backend C++17 y Ejecutar Pruebas Unitarias (`CTest`)

#### Windows (Visual Studio 2022 / MSVC)
```powershell
cmake -S . -B build
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

#### Linux / macOS / WSL2 (Ninja + GCC)
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### 3. Medición de Cobertura de Código ($\ge 90\%$)
```bash
cmake -S TecFlix -B build-coverage -G Ninja -DCMAKE_BUILD_TYPE=Debug -DENABLE_COVERAGE=ON -DBUILD_TESTING=ON
cmake --build build-coverage --parallel
ctest --test-dir build-coverage --output-on-failure
gcovr -r TecFlix build-coverage \
  --filter "TecFlix/common/" \
  --filter "TecFlix/services/metadata-api/" \
  --filter "TecFlix/services/user-api/" \
  --exclude "TecFlix/tests/" \
  --fail-under-line 90 \
  --print-summary
```

---

## Pruebas de Carga (*Load-Test*: 1 Servidor vs. 2 Servidores)

El script `scripts/load_test.py` permite evaluar la latencia ($p50, p95, p99$), el rendimiento (`RPS`) y la tasa de errores bajo usuarios concurrentes comparando una instancia directa (`8081`) frente al clúster balanceado de dos nodos (`8080`)[cite: 46]:

```bash
# Prueba contra 1 servidor (nodo único)
python scripts/load_test.py http://localhost:8081

# Prueba contra 2 servidores balanceados con NGINX
python scripts/load_test.py http://localhost:8080
```

---

## Integración Continua (CI/CD)

Cada *push* o *Pull Request* ejecuta `.github/workflows/ci.yml` en GitHub Actions[cite: 40]:
1. **Frontend Build:** Compila la aplicación React + TypeScript con Vite[cite: 40].
2. **Unit Tests & Coverage:** Ejecuta `MetadataApiUnitTests`, `UserApiUnitTests`, `TelemetryUnitTests` y `ServerCryptoSelfTest` exigiendo **$\ge 90\%$** de cobertura con `gcovr`[cite: 40].
3. **Release Artifacts:** Genera y publica los ejecutables empaquetados para **Linux (`x64`)** y **Windows (`MinGW x64`)**[cite: 40].
4. **Functional E2E Tests:** Levanta el servidor compilado y ejecuta pruebas de interfaz de usuario y API con **Playwright** en Chromium[cite: 40].