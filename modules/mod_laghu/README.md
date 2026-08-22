# mod_laghu

This directory contains Laghu native Apache HTTP Server integration. It maps Apache request and response state into the shared HTTP engine, while Apache retains bucket-brigade and error-log ownership.

Build from the repository root with `cmake --build build --target mod_laghu`, then load the produced module into Apache. Operator configuration is in `docs/pages/04-mod-laghu/`.
