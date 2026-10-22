# TecFlix

Self-hosted video sharing. **Backend:** C++17, no third-party libraries (own HTTP server, SHA-256,
MP4 parser). **Frontend:** React + TypeScript (Vite) with **dash.js** playback.

## Run

1. Build the frontend once (needs Node 20+): `cd frontend && npm install && npm run build` → `web/dist`
2. Build the server (MSYS2 g++): `build.bat` (or `make` on Linux/macOS)
3. Install **ffmpeg** and make sure it is on `PATH` (or set `TECFLIX_FFMPEG` to its full path)
4. From this folder: `tecflix.exe [port]` (default 8080) → http://localhost:8080

Frontend development: run `tecflix.exe`, then `cd frontend && npm run dev` (http://localhost:5173, proxies the API).

## How DASH works here
On upload the server runs ffmpeg in the background and writes `data/dash/<id>/manifest.mpd` plus fMP4 segments
(H.264/AAC streams are copied, anything else is transcoded). The video's `dash` status goes
`processing` → `ready` (or `failed`); the player uses dash.js when `ready` and the original file otherwise.
Videos uploaded before this feature are packaged automatically when the server starts with ffmpeg available.
Without ffmpeg everything still works, using plain progressive playback.

## Features
accounts · upload (mp4/m4v/mov/webm/ogv/mkv, ≤ 4 GiB) · watch · delete · rename · public/private visibility ·
metadata panel (size, type, duration, resolution, codecs, brand, tracks, bitrate, dates).
MP4/MOV files get full metadata; other containers show size and type only.

Data lives in `data/`. Plain HTTP, no rate limiting: put it behind a TLS proxy before exposing it publicly.
