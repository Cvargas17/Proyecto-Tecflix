# TecFlix — Especificación Técnica de Contratos e Invariantes

## 1. Invariantes de Ingeniería (C++17)
* **Gestión de memoria y recursos:** Uso estricto de RAII (`std::unique_ptr`, `std::shared_ptr`). Prohibido el uso de punteros crudos con propiedad o estado global mutable.
* **Concurrencia:** Acceso a estructuras compartidas en memoria sincronizado mediante `std::mutex` / `std::shared_mutex`.
* **Desacoplamiento:** Toda capa de persistencia debe exponerse mediante interfaces puras (`IMetadataRepository`, `IUserRepository`) para permitir inyección de dependencias y pruebas unitarias aisladas ($\ge 90\%$ de cobertura).
* **Seguridad de entradas:** Validación explícita de cabeceras, parámetros de consulta, tamaños de *payload* y sanitización de rutas antes de operar en disco o base de datos.

---

## 2. Modelos de Datos y Contratos de Persistencia

### 2.1. Documento de Metadatos (`tecflix.videos` — MongoDB)
* **Identificador:** `_id` (`string` hexadecimal único).
* **Campos obligatorios:** `owner` (`string`), `title` (`string`, $1..120$ caracteres), `visibility` (`"public"` | `"private"`), `dash` (`"pending"` | `"processing"` | `"ready"` | `"error"`), `uploaded` (`int64` epoch segundos).
* **Campos opcionales / técnicos:** `topic` (`string`), `tags` (`array<string>`), `file` (`string`), `duration` (`double` $\ge 0.0$), `sizeBytes` (`int64` $\ge 0$).
* **Índices requeridos:**
    * `{ "owner": 1, "uploaded": -1 }`
    * `{ "visibility": 1, "topic": 1, "uploaded": -1 }`
    * `{ "tags": 1 }`

### 2.2. Registro de Usuario (`data/users.db` + Árbol B en disco)
* **Clave primaria del Árbol B:** `username` (búsqueda y actualización en $O(\log_t n)$).
* **Invariante de cuota:** `quota_bytes` (`uint64`) se asigna al registrar la cuenta y es **inmutable**. Toda ingesta debe cumplir `used_bytes + content_length <= quota_bytes`; de lo contrario se rechaza antes de escribir en disco.
* **Credenciales:** Almacenamiento exclusivo de hash criptográfico (`SHA-256` con *salt*), nunca texto plano.

---

## 3. Contratos de Interfaz REST

### 3.1. API de Metadatos (`/api/videos`)
* **Autenticación:** Cookie `tf_session` o cabecera `X-User` en entorno interno.
* **Autorización:** Lectura permitida si `visibility == "public"` o `owner == requester`. Escritura (`PATCH`, `DELETE`) permitida únicamente si `owner == requester`.

| Método | Endpoint | Parámetros / Cuerpo | Respuestas |
| :--- | :--- | :--- | :--- |
| `GET` | `/api/videos` | Query: `scope` (`all`\|`mine`), `sortBy` (`uploaded`\|`title`\|`author`\|`owner`), `order` (`asc`\|`desc`), `topic`, `tag`, `page` ($\ge 1$), `limit` ($1..50$) | `200 OK` `{ items, page, limit, totalPages, totalItems }` |
| `GET` | `/api/videos/{id}` | Path: `id` | `200 OK`, `403 Forbidden`, `404 Not Found` |
| `POST` | `/api/videos` | JSON: `{ id, title, topic?, tags?, visibility?, duration?, sizeBytes? }` | `201 Created`, `400 Bad Request`, `401 Unauthorized`, `409 Conflict` |
| `PATCH` | `/api/videos/{id}` | JSON parcial: `{ title?, topic?, tags?, visibility? }` (alias `POST /api/videos/{id}`) | `200 OK`, `400 Bad Request`, `401 Unauthorized`, `403 Forbidden`, `404 Not Found` |
| `DELETE` | `/api/videos/{id}` | Path: `id` (elimina registro en MongoDB y archivos en disco) | `200 OK`, `401 Unauthorized`, `403 Forbidden`, `404 Not Found` |

### 3.2. API de Usuarios y Sesiones (`/api/auth`, `/api/users`)
* `POST /api/register`: Crea cuenta, inicializa `quota_bytes` y retorna `Set-Cookie: tf_session=<token>`. (`201`, `400`, `409`).
* `POST /api/login` / `POST /api/logout`: Emite o revoca token de sesión. (`200`, `401`).
* `GET /api/me` & `GET /api/users/{username}`: Retorna `{ username, quota_bytes, used_bytes, remaining_bytes }`. (`200`, `401`, `404`).
* `PUT /api/users/{username}` & `DELETE /api/users/{username}`: Actualiza credenciales (ignorando cambios a `quota_bytes`) o elimina cuenta e índice en Árbol B. (`200`, `401`, `403`, `404`).

### 3.3. API de Ingesta y Streaming (`/api/upload`, `/dash/*`, `/video/*`)
* `POST /api/upload`: Valida cuota disponible, guarda el binario fuente, registra metadatos con `dash = "pending"` y encola transcodificación FFmpeg (`201 Created`, `401`, `403`/`413` por cuota excedida).
* **Máquina de estados DASH:** `pending` $\rightarrow$ `processing` $\rightarrow$ `ready` (manifiesto `manifest.mpd` + segmentos `.m4s` disponibles) | `error`.
* `GET /dash/{id}/{file}`: Sirve `application/dash+xml` y segmentos `video/mp4`.
* `GET /video/{id}`: Entrega flujo directo con soporte obligatorio para cabecera `Range: bytes=start-end` (`206 Partial Content`, `Accept-Ranges: bytes`).

---

## 4. Contrato de Observabilidad (OpenTelemetry)
* **Transporte:** OTLP/HTTP hacia `${OTLP_HOST}:${OTLP_PORT}` (defecto `127.0.0.1:4318`).
* **Propagación de contexto:** Cabecera W3C `traceparent` entre capas HTTP.
* **Trazas obligatorias (`POST /v1/traces`):**
    * Capa HTTP: `GET /api/videos`, `POST /api/videos`, `PATCH /api/videos/{id}`, `DELETE /api/videos/{id}`, `POST /api/upload`.
    * Persistencia y procesamiento: `MongoDB.insertOne`, `MongoDB.findOne`, `MongoDB.findPaginated`, `MongoDB.updateOne`, `MongoDB.updateDash`, `MongoDB.deleteOne`, `btree.lookup`, `ffmpeg.packageDash`.
* **Métricas obligatorias (`POST /v1/metrics`):** Contadores de solicitudes por ruta/código HTTP y latencias acumuladas ($p50, p95, p99$).

---

## 5. Invariantes de Escalabilidad (v1.0 $\rightarrow$ v2.0)
* **v1.0 (Nodo Único):** Instancia única en puerto `8080` con persistencia local y MongoDB standalone.
* **v2.0 (Alta Disponibilidad — Sin SPOF):**
    * Balanceo de carga con **NGINX** (*health checks* activos y failover automático entre $\ge 2$ nodos de aplicación).
    * **MongoDB Replica Set** para tolerancia a fallos en metadatos.
    * Replicación/almacenamiento compartido para `users.db`, índice Árbol B y segmentos DASH.