# ==============================================================================
# Etapa 1: Compilación del Frontend React + TypeScript
# ==============================================================================
FROM node:20-alpine AS frontend-builder

WORKDIR /frontend
COPY frontend/package*.json ./
RUN npm ci
COPY frontend/ ./
RUN npx vite build --outDir /frontend/dist

# ==============================================================================
# Etapa 2: Compilación (Builder) con CMake, C++17 y mongo-cxx-driver oficial
# ==============================================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    ninja-build \
    pkg-config \
    curl \
    ca-certificates \
    libmongoc-dev \
    libbson-dev \
    libssl-dev \
    libsasl2-dev \
    && rm -rf /var/lib/apt/lists/*

# Compilar e instalar mongo-cxx-driver (r3.10.1) sobre libmongoc-dev del sistema
RUN curl -fsSL https://github.com/mongodb/mongo-cxx-driver/releases/download/r3.10.1/mongo-cxx-driver-r3.10.1.tar.gz -o /tmp/mongo-cxx-driver.tar.gz \
    && tar -xzf /tmp/mongo-cxx-driver.tar.gz -C /tmp \
    && cmake -S /tmp/mongo-cxx-driver-r3.10.1 -B /tmp/mongo-cxx-build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_STANDARD=17 \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DBUILD_VERSION=3.10.1 \
        -DENABLE_TESTS=OFF \
        -DENABLE_EXAMPLES=OFF \
    && cmake --build /tmp/mongo-cxx-build --parallel \
    && cmake --install /tmp/mongo-cxx-build \
    && rm -rf /tmp/mongo-cxx-driver* /tmp/mongo-cxx-build

WORKDIR /src

COPY CMakeLists.txt ./
COPY common/ ./common/
COPY services/ ./services/
COPY src/ ./src/

RUN cmake -S . -B /build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DTECFLIX_USE_MONGODB=ON \
    -DBUILD_TESTING=OFF \
    && cmake --build /build --parallel

# ==============================================================================
# Etapa 3: Imagen de ejecución (Runtime) con librerías compartidas, FFmpeg y Web
# ==============================================================================
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    libmongoc-1.0-0 \
    libbson-1.0-0 \
    ffmpeg \
    curl \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copiar binario compilado, librerías runtime y frontend construido
COPY --from=builder /build/tecflix-server /app/tecflix-server
COPY --from=builder /usr/local/lib/libmongocxx* /usr/local/lib/
COPY --from=builder /usr/local/lib/libbsoncxx* /usr/local/lib/
COPY --from=frontend-builder /frontend/dist /app/web/dist
RUN ldconfig && /app/tecflix-server --selftest

# Crear directorios de persistencia de medios
RUN mkdir -p /app/data/videos /app/data/dash

ENV PORT=8080 \
    MONGO_URI=mongodb://mongodb:27017 \
    MONGO_DB=tecflix \
    MONGO_COLLECTION=videos \
    OTLP_HOST=aspire-dashboard \
    OTLP_PORT=4318

EXPOSE 8080

HEALTHCHECK --interval=15s --timeout=5s --start-period=10s --retries=3 \
    CMD curl -fsS http://localhost:8080/health || exit 1

ENTRYPOINT ["/app/tecflix-server"]