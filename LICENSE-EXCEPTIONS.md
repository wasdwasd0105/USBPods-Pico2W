# Additional permission under GNU GPL version 3 section 7

The USBPods firmware links against third-party components whose licenses
are not GPL-compatible (most notably the Fraunhofer FDK AAC Codec Library
and BlueKitchen BTstack). A plain GPL-3.0 grant would therefore forbid
distributing the very binaries this project exists to produce. As the sole
copyright holder of the GPL-covered code, the author grants the following
additional permission, as allowed by section 7 of the GNU GPL version 3:

As a special exception, you have permission to combine this Program with:

* the **Raspberry Pi Pico SDK** (BSD-3-Clause) and the libraries it
  bundles, including **BlueKitchen BTstack** (distributed with the Pico
  SDK under BlueKitchen's terms for use with Raspberry Pi silicon),
* the **Fraunhofer FDK AAC Codec Library for Android**
  (`3rd-party/fdk-aac`),
* **Sony libldac** (Apache-2.0, `3rd-party/ldacBT`),
* the **LHDC V5 encoder crate** (Apache-2.0, `3rd-party/lhdcv5`),
* **TinyUSB** (MIT) and the Rust `core`/`compiler-builtins` libraries,

and to convey the resulting combined work (for example a `.uf2` firmware
image), without those components themselves becoming subject to the GPL —
provided that you comply with each component's own license terms, and with
the GNU GPL version 3 for all portions of the work that it covers.

If you modify this Program, you may extend this exception to your modified
version, but you are not obligated to do so. If you do not, remove this
file and the exception statements from your version.
