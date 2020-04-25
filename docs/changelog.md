###### The Lithe Project&trade; Development Team

## Changelog

#### v0.0.2 - Pre-Alpha Stage 2
> Stage 2 of Pre-Alpha is the stage of letting more people know about Lithe. No main chain is still yet to be
  active. Everything is currently running using `--testnet` within `lithe-daemon` and `lithe-wallet`.

- 

#### v0.0.1 - Pre-Alpha
- Forked Conceal at [this commit](https://github.com/ConcealNetwork/conceal-core/commit/6c07dbd781deed8d6e49fe98abac5e8175650321).
- Made the necessary changes to disconnect it from Conceals network to its own independent network ready.
- Modified coin economics.
- In `external/`, gtest has been disabled unless testing.
- Added clarifying colours to the core daemon.
- Added file names to daemon info, errors and warnings so we can track easier.
- Removed hardforks after `MAJOR_BLOCK_VERSION_3`
- Fixed some more forks.
- Removed Zawys old difficulty algorithm.
- Removed premine code.
- Minor upgrades to `lithe-wallet`.
- Added args `--enable-cors` + `--enable-blockexplorer` to `lithe-daemon`.
- Added new command within `lithe-daemon`. `status` will now show blockchain information **(WIP)**.
- Fixed Testnet as previous declaration didn't fork past `major_version: 2`.
  - **@NOTE:** This could be a fix for *all* other CryptoNote based coins. 
- Updated Copyrights.
- Add AppVeyor build status and binary distribution.
- Chacha8 cleanup.
- Better logging in some areas.
- Daemon now generates and shows you a peer id.
- Better UI for `lithe-wallet`.
- More code cleanup and fixes
- Add validate password `lithe-wallet`
