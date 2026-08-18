# libvips runtime dependency

The optional `laghu-libvips` image backend dynamically links to libvips, which is distributed under the LGPL-2.1-or-later license. Laghu does not vendor or statically link libvips. Official packages must retain the libvips package's license and source-availability notices and declare its JPEG, PNG, GIF, and WebP runtime dependencies. Large animated-GIF conversion invokes the distribution FFmpeg executable as a separate process; Laghu neither links nor vendors FFmpeg, and packages declare it as a runtime dependency.
