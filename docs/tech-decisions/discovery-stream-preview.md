# Discovery stream preview

2026-10-08. Qt displays the VMS RTSP address beside a discovered device before ADD CAMERA.

## Decision and constraints

Extend version-1 DISCOVER_CAMERAS with cameraId, rtspUri, registered, ready. CameraService reserves an identity per ONVIF service URL on the control thread; discovery does not start ingest or expose upstream URLs. Registration keeps authenticated media queries, reuses the identity and starts ingest. REGISTER_CAMERA also returns rtspUri. A reserved URI is not a working stream; readiness remains explicit.

Reservations are bounded to 64 per process; registration remains limited to 16 cameras. Existing/configured identities take precedence and new CAMxx IDs avoid all reservations. Reservations are not persisted across restarts.

## Alternatives

- Auto-register during discovery: starts ingest before ADD/credentials/profile selection.
- Return upstream URI: differs from the requested managed playback address and exposes camera connection information.
- Guess URI in Qt: can diverge from ID/public host/port configuration.
- Wait for registration: does not provide the requested discovery→preview→ADD flow.

## Evidence and limits

discovery_registration_integration.py checks local SOAP/RTSP fixtures: preview managed URI, no RTSP connection before ADD, matching registration ID/URI, ready REST response, and refreshed discovery state. Qt selection/status tests are in its repository. Fields are additive for existing clients; older servers need rebuilding for previews. Windows/Pi integration remains to be checked.
