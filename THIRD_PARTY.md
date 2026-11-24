# Third-Party Provenance & Licenses

- **heic-decoder** (`0.1.1`): https://github.com/Multipad-cyber/heic-decoder-rust, licensed under `MIT OR Apache-2.0`. Used as the pure-Rust HEIF/HEVC software decoding backend via Cargo (`Cargo.lock` pins exact versions).
- **rayon** (`1.x`) and transitive crates (`rayon-core`, `crossbeam-*`, `either`, `thiserror`): licensed under `MIT OR Apache-2.0` / `Apache-2.0`.
- **Adobe Photoshop C++ SDK** (`AdobeDocs/photoshop-cpp-sdk`): **Not bundled in this repository** in accordance with the Adobe SDK License Agreement (Sections 2 and 12). When building `heif-ps-plugin` from source, download the official SDK separately from https://github.com/AdobeDocs/photoshop-cpp-sdk and pass its path via `-PhotoshopSdkDir` or `PHOTOSHOP_SDK_DIR`.
