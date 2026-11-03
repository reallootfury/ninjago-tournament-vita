# Third-party components

The project's original code and project-specific modifications are offered
under LGPL-3.0-or-later; see `LICENSE` and `COPYING`. Third-party code keeps its
existing licenses and copyright notices. This does not relicense upstream
code or game/artwork assets. Required inherited MIT notices are preserved in
`licenses/upstream-MIT.txt`, not used as the overall project license.

This project is based on the MIT-licensed Android `.so` loader lineage by Andy
Nguyen, Rinnegatamante, GrapheneCt, and Volodymyr Atamanenko. Copyright notices
remain in the individual source files.

- FalsoJNI: MIT; see `lib/falso_jni/LICENSE`
- so_util and inherited loader compatibility code: MIT; see
  `licenses/upstream-MIT.txt` and source copyright notices
- Android JNI/OpenSL ES portions: Apache-2.0; see `licenses/Apache-2.0.txt`
- OpenSL ES Vita backend: built locally and linked as `libOpenSLES.a`;
  original Apache/Khronos notices are retained
- zlib: zlib license; built locally and linked as `libz.a`
- minimp3: CC0; compiled into the loader for OBB MP3 streaming
- libsndfile: LGPL-2.1-or-later; its public header defines the narrow audio
  adapter ABI, but its hard-float library is not linked
- vitaGL: the pinned source with the included scissor-state/binding-restoration
  patch, LGPL-3.0-or-later; original source and licenses are in `lib/vitagl/`
- vitaShaRK: LGPL-3.0-or-later; supplied by the soft-float VitaSDK
- SceShaccCgExt: current upstream license is LGPL-2.1; the installed SDK
  archive's exact source revision must be verified before binary distribution
- mathneon: MIT; its original notice is reproduced in
  `licenses/THIRD-PARTY-NOTICES.txt`
- SHA-1 helper: Brad Conte attribution and original disclaimer retained;
  [upstream](https://github.com/B-Con/crypto-algorithms) publishes these
  implementations as public-domain code

Full dependency license texts and additional BSD/ISC/Khronos/stb notices are
included in `licenses/` and packaged with the application. These notices alone
are not a legal compliance certification. Source/relinking materials must
accompany binary distribution as required by the applicable licenses.

- Android-loader LiveArea background/gate: unchanged assets from the
  `v-atamanenko/soloader-boilerplate` loader lineage, restored from the user's
  local loader reference. The loader project is MIT-licensed; Android artwork
  and branding remain private-test material pending redistribution review.

No game executable, expansion file, save, copied cache, or music is bundled.
The TournamentArt-derived bubble icon and user-provided gameplay screenshots
contain LEGO/WB artwork; screenshots are not packaged in the VPK. Those assets,
Android branding and trademarks are not licensed by the loader's MIT license.
Current artwork remains private-test material pending redistribution review.
