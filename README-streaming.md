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

System packages (install with your distro's package manager):

| Tool      | Used for                              | Required if you use   |
|-----------|---------------------------------------|-----------------------|
| `ffmpeg`  | decoding / resampling                 | any streaming source  |
| `yt-dlp`  | SoundCloud / http(s) extraction       | SoundCloud or http(s) |
| `jq`      | parsing yt-dlp JSON in scan-soundcloud| SoundCloud            |
| `librespot` | Spotify Connect audio                | Spotify               |

Python (only for `scan-spotify`):

```bash
pip install -r requirements-streaming.txt
```

Then make the scripts executable:

```bash
chmod +x scan-soundcloud scan-spotify import-streaming
```

## Authentication

### SoundCloud

`yt-dlp` handles SoundCloud's web client_id automatically — no
account or API key needed for public tracks. Private/Go+ tracks
that require login are out of scope.

### Spotify (two one-time setups)

**1. Web API client (for `scan-spotify`)**

Free Spotify developer registration; takes a minute:

1. Go to <https://developer.spotify.com/dashboard> and create an app.
2. Set the redirect URI to `http://localhost:8888/callback`.
3. Export your client id:

   ```bash
   export SPOTIFY_CLIENT_ID=<your-client-id>
   ```

The first run of `scan-spotify` will pop a browser tab to grant the
app access to your library; the token is then cached at
`~/.cache/xwax/spotify-token.json`.

> **BPM caveat:** Spotify deprecated the `audio_features` endpoint
> for apps registered after 2024-11-27. If your app cannot fetch
> tempo data, scan-spotify silently emits records without BPM and
> the catalog still populates correctly.

**2. librespot login (for `import-streaming`)**

`librespot` needs your Premium account credentials once. Easiest
flow is Zeroconf discovery:

1. Run `librespot --name "xwax-test"` once on the same network as
   the Spotify app on your phone/desktop.
2. In the official Spotify app, open *Connect to a device* and
   pick `xwax-test`.
3. Credentials are cached; future invocations work headless.

If your `librespot` build does not include `--single-track`
(mainline supports it; some distro packages ship older builds),
either use a recent `librespot` from <https://github.com/librespot-org/librespot>
or build from source with `cargo install librespot`.

> **DRM caveat:** Some Spotify content (newer hi-fi tracks, certain
> podcasts) is Widevine-only and librespot cannot decode it. Those
> tracks still appear in the catalog from `scan-spotify`; the
> import will fail and xwax displays "Error importing" via its
> existing status path. No code path here attempts to circumvent
> Widevine.

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

* `spotify:track:<id>` → `librespot` pipe
* `http(s)://…` → `yt-dlp` pipe (SoundCloud, YouTube, Bandcamp, …)
* anything else → delegates to the stock `./import` script

## Performance

| Source     | Load speed                       | Cached re-load |
|------------|----------------------------------|----------------|
| Local file | as fast as disk + decoder        | n/a            |
| SoundCloud | typically 5–20× real-time        | instant        |
| Spotify    | roughly real-time (~5 min track ≈ 5 min wait) | instant |

Spotify is the slow one because librespot streams via the same
Connect protocol used by hardware speakers. Once a track has been
loaded once, the cache makes the next scratch instant.

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

* **"librespot: unrecognized argument '--single-track'"** — install
  a recent librespot (`cargo install librespot`).
* **"Track import completed with status …"** in xwax's status bar —
  the importer process exited non-zero. Run `import-streaming
  '<pathname>' 44100 > /dev/null` directly in a terminal to see the
  underlying error from librespot or yt-dlp.
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
