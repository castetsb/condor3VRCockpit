# Third-party notices

This project is MIT-licensed (see [LICENSE](LICENSE)). It uses the components below. None of
them is stored in this repository: they are downloaded at build time, or installed by the user.

## Downloaded at build time (compiled into the programs)

| Component | Used by | License |
|---|---|---|
| [nlohmann/json](https://github.com/nlohmann/json) 3.11.3 | `condorhands` (profiles) | MIT, © 2013-2023 Niels Lohmann |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) headers 1.1.65 | `layer` (not used by `condorhands`) | Apache-2.0 OR MIT, © The Khronos Group Inc. |

Both licenses allow use and redistribution in open-source and binary form. If you
distribute binaries, include the copyright notices above.

## Installed by the user, loaded at run time (not distributed)

| Component | Notes |
|---|---|
| [vJoy](https://github.com/njz3/vJoy) | Virtual joystick driver. `vJoyInterface.dll` is loaded from the user's vJoy installation. |
| Meta Horizon Link runtime | Loaded through Condor's own `LibOVR.dll`. No Meta SDK file or header is included in this repository: the few data layouts used are declared in this project's own code. |
| Condor 3 | Not modified. Only its `LibOVR.dll` is loaded, from the user's installation. |
| Windows speech (SAPI) | Part of Windows. |

## Note on the Meta runtime

`condorhands` calls hand-tracking functions that the Meta runtime exports but does not
document. They were identified by observing the runtime's export table and behaviour, to make
this program work with Condor (interoperability). This project is not affiliated with or
endorsed by Meta. A runtime update could change or remove these functions.

## Trademarks

"Condor" is a trademark of its owners. "Meta" and "Quest" are trademarks of Meta Platforms,
Inc. They are used only to describe compatibility.
