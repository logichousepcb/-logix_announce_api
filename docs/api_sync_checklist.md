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