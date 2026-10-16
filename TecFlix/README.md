# TecFlix

A self-hosted video sharing web app written in C++17 with **no third-party dependencies**
(own HTTP server on Winsock/BSD sockets, own SHA-256, own MP4 parser). The UI is a single
static page in `web/`.

## Build & run

Windows (MSYS2/MinGW g++): `build.bat` · Linux/macOS: `make`

```
tecflix.exe [port]      # default 8080; run from this folder so ./web and ./data resolve
```
Open http://localhost:8080

## Features
- Accounts (register / login / logout; salted, iterated SHA-256 password hashes; HttpOnly session cookie)
- Upload (mp4, m4v, mov, webm, ogv, mkv, up to 4 GiB) with progress bar
- Watch with seeking (HTTP Range streaming)
- Delete, rename, and switch **public / private** (private videos are invisible to everyone but the owner)
- Metadata panel: file name/size/type, duration, resolution + aspect ratio, video/audio codec,
  container brand, track count, average bitrate, media creation date, upload date

## Notes
- Data lives in `data/` (`users.db`, `videos.db`, `videos/`).
- Deep metadata (duration, resolution, codecs) is parsed from MP4/MOV; WebM/OGV/MKV show size/type only.
- Plain HTTP with no rate limiting: put it behind a TLS reverse proxy before exposing it to the internet.
