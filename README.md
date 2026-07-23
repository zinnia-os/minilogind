# minilogind
Small logind stub that provides a login1 D-Bus interface

Run a system-wide `seatd` daemon so it can arbitrate between direct libseat
clients and minilogind.

If seatd is unavailable, `TakeDevice` falls back to
opening device nodes directly.

`LIBSEAT_BACKEND` is pinned to `seatd` by default so libseat cannot recurse
into its own logind backend.

## What it's not

- User switcher
- sd-login replacement

### Dependencies

- GCC 15 or Clang 19
- gio-unix-2.0
- libseat
- seatd
- libudev (optional)
