# vip_duckspeed (Plugify)

[VIP] DuckSpeed: a per-frame timer writes the group's 'duckspeed' value into m_flDuckSpeed of every alive VIP holding the duck button (feature 'duckspeed', VIP_FLOAT, HIDE). Ported from the Pisex VIP core module VIP_DuckSpeed (vip_duckspeed). Requires the vip core.

Plugify port (fork) of **VIP_DuckSpeed ([VIP] DuckSpeed)** by ABKAM (Metamod (C++)).
Original: https://github.com/Pisex/cs2-vip-modules
The code was rewritten for Plugify / s2sdk; behaviour follows the original.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugify dependencies: s2sdk, vip, hudmenu (optional)

## Installation
Download `vip_duckspeed-1.0.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/vip_duckspeed`.

`plugins/vip` carry only the API headers of the plugins this one uses.

## License
GPL-3.0, see LICENSE.

