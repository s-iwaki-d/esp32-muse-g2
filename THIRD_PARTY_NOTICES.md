# Third-party notices

The firmware uses [Muse Gadget SDK](https://github.com/facebookincubator/muse-gadget-sdk),
Copyright (c) Meta Platforms, Inc. and affiliates, under the Apache License 2.0.
The pinned SDK revision is recorded in [upstream.json](upstream.json); a copy of its
license is included in [LICENSES/muse-gadget-sdk-Apache-2.0.txt](LICENSES/muse-gadget-sdk-Apache-2.0.txt).

`firmware/patches/upstream.patch` and `firmware/display/ui.patch` contain changes to
that SDK and include context from its source. They add the Even G2 bridge,
adapt StopWatch provisioning and memory use, disable local audio, and retain the
Muse character alongside connection status. `scripts/prepare_firmware.py` applies
these patches to a separate SDK checkout while preserving upstream copyright
and license notices.

The SDK checkout, character assets, downloaded dependencies and compiled firmware
are not committed here. Their own licenses and notices remain in the materialized
SDK and dependency trees. Dependency versions are recorded in `firmware/dependencies.lock`
and `cloudflare/package-lock.json`.

This notice documents third-party licensing only; it does not grant a new license
for this repository's original code.
