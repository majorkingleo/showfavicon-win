# Plan 2 — Windows 11

## Goal

A tray (notification area) application: one icon per site, click → open browser,
hourly refresh, grayed icon when offline, configurable via a settings window.

## Architecture

- **C# / .NET 8 (WinForms)** — simplest reliable `NotifyIcon`. One `NotifyIcon`
  per site (max 2 in v1). Self-contained single-file publish.
- Background timer: `System.Threading.Timer` / `PeriodicTimer` (1 h) plus a fetch
  on startup.
- Fetching: `HttpClient` with timeout; same resolution logic as the Python script
  (parse `<link rel=icon>`, fallback `/favicon.ico`, normalize to PNG). Cache in
  `%LOCALAPPDATA%\ShowFavicon\<host>.png`.
- Graying: `System.Drawing` `ColorMatrix` (grayscale) plus reduced alpha applied
  to the cached image when the last fetch failed.
- Click handling: `NotifyIcon.Click` →
  `Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })`.

## Configuration & drag-and-drop

- Settings window (WinForms) with two URL fields and add/remove buttons.
- **Caveat:** Windows tray icons do **not** accept drag-and-drop. Alternatives:
  drag a URL onto the settings window's list (implement `AllowDrop` on the form),
  or a small always-visible "drop target" window. Recommended: drop onto the
  settings window plus a right-click "Add current clipboard URL" convenience item.
- Right-click context menu: *Open*, *Update now*, *Configure…*, *Exit*.

## Multi-site

One process hosts N `NotifyIcon`s. Note: Windows 11 hides tray icons in the
overflow by default — document that the user must drag them into the visible tray
area.

## What the Android port learned

The Android version of this idea is implemented and measured; the findings are in
[`plan-android.md`](plan-android.md). What carries over to Windows:

### Icon discovery is the hard part, not the rendering

- Sites ship the strangest things. Measured against real sites: a page whose only
  icon is an **SVG**, one with a **4 entry ICO** (32, 1, 16 and 1 bit), an **8 bit
  paletted ICO** with an AND mask, a **PNG inside an ICO**, and one whose ICO
  directory lies about the bit depth while its DIB header is correct.
- A **404 on `/favicon.ico` is normal**, and a site that declares nothing still
  serves something there. Keep the order: declared `<link rel="icon">` candidates
  first (apple-touch-icon, raster, vector, mask-icon), `/favicon.ico` **at the
  origin** last.
- Resolve a relative icon href against the URL that **finally answered**, not the one
  that was requested. The measured case: `/serverhealthcheck/www` answers 301 to
  `/www/` and then 302 to `login.php`, and its `favicon.php` exists only inside
  `/www/`. Resolved against the requested URL the candidate was one directory too
  high — 404 while the page itself answered fine, so the icon stayed gray and the
  network got blamed for it.
- Send a browser `User-Agent`: some CDNs serve their icon only to one.

### Formats

- `System.Drawing.Icon` reads ICO including palettes and the AND mask, so the
  hand-written decoder the Android app needed (about 200 lines) is not necessary
  here. If one is written anyway: a 32 bit DIB whose alpha channel is all zero keeps
  its transparency in the AND mask, and those pixels have to be made opaque first —
  otherwise the icon renders completely invisible.
- There is no built-in SVG renderer. Pick `Svg.Skia`, or rasterise through a
  WebView2. Dropping SVG costs real icons, so do not.
- Convert whatever arrives into a PNG in the cache, so exactly one code path is left
  downstream.

### Refresh, network and retries

- Refresh hourly, on startup, and **whenever the network changes**. On Android the
  default network callback produced a fetch 100 ms after airplane mode was switched
  off; on .NET the direct equivalent is `NetworkChange.NetworkAddressChanged` and
  `NetworkAvailabilityChanged`.
- A site that cannot be fetched is often only unreachable *right now* — a LAN
  address while the machine sits on another network is exactly the case that started
  this. Retry with a backoff (Android: linear from 30 s, six attempts per run) and
  keep the icon grayed meanwhile rather than empty.
- Do not spend a fetch when there is no network at all.

### Cache and state

- `%LOCALAPPDATA%\ShowFavicon\icons\<host>.png`, written to a temp file and renamed
  into place: Explorer and the tray read the file while a fetch is running.
- Keep the "last fetch failed" flag next to the cache file and show the **last known
  icon grayed** instead of an empty placeholder.
- Prune the files of sites that are no longer configured.
- One site is one host. The Android settings screen rejects a second URL with the
  same host, which keeps the cache key trivial.

### What does not transfer

- Java's `URI.resolve` quirk with a base that has no path, and
  `HttpURLConnection`'s refusal to follow a redirect that changes the protocol. Both
  are Java problems; .NET follows such a redirect, but check that once instead of
  trusting this line.
- Android's cleartext policy. On Windows a plain `http://` LAN address just works,
  which matters: the local serverhealthcheck instance is reached that way.
- The fixed slot count. `RemoteViews` cannot add views at runtime; a tray application
  can create as many icons as it likes, so the limit there is tray space.

### Testing recipes that paid off

- Keep a **fetch log** in the Android shape: page, final URL, candidate list, per
  candidate the byte count and the detected kind, then the result. Both bugs that
  survived until late were found by that log and by nothing else.
- A `--fetch-now` switch on the executable is the Windows equivalent of the Android
  trick (clear the app data, relaunch, watch the log): trigger a fetch without going
  through the UI.
- Serve crafted icons from `python3 -m http.server` and point a site at it. On
  Windows that is simply `http://localhost:8000/a`, no emulator address needed.
  Cover 1, 4, 8, 24 and 32 bit, a width that forces row padding, a 32 bit icon with
  an empty alpha channel, a compressed DIB and an entry past the end of the file.
- Compare pixels instead of impressions:
  `magick icon.png -format "%[pixel:p{0,0}]" info:` is what separates "decoded" from
  "broken".

### Features the Android version grew that Windows wants too

- Editing an existing entry, with the URL field doubling as the editor.
- A visible way to close the settings window, not only a menu item.
- A surface that shows all icons at a glance. Tray icons hide in the overflow by
  default, so a small always-on-top palette window may serve the goal better than a
  handful of `NotifyIcon`s.

## Milestones

1. Single site, hourly fetch, cache, click-to-open.
2. Offline grayed rendering.
3. Settings window + drag-and-drop onto it.
4. Second site (second `NotifyIcon`).
5. Refresh on network change, and retry with backoff while a site is failing.
6. Editing an existing URL in the settings window.
7. Decide the surface: tray icons only, or an always-on-top palette window.
8. Self-contained publish (`dotnet publish -r win-x64 --self-contained`),
   optional MSIX.
