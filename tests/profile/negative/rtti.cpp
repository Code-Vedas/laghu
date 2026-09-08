// SPDX-License-Identifier: AGPL-3.0-only
struct base;
struct derived;
derived* rejects_rtti(base* value) { return dynamic_cast<derived*>(value); }
