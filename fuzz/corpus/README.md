<!-- SPDX-License-Identifier: AGPL-3.0-only -->

# Fuzz corpus

Each target owns minimized inputs in `fuzz/corpus/<target>/`. Ordinary regression tests remain in the owning `tests/` subsystem.

Corpus inputs are AGPL-3.0-only test data; their payload bytes intentionally carry no SPDX tag.
