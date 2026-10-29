# Hand tracking through the Meta PC runtime (LibOVR)

These notes describe what `condorhands` relies on. They were observed with the 32-bit Meta
Horizon Link runtime (2026-10), and come from the runtime's export table and from tests in
the headset. Meta does not document any of this, so it may change with a runtime update.

## Hands or controllers

`ovr_GetConnectedControllerTypes` tells which input the headset is using:

| Value    | Mode                                                        |
|----------|-------------------------------------------------------------|
| `0x3000` | hand tracking                                               |
| `0x0003` | Touch controllers (left + right)                            |

The Quest tracks one or the other, never both at the same time. When a controller is used,
hand tracking stops for both hands.

In controller mode, the public `ovr_GetTrackingState` (hand poses) and `ovr_GetInputState`
(grip and trigger values) work from an invisible session.

## Hand tracking functions

`LibOVRRT32_1.dll` forwards these exports to `LibOVRRTImpl32_1.dll`. They only work over Link.

| Export | Parameters (cdecl) | Result |
|---|---|---|
| `ovr_GetCurrentHandInputState` | `(session, int hand, void* out)` | **Works** from an invisible session. Used by `condorhands`. |
| `ovr_GetHandSkeleton` | `(session, int hand, void* out)` | Works. `out[0]` must be `0xDF000001`. Writes 0x53C bytes (24 bones, 19 capsules). |
| `ovr_GetHandPose` | `(session, int hand, double absTime, void* out)` | `out[0]` must be `0xDF000001`. Joints are only filled for the app in front (Condor); other sessions get zeros. |
| `ovr_ShowAvatarHands` | `(?, bool)` | Refused: "Must be called by a Hybrid App". |

`hand`: 0 = left, 1 = right.

## `ovr_GetCurrentHandInputState` output (52 bytes)

| Word | Content |
|---|---|
| 0 | unknown (0) |
| 1–4 | pinch strength: index, middle, ring, pinky (0..1) |
| 5–8 | orientation quaternion x, y, z, w |
| 9–11 | position in metres, in the session's tracking origin |
| 12 | status flags, see below |

Return value: `0` = known, `1002` = not tracked, negative = error.

| Flag | Meaning |
|---|---|
| `0x02` | data valid (hand currently seen) |
| `0x40` | palm facing the user (Meta system gesture; a pinch here opens the Meta menu) |
| `0x80` | right hand |

When the runtime loses a hand, it keeps returning the last frame for about 15 seconds,
with neither `0x02` nor `0x40` set. `condorhands` treats those frames as "not tracked".

## Tools

`tools/ovrprobe` holds the two probes used to find this out:
* `ovrprobe` reads the public tracking and input state;
* `handprobe` dumps the hand functions' raw output.
