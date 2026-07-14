# Display Session Contract v1

`msys.display-session.v1` is the live ready/state document published by every
MSYS X11 `display-output` provider. It is deliberately a small file contract:
the X server can become ready before an mIPC component channel exists, and
`msysd` can use the same file as its freshness-gated `x11-display` ready marker.

The provider must publish the document atomically only after all of these are
true:

1. its owned X server process is alive;
2. the configured `DISPLAY` answers `xdpyinfo`;
3. root geometry has been read from that live X server;
4. the provider's effective input mapping has been observed or derived from
   the exact configuration used by its input implementation.

`MSYS_X11_READY_FILE` is the provider-private readiness edge.
`MSYS_DISPLAY_SESSION_STATE_FILE`, or by default
`$MSYS_RUNTIME_DIR/display-session.json`, is the active role state consumed by
the window policy and HAL. A provider publishes the common state first and its
readiness file second. An adjacent owner token prevents an exiting old provider
from deleting a replacement provider's state. `msysd` additionally checks that
the readiness file modification time belongs to the current component
generation.
While running, the reference providers periodically re-probe and atomically
refresh the document, so a live HDMI mode change or input transform change does
not leave geometry/state frozen at boot.

Example:

```json
{
  "schema": "msys.display-session.v1",
  "state": "ready",
  "provider": "org.msys.openstick.ch347:x11-spi-touch-output",
  "generation": 7,
  "display": ":24",
  "geometry": {"width": 320, "height": 480, "depth": 24},
  "input_transform": {
    "enabled": true,
    "mode": "ch347-direct",
    "device": "CH347 XPT2046",
    "space": "normalized-display",
    "matrix": [1, 0, 0, 0, 1, 0, 0, 0, 1],
    "source": "ch347-direct-effective",
    "verified": true
  },
  "observed_at_unix_ms": 1780000000000
}
```

`display` is the effective X11 address, not a profile alias. `geometry` is the
current root-window pixel size and depth. The row-major 3x3 matrix maps
normalized provider input coordinates to normalized display coordinates. A
provider which owns no input reports `enabled: false` and `matrix: null`; it
must not invent an identity transform for hardware it does not manage.
Enabled matrices must be finite and invertible so navigation hit regions can
be projected back into provider input space without ambiguity.

Transform sources in the reference providers are:

- `ch347-direct-effective`: derived from the same swap/invert flags used by the
  CH347 direct mapper;
- `ch347-xtest-effective`: the same calibrated mapping after the optional
  native-XInput probe selected the existing x11display XTest mouse path;
- `xinput-coordinate-transformation-matrix`: queried from the selected live
  XInput device;
- `provider-effective-environment`: an explicit matrix applied by a board
  provider;
- `no-provider-owned-input`: output-only session such as the default HDMI
  provider.

The package-local publisher can probe or inspect state without D-Bus or a
service manager:

```sh
DISPLAY=:24 MSYS_DISPLAY_INPUT_MODE=ch347-direct \
  bin/msys-x11-policy --publish-display-session \
  /tmp/ch347_dirty_usb_x11/msys.ready \
  org.msys.openstick.ch347:x11-spi-touch-output

# On legacy hosts, the on-demand compatibility implementation remains:
python3 scripts/msys_display_session_state.py \
  --display :24 \
  --provider org.msys.openstick.ch347:x11-spi-touch-output \
  --input-mode ch347-direct \
  --state-file /tmp/ch347_dirty_usb_x11/msys.ready
```

Publishing fails closed when the live display, geometry, requested input
device, or transform cannot be verified. Display providers keep supervising
their X server after publication; loss of X11 causes the component to exit and
lets normal MSYS restart policy take over.

The CH347 wrapper keeps native UHID/libinput touch as its default. Setting
`MSYS_CH347_XTEST_FALLBACK=1` opts into a bounded `xinput` probe; when the
`CH347 XPT2046 Touchscreen` device does not appear and the XTEST extension is
available, the wrapper atomically switches the existing x11display touch-mode
file to `mouse`. This adds no systemd, D-Bus, udev configuration, or package
manager dependency and does not change existing deployments unless enabled.
The wrapper sources the same `CH347_TOUCH_CAL_FILE` before launching
x11display and publishing state, so swap/invert values in the document cannot
drift from the mapper actually receiving XPT2046 samples.
