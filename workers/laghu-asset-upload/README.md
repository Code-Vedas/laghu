# laghu-asset-upload

This worker publishes configured immutable assets after request processing. Native modules and standalone queue jobs here; the worker owns provider authentication and network delivery.

Run `laghu-asset-upload --once /etc/laghu/asset-offload.conf` for one job or `laghu-asset-upload --serve` for continuous processing.
