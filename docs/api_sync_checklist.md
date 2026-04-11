# API Sync Checklist

Use this checklist whenever you modify routes or payloads in `src/api_server.cpp`.

1. Routes match:
   - Confirm HTTP method and path match `server.on(...)` declarations.
2. Request schema matches parser logic:
   - Update required/optional JSON fields and validation rules.
3. Response contracts match implementation:
   - For each status code emitted in code, document response body shape.
4. Examples stay realistic:
   - Update example request/response payloads for changed behavior.
5. Error messages stay aligned:
   - Keep documented `message` strings in sync with `sendJson(...)` outputs.
6. Quick verification:
   - Trigger each endpoint once (`/`, `/network`, `/network/mode`, `/network/wifi`, `/play`, `/stop`, `/volume`, `/version`, `/status`, `/files`, `DELETE /files`, `/files/upload`, `/playlist/item`, `/playlist/queue`, `/playlist/start`, `/playlist/pause`) and verify docs still reflect observed responses.

## URL Playback Addendum

Run this addendum whenever URL playback behavior is changed.

1. Play request parity:
   - Confirm `/play` documents both `file` and `url` requests and optional `loop` for URL playback.
2. URL validation and errors:
   - Confirm URL validation errors in docs match implementation messages.
3. URL success contract:
   - Confirm URL play success responses document `url` and `url_loop_enabled` fields.
4. Status and files contract:
   - Confirm `/status` and `/files` response schemas include URL loop fields returned by firmware.
5. Loop timing behavior:
   - Confirm README and OpenAPI describe that URL loop restart uses the same pause setting as queued playback.