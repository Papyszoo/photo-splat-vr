# C1 model manager

Manages the licence gate, download or offline import, SHA-256 verification and
conversion for the two reconstruction backends:

| backend | source | licence | gate |
| --- | --- | --- | --- |
| `sharp` | `sharp_2572gikvuh.pt`, 2,809,738,232 bytes, pinned SHA-256 | ml-sharp `LICENSE_MODEL` | yes |
| `da2` | Depth Anything V2 Small `model.safetensors`, 99,173,660 bytes, pinned SHA-256 | Apache-2.0 | no, enabled by default |

The pinned constants are the existing ones in `core/model/checkpoint.h` and
`core/model/da2_profile.h`; `core/model/model_manager.h` only adds the URLs, the
file names, the GGUF architecture and the converter version. SHARP is opt-in
because its terms are research-only; the fallback needs no acceptance and its
Apache-2.0 notice belongs on the About screen.

## State machine

One persisted record per backend, `<state dir>/models/<backend>.json`:

```
license_required -> accepted -> downloading -> verifying -> converting -> ready
                        \            \             \            \
                         +------------+-------------+------------+--> failed (message)
```

- `license_required`: SHARP only, before an acceptance exists.
- `accepted`: gate passed, nothing downloaded yet.
- `downloading`: recorded with the current `.part` size. A restart rewinds this
  to `accepted` with a "download paused" note and keeps resuming from the
  `.part`.
- `verifying`: the pinned SHA-256 pass. A crash here is reported as failed;
  fetching again re-verifies the source or re-downloads.
- `converting`: the existing C1 converter (`convert_checkpoint` for SHARP,
  `convert_depth_anything_v2` for DA2) writes `<models>/<backend>.gguf.part`,
  which is renamed only after the metadata check passes.
- `ready`: the GGUF is present and its metadata matches the pin.
- `failed`: always carries a message. Fetching again is allowed and retries
  whatever is missing.

`status()` revalidates a `ready` record on every load by reading the GGUF header
(no tensor allocation): `general.architecture`, `<backend>.backend`,
`<backend>.source_sha256` and `<backend>.converter_version`. A missing,
truncated, edited or stale file demotes the record to `failed` with the reason
instead of letting a later backend load fail in a less obvious place.

## Files under the state directory

```
licenses.json              licence acceptances (core/model/license.*)
models/<backend>.json      persisted state record
models/<backend>.gguf      converted runtime file (a labelled derivative)
models/<backend>.gguf.part temporary output of a running conversion
downloads/<filename>       verified source checkpoint
downloads/<filename>.part  resumable partial download
inbox/                     offline drop folder (FTP/USB)
```

The state directory is `/data/<app>/state` on the console and `--state-dir`,
`$SPLAT_STATE_DIR` or `./splat-state` for the CLI. It is the only directory the
manager writes to.

## Licence gate

Spec rules 3 and 5: the licence text is **not bundled**. `fetch_license_text()`
downloads `LICENSE_MODEL` from
`https://raw.githubusercontent.com/apple-aiml-research/ml-sharp/main/LICENSE_MODEL`,
the UI shows it, and `accept_license()` stores the SHA-256 of exactly that text
in `licenses.json` (the existing `core/model/license.*` store) together with the
timestamp. `license_accepted()` fails closed: a missing or malformed
`licenses.json` means nothing has been accepted.

`fetch()` refuses to touch the network, and `import()` refuses to convert,
until the acceptance exists. The acceptance is per backend; a changed licence
text invalidates it (compare the stored hash with the text that is shown again).

The offline path therefore needs the licence text once, from the network: the
acceptance cannot be recorded on a console that has never had internet access.
The fallback is unaffected.

## Download, resume, verify

1. The manager reuses `downloads/<filename>` when it still matches the pinned
   size and hash (for example after a conversion failure), and deletes a stale
   copy otherwise.
2. Otherwise it checks the inbox: `<inbox>/<official filename>`. The file name
   must be exact; size and SHA-256 are then checked. A correct file is moved
   into `downloads/`; a wrong hash leaves the user's inbox file untouched and
   records the failure.
3. Otherwise it sends HEAD, compares `Content-Length` with the pinned size, and
   downloads to `<filename>.part`, asking for `Range: bytes=<part size>-` when
   a partial file exists. The fetcher appends on 206; a server that ignores the
   range (200) is handled by truncating and starting over.
4. A failed or cancelled transfer keeps the `.part` for the next call. A size
   or SHA-256 mismatch deletes the `.part`, because a corrupt prefix must never
   be resumed.
5. A verified download is renamed to `downloads/<filename>` and converted.

Verification is `sha256_file()` from `core/model/sha256.*`, streamed in 1 MiB
chunks, so the 2.8 GB checkpoint is never held in memory. Conversion reuses the
existing streaming converter and its own pinned-hash check (spec rule 2).

## HTTP interface

`core/model/http_fetch.h` is the whole network seam:

```cpp
struct HttpResource { uint64_t content_length; bool range_supported; };
struct FetchRequest {
    std::string url;
    Path destination;
    uint64_t offset;        // resume point
    uint64_t total_bytes;   // progress only
    Progress* progress;
    std::function<bool()> cancelled;
};
class IHttpFetcher {
    virtual Result<HttpResource> head(const std::string& url) = 0;
    virtual Status download(const FetchRequest&) = 0;
    virtual Result<std::string> get_text(const std::string& url, size_t max_bytes) = 0;
};
```

`platform/desktop/curl_http_fetcher.*` implements it on the system libcurl
(`find_package(CURL)`); the PS5 gets its own implementation later. The PS5 build
must not include the desktop file. `splat_http_curl` is the CMake target that
wraps it; it is defined in `tools/model_cli/CMakeLists.txt` so this CLI builds
on hosts without Vulkan. The desktop app can link the same target.

## CLI

```sh
splat_model [--state-dir DIR] status [backend]   # persisted state, all backends by default
splat_model [--state-dir DIR] accept <backend>   # fetch + print the licence, then record it
splat_model [--state-dir DIR] fetch <backend>    # resume/pick up, verify, convert
splat_model [--state-dir DIR] import <file>      # verify + convert a file the user already has
```

`fetch` prints 10% progress lines on stderr. `import` infers the backend from
the official file name and copies the file (it does not delete the user's copy).
Exit status is 0 on success, 1 on a model/manager error and 2 on usage.

## Tests

`tests/model_manager_test.cpp` (11 cases, 130 assertions) starts a local
`cpp-httplib` server with HEAD, `Range`/206/`Content-Range` and a licence
endpoint, and drives the manager with a test fetcher and a test converter that
writes a tiny GGUF with the expected metadata. Covered:

- the licence gate blocks both `fetch` and `import` before acceptance, and
  acceptance stores the hash of the shown text;
- an interrupted download resumes with `Range: bytes=1200-` and reaches `ready`;
  a complete `.part` left by a crash is verified without the network;
- a wrong hash is rejected and the `.part` is removed;
- inbox pickup by exact file name and hash, wrong name ignored, wrong hash
  rejected with the user's file left in place;
- `ready` survives a restart, a tampered GGUF is demoted to `failed`, and a
  stale converter version is rejected;
- transient states from a crash (`downloading`, `converting`) are reconciled;
- `import` requires the official file name and does not move the user's file.

`tests/model_manager_net_test.cpp` is built as `splat_model_net_tests` because
it needs the libcurl fetcher. Four cases always run against a local server and
cover the desktop fetcher itself: HEAD, a resumed ranged GET, a server that
ignores `Range` (the file is truncated rather than doubled), and the bounded
text GET. The fifth is the real-network check, skipped unless
`SPLAT_NET_TESTS=1`:

```sh
SPLAT_NET_TESTS=1 ctest -R model_manager_net --output-on-failure
```

Measured 2026-10-26: HEAD on the official SHARP URL returned the pinned
2,809,738,232 bytes with `Accept-Ranges: bytes`. Both `fetch da2` and
`import sharp_2572gikvuh.pt` were also run end to end with the real libcurl
fetcher, the pinned hash check and the real converter, and reached `ready`.
The full suite was 160 cases in `splat_tests` plus 6 doctest cases in
`splat_model_net_tests`, all passing.

