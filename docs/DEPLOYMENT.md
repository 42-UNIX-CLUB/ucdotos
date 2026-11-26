# Deploying UC.OS as a website

UC.OS is a static site: a page, a few scripts, the kernel and the boot image. Any web server
can serve it. This repository carries one way to do it, with Docker Compose, so that the
server only needs `git pull` and one command. The files are in `deploy/`.

Status, 2026-11-26, measured on a development laptop: the image builds in about 1 minute
40 seconds from an empty cache (most of it downloading the toolchain; the OS itself compiles
in about 22 seconds) and is 187 MB (Caddy on Alpine plus the 58 MB site). In headless
Chromium against the container the desktop boots, the shell works, and Doom fetches its data
once, on its first start. A second container on a shared network reached the site by name,
as a tunnel container would. Not done yet: running it on the real server, the tunnel, the
public name, the variant with a cloudflared of its own (checked for syntax only: it needs a
token), and the report-only policy in a browser console.

## 1. Run it

From the repository root, on a machine with Docker:

```sh
docker compose -f deploy/compose.yml up -d --build
```

The site then answers on `http://127.0.0.1:8081` on that machine only. Nothing is opened to
the outside. To update after new commits:

```sh
git pull
docker compose -f deploy/compose.yml up -d --build
```

Browsers pick up the new version on their next load.

## 2. What the container does

`deploy/Dockerfile` has two stages.

1. **Build.** An Arch Linux image with clang, lld, llvm, make and the usual shell tools
   (diffutils, findutils, gawk, sed, grep), the toolchain the project is developed with.
   Arch is a rolling release and the build image is not pinned: the compiler version moves
   between builds and is printed in the build log (clang 23.1.5 on 2026-11-26). It copies the checkout (`.dockerignore` keeps out git history, build
   directories, worktrees, notes, game data, the club's private documents and `deploy/.env`)
   and runs `make B=build-site dist`.
2. **Serve.** Caddy 2.11.9 on Alpine, pinned by digest, serves `build-site/dist` from
   `/srv/ucos` on port 8080 inside the container, with `deploy/Caddyfile`.

Nothing is built or installed on the host.

The container runs as user 65534 (nobody) with a read-only root, no capabilities,
`no-new-privileges`, at most 100 processes and 256 MB of memory, and a health check that
fetches `/index.html`. The cloudflared image in `compose.cloudflared.yml` is pinned by digest
too. `.dockerignore` also keeps out any `.env`, `*.key`, `*.pem` and `secrets/`. Caddy keeps no access log: visitors' addresses are personal
data and nothing here needs them.

Settings, in the environment or in `deploy/.env`:

| Variable | Default | What |
|---|---|---|
| `UCOS_PORT` | 8081 | host port, on 127.0.0.1 only |
| `UCOS_DOOM_DATA` | 1 | 1: the build downloads Doom's two data files (`tools/get-doom-data.sh`, pinned SHA-256 sums) and the site serves them; 0: no game data, and Doom says so |
| `UCOS_TUNNEL_NETWORK` | none | the Docker network of an existing cloudflared container (section 3) |
| `TUNNEL_TOKEN` | none | only for a cloudflared of its own (section 3); keep it in `deploy/.env`, which git ignores |

## 3. Putting https in front

The site must be reached over https (or as localhost). Sound uses an interface browsers
offer only there, and today the page does not even boot on a plain http address that is not
localhost (see `STATUS.md`, known problems). TLS ends in front of the container. Three ways:

- **An existing cloudflared container** on the server (the owner's set-up). Add
  `deploy/compose.tunnel-network.yml`, which joins the site to that container's network:

  ```sh
  UCOS_TUNNEL_NETWORK=name-of-that-network \
  docker compose -f deploy/compose.yml -f deploy/compose.tunnel-network.yml up -d --build
  ```

  The network's name: `docker inspect CLOUDFLARED_CONTAINER --format '{{json .NetworkSettings.Networks}}'`.
  In the Cloudflare dashboard, the tunnel's public hostname points at `http://ucos-web:8080`.
  The loopback port 8081 is then not needed: use either the tunnel network or the port.
- **A cloudflared of its own**, for a server with none: put `TUNNEL_TOKEN=...` in
  `deploy/.env` and add `deploy/compose.cloudflared.yml` to the command.
- **A tunnel or reverse proxy on the host itself**: point it at `http://127.0.0.1:8081`.

## 4. The name

The owner asked for `uc.os.ensui.dev`. Through a Cloudflare Tunnel the name is proxied by
Cloudflare, and Cloudflare's free certificate covers `ensui.dev` and `*.ensui.dev` only.
`uc.os.ensui.dev` is two labels deep, so it needs an Advanced Certificate, or a name one
label deep instead. Not chosen yet.

In Cloudflare, turn off Rocket Loader and any script rewriting for the hostname: the page
loads its scripts as modules and must get them unchanged.

Browser storage is per origin. Once files are kept between visits (not built yet), the
hostname is where users' files live: changing it later leaves them behind.

## 5. Caching and headers

The page, its scripts, the kernel and the image are sent with `Cache-Control: no-cache`, so
a browser always checks for a new version and never runs an old kernel with a new page.
The data under `files/` (Doom's game data) has no such header and may be cached: the kernel
checks every file against its SHA-256 sum anyway.

`initrd.tar.gz` is sent as a plain file, never with `Content-Encoding` (the page unpacks it
itself), so the Caddyfile has no compression on purpose.

Also sent: `Strict-Transport-Security` for 300 seconds, `X-Content-Type-Options: nosniff`,
`Referrer-Policy: no-referrer`, and a small enforced Content Security Policy (no plugins, no
`<base>`, no framing). A fuller policy is sent as report-only until it has been checked.

## 6. Without Docker

`make B=build-NAME dist` writes `build-NAME/dist/`: copy it to any static host. Serve `.wasm`
as `application/wasm` and `.mjs` as JavaScript, do not add `Content-Encoding` to
`initrd.tar.gz`, and serve it over https.
