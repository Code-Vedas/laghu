// SPDX-License-Identifier: AGPL-3.0-only
struct base { virtual ~base() = default; };
struct derived final : base {};
derived* rejects_disabled_rtti(base* value) { return dynamic_cast<derived*>(value); }
