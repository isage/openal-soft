# openal-soft-vita

This repo contains patchsets for vita [openal-soft](https://openal-soft.org/) port.

Format is openal-soft-<openal_version>-vita-<patchset_version>.patch

For full source see vita-* branches


# building

```
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=${VITASDK}/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release ..
make install
```
