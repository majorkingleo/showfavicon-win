# Plan 3 — Android

## Goal

A persistent presence per site. The main surface is a home-screen **App Widget**:
one icon per site, tap → open browser, hourly refresh, grayed icon when offline.
A persistent notification can carry the same presence into the status bar later.
Configuration is **not** drag-and-drop — a settings screen with a list of URLs is
enough.

## Progress

Milestones 1–4 are implemented and the project **builds**: `assembleDebug` and
`lintDebug` are green, lint reports no findings. Measured on CachyOS with AGP
9.4.1, Gradle 9.8.0, JDK 21, compiling against API 37.2.

What the first real build revealed — each of these cost a build cycle:

- AGP 9 has **built-in Kotlin**: the `org.jetbrains.kotlin.android` plugin is
  gone, and `jvmTarget` follows `compileOptions.targetCompatibility`.
- `androidx.core` 1.19.1 forces `compileSdk = 37` **plus** `compileSdkMinor = 2`;
  the SDK package for that is `platforms;android-37.2`, and `platforms;android-37`
  does not exist.
- The Gradle build cache can hand back a stale resource merge after a resource
  folder is renamed, which surfaces as `AAPT: resource mipmap/ic_launcher not
  found`. `--no-build-cache` clears it.

The build details live in `.github/skills/android-build/SKILL.md`.

The first *real* icon run then surfaced a second class of problem. `github.com`
fetched fine while `serverhealthcheck.borger.co.at` stayed on the placeholder, and
the cause was not the icon format at all: a configured site URL has no path
(`https://host`), so `URI.resolve` glued the relative href onto the authority —
`favicon.php?ts=…` became `https://hostfavicon.php`, an `UnknownHostException`.
GitHub's page uses absolute hrefs, which is exactly why the bug stayed hidden.
Two more of the same kind followed: a redirect that changes the protocol is not
followed by `HttpURLConnection` at all, and a 32 bit ICO whose alpha channel is
empty has to be made opaque before its AND mask can mean anything. All of it is
measured and fixed under "Decoding the icon bytes".

- [x] **Milestone 0 — toolchain.** `scripts/install-android-toolchain.sh`, with
      the SDK package names verified against Google's repository XML.
- [x] **Milestone 1 — widget and settings.** `FaviconWidgetProvider`,
      `MainActivity`, `SiteAdapter`, layouts, adaptive launcher icon.
- [x] **Milestone 2 — fetch and cache.** `FaviconWorker` (hourly WorkManager
      job), `FaviconFetcher`, `FaviconResolver`, `FaviconStore`.
- [x] **Milestone 3 — grayed icon.** Desaturation through a `ColorMatrix`,
      driven by the failed-fetch flag in the store.
- [x] **Milestone 4 — multi-site.** One widget shows up to four icons (one slot
      per site; `RemoteViews` cannot add views at runtime), fed by the settings
      list. The per-instance `android:configure` path was dropped by decision:
      the widget always shows the first four sites and the settings list is the
      single place to edit them.
- [ ] **Milestone 5 — notification.** Optional, and not started.
- [ ] **Milestone 6 — share intent.** Not started.
- [ ] **Milestone 7 — AAB release.** Not started.

## Toolchain (Linux / CachyOS)

Target workstation: Arch-based, `pacman` plus an AUR helper (`paru`/`yay`).

What is needed:

- **JDK 21 (LTS)** — `jdk21-openjdk`. Selection with
  `sudo archlinux-java set java-21-openjdk`.
- **adb / fastboot** — `android-tools`.
- **udev rules** — `android-udev`, so device access works without root.
- **Android SDK** — either through Android Studio's SDK Manager, or headless
  through the `cmdline-tools`.
- **Kotlin and Gradle are not installed separately**: AGP 9 compiles Kotlin
  itself (built-in Kotlin, no `kotlin-android` plugin), and Gradle comes through
  the wrapper (`gradlew`).
- Runtime dependencies (WorkManager, AndroidX, foreground-service APIs) are
  Gradle libraries, not installs.

**Caveat:** the machine also carries **JDK 25** (`jdk25-openjdk`), which this
AGP/Gradle pair does not support. Switch the system default
(`sudo archlinux-java set java-21-openjdk`) and/or pass
`JAVA_HOME=/usr/lib/jvm/java-21-openjdk`. The commands below and the VS Code task
all pass it explicitly, so the build does not depend on the shell default.

Option A — Android Studio (recommended):

```fish
sudo pacman -S jdk21-openjdk android-tools android-udev
sudo archlinux-java set java-21-openjdk
paru -S android-studio
```

`android-studio` is AUR-only in this setup; JetBrains Toolbox is the equivalent
alternative for self-updating installs. SDK Manager packages:
`platforms;android-37.2`, `build-tools;36.0.0`, `platform-tools`, `emulator`,
`cmdline-tools;latest`, `system-images;android-36;google_apis;x86_64` (the
emulator image API level is independent of compileSdk).

Option B — headless, driven from VS Code:

```fish
sudo pacman -S jdk21-openjdk android-tools android-udev curl unzip
sudo archlinux-java set java-21-openjdk
```

Then the SDK itself, per user and without root: unpack the official
`commandlinetools-linux-*_latest.zip` into
`$ANDROID_HOME/cmdline-tools/latest`, accept the licenses, install the packages.

```fish
sdkmanager --licenses
sdkmanager "platform-tools" "platforms;android-37.2" "build-tools;36.0.0" \
  "emulator" "system-images;android-36;google_apis;x86_64"
```

`scripts/install-android-toolchain.sh` performs exactly that flow and is
idempotent: repo packages, JDK 21 as default, udev rules plus group membership,
command line tools, licenses, SDK packages, emulator, and `ANDROID_HOME` in
`~/.config/environment.d/`. `--with-studio` adds option A, `--dry-run` only
prints the steps.

VS Code extensions: `vscjava.vscode-java-pack` (language support, debugger),
`vscjava.vscode-gradle` (Gradle tasks and sync), `fwcd.kotlin` (Kotlin support),
optionally `adelphes.android-dev-ext` (logcat and debugging).

Emulator prerequisites: `/dev/kvm` present (VT-x/AMD-V enabled in firmware,
`kvm_amd`/`kvm_intel` loaded) and the user in the `kvm` group. Without it, use a
physical device over USB debugging.

Release signing: `keytool` ships with the JDK — no extra tool for the AAB
release milestone.

## Architecture

- **Kotlin + Jetpack**, min SDK 26+.
- **App widget (primary surface)** — an `AppWidgetProvider` with `RemoteViews`
  (or Jetpack **Glance**), one icon per configured site. Each icon is a
  `RemoteViews` child with its own `setOnClickPendingIntent()` to an
  `ACTION_VIEW` intent; updates go through
  `AppWidgetManager.updateAppWidget()`.
- **Widget configuration** — `android:configure` on the widget points at the
  settings `Activity`, so placing the widget on the home screen opens the URL
  list directly. The configure path returns `RESULT_OK` with the widget id as an
  extra, which also allows per-instance site lists.
- Fetching: **WorkManager periodic work** (hourly; its 15-minute minimum is no
  issue) — it survives reboots and triggers the widget refresh. No long-running
  process needed.
- Cache: app internal storage `filesDir/showfavicon/<host>.png`.
- Graying: render a `Bitmap` through a `ColorMatrix` (saturation 0 plus reduced
  alpha) when the last fetch failed.
- **Optional: foreground service with a persistent notification** — carries the
  status bar presence. Not needed for the widget, and the only part that
  requires `POST_NOTIFICATIONS` and the `FOREGROUND_SERVICE_*` permissions.

## Multi-site

Two widget shapes, fed by the same settings list:

- **One widget with N icons** — a row/grid of `RemoteViews` children, one per
  configured site (recommended: one list drives everything).
- **One widget instance per site** — the instance shows a single icon; the
  configure activity stores which URL that widget id shows.

Layout caveat: keep `minWidth` / `minHeight` small enough that a single 1×1
cell is a legal widget size for the one-icon case.

## Configuration

- A plain settings **Activity**: a list of URLs with add, edit and remove, plus
  validation, persisted in `SharedPreferences`. **No drag-and-drop required.**
  Editing reuses the single URL field: tapping a row loads that site, the button
  becomes *Save* and the row stays highlighted, so a typo costs one tap instead of
  removing the site and adding it again at the end of the list. Tapping the same
  row leaves the mode again.
- **Not** a widget configuration activity: the widget always shows the first four
  sites, so `android:configure` was dropped by decision (see milestone 4).
- Optional convenience: a "share from browser → ShowFavicon" intent to append a
  URL without opening the app.
- Permissions: `INTERNET`. Only the optional notification path adds
  `POST_NOTIFICATIONS` runtime permission (Android 13+) and
  `FOREGROUND_SERVICE` plus `FOREGROUND_SERVICE_SPECIAL_USE`/`DATA_SYNC`.
- Note: modern Android renders status-bar small icons as **monochrome** masks —
  the full-color favicon appears in the notification's large icon / expanded
  view. On the home-screen widget full colour works, so the widget is the better
  place for the real favicon.

## Decoding the icon bytes

Two different things went wrong on the first real site, and only one of them was
about the icon format.

**The href, not the icon.** `https://serverhealthcheck.borger.co.at/` announces its
icon as a *relative* reference, `favicon.php?ts=…`. A configured site URL carries
no path of its own (`https://host`), and against such a base `URI.resolve` does not
insert the root: the reference lands in the authority and the result is
`https://hostfavicon.php`, which fails with `UnknownHostException`. The fallback
`/favicon.ico` then answered 404, so the whole fetch failed and the widget drew the
placeholder — with the network up and github.com succeeding in the same run.
Absolute hrefs (`https://github.com/fluidicon.png`) never trigger this, which is why
it stayed hidden. Fixed by rooting the base before resolving
(`FaviconResolver.resolve`); measured after the fix:

    candidates for https://serverhealthcheck.borger.co.at:
      [/favicon.php?ts=…, /favicon.ico]
    candidate …/favicon.php?ts=… -> 252 bytes, svg
    candidate …/favicon.php?ts=… -> icon 6255        # 192×192 PNG in the cache

**The hop, not the icon.** `HttpURLConnection` refuses a redirect that changes the
protocol, and sites do exactly that. `https://www.slackware.com/favicon.ico`
answers 301 to `http://…`, and a page the user typed as `http://host` answers 301 to
https. Both used to end in "no icon", with the first response logged as a bare
`HTTP 301`. The fetcher now follows up to five hops by hand and resolves each
`Location` with the same helper as an icon href:

    http://www.postgresql.org -> HTTP 301 to https://www.postgresql.org/
    http://www.postgresql.org/favicon.ico -> HTTP 301 to https://…/favicon.ico
    candidate http://www.postgresql.org/favicon.ico -> icon 1500

**Then the format.** `BitmapFactory` decides what can be displayed, and it covers
raster formats only (PNG, JPEG, WebP, GIF, BMP). Two gaps were left:

- **SVG** — the site above offers nothing but an `image/svg+xml` icon.
- **ICO** — `BitmapFactory` cannot decode `.ico` at all, so a site that offers only
  `/favicon.ico` fails the same way, however reachable it is.

Both are closed. `SvgRasterizer` renders vectors through AndroidSVG onto a
192×192 canvas, and `IcoDecoder` reads the container — the largest entry, an
embedded PNG passed straight through or a DIB turned into a bitmap — so the store
stays a plain PNG store and nothing downstream knows about either format.

What was verified, and how:

| Case | Evidence | Result |
| --- | --- | --- |
| SVG only | `serverhealthcheck.borger.co.at` | 252 bytes in, 6255 byte 192×192 PNG out |
| ICO, 32 bit with real alpha | `www.debian.org`, 4 entries, picks 32×32 | `#c00040` swirl on a transparent background |
| ICO, 8 bit palette plus mask | `www.postgresql.org`, 3 entries, picks 48×48 | Postgres blue `srgba(63,95,159,1)`, transparent corner |
| ICO with an embedded PNG | `www.netbsd.org` | 2887 of 2909 bytes, passed through |
| Directory field inconsistent | `www.slackware.com` says 0 bit in the directory, the DIB header is valid | decoded from the DIB header |
| 1, 4 and 24 bit, padded rows | crafted files, local server | alternating red/blue and a transparent bottom row, pixel by pixel |
| 32 bit with an empty alpha channel | crafted file, transparency in the AND mask | invisible before the fix, correct pattern after it |
| Compressed DIB, entry past the end | crafted files | rejected, site marked failed, worker carries on, no crash |

A decoded icon has to be looked at: pulling it and probing pixels with
`magick … "%[pixel:p{x,y}]"` is what separates "decoded" from "placeholder". The
crafted files come from a generator plus a local `python3 -m http.server` that the
emulator reaches as `10.0.2.2`; the recipe is in the build skill.

Relative icon hrefs are resolved against the URL that finally answered, not against
the one that was requested. The local instance shows why that is more than a
detail: the page is entered as `/serverhealthcheck/www`, which answers 301 to
`/www/` and then 302 to `login.php`, and its `<link rel="icon" href="favicon.php">`
exists only inside `/www/`. Resolved against the requested URL the candidate was
`/serverhealthcheck/favicon.php` — 404, while the page itself answered fine. The
icon stayed gray, and the network got blamed for it. Measured after the fix:

    page …/www -> redirected to …/www/login.php
    candidates for …/www/login.php: […/www/favicon.php, /favicon.ico]
    …/www/favicon.php -> 252 bytes, svg -> icon 6240

## Reacting to the network

A site that cannot be reached is what grays an icon out, and the reason is usually
the network the phone is on: a Wi-Fi that was just joined, or an address that only
exists inside the LAN. Three things make the icon come back on its own:

- The work requires a network (`NetworkType.CONNECTED`), so a fetch that cannot
  work is not spent.
- A run that leaves a site failing returns `Result.retry()`, with a linear backoff
  starting at 30 s and a cap of six attempts per run. WorkManager holds that retry
  back until the network is there again, which covers a device that had none at all.
- `ShowFaviconApp` registers a default network callback. A constraint cannot see a
  change from one network to another, and that is exactly the case here: when the
  device switches networks, one refresh is queued immediately.

Measured on the emulator: airplane mode off → `default network became available`,
and a fetch ~100 ms later. A dead port produced `Worker result RETRY` with the next
attempt 30 s out. The periodic work is enqueued with
`ExistingPeriodicWorkPolicy.UPDATE` rather than `KEEP` — with `KEEP`, an install
that already had the work would keep the constraints of the older version, which is
the kind of change nobody notices for a week.

## Milestones

0. Toolchain installed, empty project builds and runs on device/emulator.
1. Widget skeleton + settings activity: one URL, tap-to-open, icon rendered.
2. WorkManager hourly fetch + PNG cache + widget refresh.
3. Offline grayed icon.
4. Multiple URLs in the settings list, widget with one icon per site, widget
   configure path for per-instance sites.
5. Optional: foreground service + persistent notification.
6. Share-intent add.
7. AAB release build.
