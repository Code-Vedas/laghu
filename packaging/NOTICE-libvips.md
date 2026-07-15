# libvips runtime dependency

The optional `laghu-libvips` image backend dynamically links to libvips,
which is distributed under the LGPL-2.1-or-later license. Laghu does not vendor
or statically link libvips. Official packages must retain the libvips package's
license and source-availability notices and declare its JPEG, PNG, GIF, and
WebP runtime dependencies.

For Enterprise Linux 9, production Laghu repositories will carry dynamically
linked, Codevedas-built dependency RPMs from pinned and reviewed source RPMs.
Remi Safe is used only to bootstrap Rocky and Alma development tests until
those repository artifacts exist; it is not an installation prerequisite for
Laghu users.
