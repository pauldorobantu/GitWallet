# Triplet x64-windows release-only — botan debug nu ne trebuie (app Release),
# iar buildul botan durează; vcpkg nu respecta VCPKG_RELEASE_ONLY in manifest.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_BUILD_TYPE release)
