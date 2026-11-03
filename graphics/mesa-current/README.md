# Current Mesa candidate for Lumia 730

This directory is **inactive** in the product. `SUPERMAN_MESA26_QUALIFIED` is
unset. Do not enable it simply to obtain a newer version string.

Pinned Mesa 26.2.5 and libdrm 2.4.134 are built for the Android 11 ARMv7 ABI.
`upstream/` contains the original archives, `patches/` the complete local source
changes, and `INPUTS.sha256` covers sources, tool inputs, patches, and prebuilts.
No Android linker stub library is shipped. Hardware exposes GLES 3.0. Turnip
Vulkan does not support this Adreno generation.

The first candidate (P150) passed native pixels, RenderEngine, and launcher
checks but failed physical gameplay with tearing and constant flashing. It was
rolled back and must not be called qualified.

P151 adds the shared backing-buffer ordering correction missing from the
upgrade. On the same boot the P149 working driver passed the alias hazard test,
P150 failed four categories with millions of incorrect pixels, and P151 passed
all categories with zero errors. P151 also passes the six native cases and all
60 RenderEngine tests. Human gameplay was subsequently rated worse, and its
capture averaged 50.19 successful display updates/sec. P151 was rejected and
rolled back to exact P149. Different scenes prevent a controlled speed comparison.
These checks establish the reproduced alias correctness fix only; this candidate
must not be called a smooth gameplay or performance improvement.

Rebuild using existing Android platform libraries:

```
python3 device/nokia/superman/graphics/mesa-current/rebuild.py --verify-only
python3 device/nokia/superman/graphics/mesa-current/rebuild.py --build-dir out/superman-mesa-current
```

The recipe builds with Ninja `-j20`, preserves candidate output outside the
product, and never deploys or creates an image. Python Mako and PyYAML are host
prerequisites. Bison, Meson, packaging, and upstream Meson fallback sources are
pinned locally. Only non-runtime debug data is stripped; symbol tables remain.
A fresh build may require paths to built Android platform libraries in `out/`.

Future product selection must disable the legacy Mesa Android.mk **and** remove
`external/mesa3d` from `PRODUCT_SOONG_NAMESPACES`; the legacy makefile rejects
being exported while customized Mesa is selected. Include every dependency in
`runtime-paths.json` and regenerate startup/component hashes from the newly
qualified binaries. Retain the native scanout HALs and their linear modifier
contract; downstream Qualcomm ION/C2D configuration is incompatible with this
mainline DRM kernel.

Evidence: `.codex-restart/artifacts/p150-mesa-current-upgrade-20261028` and
`.codex-restart/artifacts/p151-mesa26-presentation-20261029`. Deployment and human
feedback are separate from source/build validation in those records.

Public Git checkpoints retain the recipes, source URLs, digests and patches.
Downloaded `upstream/` archives, host tools and compiled `prebuilt/` libraries
are local inputs excluded from Git. Restore them from the recorded provenance
before running the recipe; the public checkout alone is not a packaged payload.
