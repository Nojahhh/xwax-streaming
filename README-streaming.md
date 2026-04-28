# xwax streaming integration

This adds SoundCloud and Spotify (Premium) as track sources for xwax
without changing any C code. It plugs into the existing `-s <scanner>`
and `-i <importer>` extension points.

* `scan-soundcloud` — SoundCloud scanner (URLs, sets, likes, reposts).
* `scan-spotify` — Spotify scanner (liked, playlists, albums, tracks).
* `import-streaming` — universal importer that decodes anything
  (Spotify URI, https URL or local file) into the S16LE PCM stream
  xwax expects.

Streaming tracks are fully downloaded and decoded to PCM at load
time, so every existing DVS feature (scratching, scrubbing, cue
points, BPM, overview meter) keeps working unchanged.

## Install

### The easy way: `./install-wizard`

```bash
./install-wizard
```

The Linux install wizard at the repo root takes you end-to-end:

* detects which streaming tools are present;
* offers to install the **`spotty`** fork of librespot
  ([michaelherger/librespot](https://github.com/michaelherger/librespot),
  branch `spotty`) via
  `cargo install --git ... --branch spotty spotty` if no
  `--single-track`-capable binary is in `PATH`. The wizard prepends
  `~/.cargo/bin` to `PATH` for the rest of the run. Mainline
  `librespot` from crates.io is **not** used because it does not
  ship the `--single-track` flag that `import-streaming` relies on;
* runs the **one-time Zeroconf pairing** for you — launches
  `spotty --name xwax-test --backend pipe --cache ~/.cache/xwax/librespot`,
  waits while you pick *xwax-test* in the official Spotify app on
  your phone or desktop, then verifies credentials were saved;
* creates a Python venv for `scan-spotify` at
  `~/.local/share/xwax/streaming-venv` and `pip install`s `spotipy`;
* makes the streaming scripts executable;
* writes `~/.config/xwax/streaming.env` with your Spotify client id
  (so you can `source` it before launching xwax).

The wizard does *not* install distro packages for the system tools
(`ffmpeg`, `yt-dlp`, `jq`) — it tells you which are missing and
prints the right command for your distro. `spotty` is the one
exception: it isn't packaged by any distro and is built from the
fork's source via cargo.

### Manual setup

System packages (install with your distro's package manager):

| Tool      | Used for                              | Required if you use   |
|-----------|---------------------------------------|-----------------------|
| `ffmpeg`  | decoding / resampling                 | any streaming source  |
| `yt-dlp`  | SoundCloud / http(s) extraction       | SoundCloud or http(s) |
| `jq`      | parsing yt-dlp JSON in scan-soundcloud| SoundCloud            |
| `spotty`  | Spotify Connect audio (`--single-track`) | Spotify            |

`spotty` is a fork of `librespot` that adds the `--single-track`
flag `import-streaming` needs. Mainline `librespot` (the
`librespot-org/librespot` crate on crates.io) does **not** support
`--single-track` and will not work as a replacement.

Build dependencies: `git`, `pkg-config`, OpenSSL development headers
(`libssl-dev` on Debian/Ubuntu, `openssl-devel` on Fedora) and ALSA
development headers (`libasound2-dev` / `alsa-lib-devel`).

Plain `cargo install --git ... spotty` does **not** work: the
spotty branch does an unconditional `include_str!("client_id.txt")`
at compile time and that file is `.gitignore`d upstream (the
maintainer drops in their LMS plugin's id during their own
release builds). You'll get:

```
error: couldn't read `src/client_id.txt`: No such file or directory
```

Workaround — clone, write an empty placeholder, install from path:

```bash
git clone --depth 1 --branch spotty \
    https://github.com/michaelherger/librespot \
    "$HOME/.cache/xwax/spotty-src"
: > "$HOME/.cache/xwax/spotty-src/src/client_id.txt"
cargo install --path "$HOME/.cache/xwax/spotty-src" --locked
```

The empty placeholder is fine for our use case: the value is only
consumed by Spotify Web API code paths (e.g. `--get-token`) that
`import-streaming` does not exercise. If you ever need a real id
for those features, pass it at runtime via `--client-id`.

`./install-wizard` runs exactly this dance for you (with a
disk-backed `TMPDIR` redirect when `/tmp` is too small, automatic
`rustup` install if `cargo` is missing, and the build-deps
pre-flight check).

Python (only for `scan-spotify`):

```bash
pip install -r requirements-streaming.txt
```

Then make the scripts executable:

```bash
chmod +x scan-soundcloud scan-spotify import-streaming
```

`scan-spotify` ships with a polyglot `/bin/sh` + Python shebang that
auto-uses the wizard's venv at `~/.local/share/xwax/streaming-venv` if
present, otherwise falls back to system `python3`. You can override
the interpreter with `XWAX_STREAMING_PY=/path/to/python ./scan-spotify ...`.

## Authentication

### SoundCloud

`yt-dlp` handles SoundCloud's web client_id automatically — no
account or API key needed for public tracks. Private/Go+ tracks
that require login are out of scope.

### Spotify (two one-time setups)

**1. Web API client (for `scan-spotify`)**

Free Spotify developer registration; takes a minute:

1. Go to <https://developer.spotify.com/dashboard> and create an app.
2. Set the redirect URI to `http://127.0.0.1:8888/callback`. Spotify
   rejects `localhost` for apps registered after April 2025; the
   redirect URI must be a loopback IP.
3. Export your client id and the matching redirect URI:

   ```bash
   export SPOTIFY_CLIENT_ID=<your-client-id>
   export SPOTIFY_REDIRECT_URI="http://127.0.0.1:8888/callback"
   ```

   (`./install-wizard` writes both into `~/.config/xwax/streaming.env`
   for you; just `source` it before launching xwax.)

The first run of `scan-spotify` will pop a browser tab to grant the
app access to your library; the token is then cached at
`~/.cache/xwax/spotify-token.json`.

> **BPM caveat:** Spotify deprecated the `audio_features` endpoint
> for apps registered after 2024-11-27. If your app cannot fetch
> tempo data, scan-spotify silently emits records without BPM and
> the catalog still populates correctly.

**2. spotty login (for `import-streaming`)**

`spotty` needs your Premium account credentials once. The
`./install-wizard` flow does this for you, but the manual steps are:

1. Pair once via Zeroconf, **into the same cache directory
   `import-streaming` reads from**, and use `--backend pipe` so the
   client doesn't try to open an audio device (the default `rodio`
   backend panics with `NoDeviceAvailable` on hosts where cpal can't
   find a default ALSA card, e.g. Pi OS + PipeWire):

   ```bash
   spotty --name xwax-test --backend pipe \
       --cache "${XDG_CACHE_HOME:-$HOME/.cache}/xwax/librespot" \
       >/dev/null
   ```

2. In the official Spotify app on a phone or desktop on the same
   network, open *Connect to a device* and pick `xwax-test`.
3. Once it shows as connected, Ctrl-C `spotty`. Credentials are now
   persisted at `~/.cache/xwax/librespot/credentials.json` (the
   directory keeps its `librespot` name for backward compatibility
   with existing pairings; you can also point spotty at a different
   path if you prefer).
4. Future `import-streaming` invocations pass the same `--cache`
   path with `--disable-discovery --single-track` and authenticate
   silently. `import-streaming` looks for `spotty` first, then falls
   back to `librespot` only if it advertises `--single-track`.

> **Why not mainline librespot?** The CLI binary published by
> `librespot-org/librespot` does not have a `--single-track` flag.
> The flag is specific to the `spotty` fork (originally built for
> the Logitech Media Server Spotty plugin). `import-streaming`
> needs a "fetch one URI, write PCM to stdout, exit" mode, and
> `spotty --single-track` is the cleanest way to get that today.

`./install-wizard` will offer to do the cargo install for you and
add `~/.cargo/bin` to `PATH` for the current session. Persist it
across shells by appending to `~/.bashrc`:

```bash
export PATH="$HOME/.cargo/bin:$PATH"
```

> **DRM caveat:** Some Spotify content (newer hi-fi tracks, certain
> podcasts) is Widevine-only and the librespot/spotty stack cannot
> decode it. Those tracks still appear in the catalog from
> `scan-spotify`; the import will fail and xwax displays
> "Error importing" via its existing status path. No code path here
> attempts to circumvent Widevine.

## Running xwax with streaming crates

Use `-s` and `-i` exactly the way xwax already documents them — they
are positional, applying to the next `-l` (library) or audio device.

```bash
./xwax \
  -i ./import-streaming \
  -s ./scan            -l ~/music \
  -s ./scan-soundcloud -l 'https://soundcloud.com/discover/sets/charts-top:all-music' \
  -s ./scan-soundcloud -l 'https://soundcloud.com/yourusername/likes' \
  -s ./scan-spotify    -l 'liked' \
  -s ./scan-spotify    -l 'spotify:playlist:37i9dQZF1DXcBWIGoYBM5M' \
  -a hw:0 \
  -a hw:1
```

Each `-l` becomes its own crate (named via `basename` of the path,
see `library.c:597`); the *All records* crate aggregates them.

The single `-i ./import-streaming` covers every crate because the
importer dispatches on the per-track pathname:

* `spotify:track:<id>` → `spotty` (or `librespot` if it advertises
  `--single-track`) pipe
* `http(s)://…` → `yt-dlp` pipe (SoundCloud, YouTube, Bandcamp, …)
* anything else → delegates to the stock `./import` script

## Performance

| Source     | Load speed                       | Cached re-load |
|------------|----------------------------------|----------------|
| Local file | as fast as disk + decoder        | n/a            |
| SoundCloud | typically 5–20× real-time        | instant        |
| Spotify    | roughly real-time (~5 min track ≈ 5 min wait) | instant |

Spotify is the slow one because spotty/librespot streams via the
same Connect protocol used by hardware speakers. Once a track has
been loaded once, the cache makes the next scratch instant.

## Cache

* Location: `${XDG_CACHE_HOME:-$HOME/.cache}/xwax/streaming/`
* Format: raw S16LE stereo PCM at the deck sample rate.
* Key: `sha1(<pathname>:<rate>)` — same track at a different deck
  rate gets its own cache entry.
* Size cap: `XWAX_CACHE_BYTES` env var (default 20 GiB). Set to `0`
  to disable caching (useful when debugging an importer).
* Eviction: LRU by access time when the cap is exceeded, evaluated
  at the start of each cache-miss import.

A 5-minute stereo track at 44100 Hz is about 50 MB on disk, so 20 GiB
holds roughly 400 tracks.

## Troubleshooting

* **"librespot: unrecognized argument '--single-track'"** or
  **"warning: your librespot lacks --single-track"** — you have
  mainline `librespot` installed (it has never shipped this flag).
  Install the `spotty` fork (see Manual setup above for the full
  recipe — `cargo install --git ... spotty` alone fails on a
  missing `client_id.txt`, the wizard handles that).
  `import-streaming` and the wizard look for `spotty` first, so
  installing it alongside mainline `librespot` is enough — you do
  not need to uninstall the mainline binary.
* **"couldn't read `src/client_id.txt`: No such file or
  directory"** while installing spotty — the spotty branch
  references a file the upstream maintainer keeps out of git. Use
  the `git clone` + empty-placeholder + `cargo install --path`
  recipe in *Manual setup*, or just rerun `./install-wizard` which
  does this automatically.
* **"Track import completed with status …"** in xwax's status bar —
  the importer process exited non-zero. Run `import-streaming
  '<pathname>' 44100 > /dev/null` directly in a terminal to see the
  underlying error from spotty/librespot or yt-dlp.
* **Spotify auth loops in browser** — make sure the redirect URI in
  your Spotify dashboard exactly matches `SPOTIFY_REDIRECT_URI`.
* **No BPM on Spotify tracks** — your Spotify app lacks the
  `audio_features` permission; this is a Spotify-side limitation.
  Set BPMs manually in your scan output if you need them, or run
  `aubio tempo` over the cached PCM.
* **Streaming tracks appear in xwax but won't load** — confirm
  `import-streaming` is on the command line *before* the audio
  device, e.g. `-i ./import-streaming -a hw:0`.

## Personal-use disclaimer

This integration is intended for the user's own legally-licensed
content, used for personal and educational purposes. You are
responsible for complying with the terms of service of any
streaming provider you connect to it.
