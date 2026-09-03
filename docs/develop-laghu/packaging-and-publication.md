---
title: Packaging and publication
parent: Develop Laghu
---
# Packaging and publication

Publication produces `ngx-laghu`, `mod-laghu`, and `laghu`; `laghu-libvips` is an internal dependency. Release packages, Homebrew, GHCR, and Helm must carry the same verified product behavior.

## Chrome analysis isolation

Chrome analysis is deliberately narrower than the core product. Enable it only
through the dedicated Debian or compatible-RPM systemd package service, after
copying its packaged `chrome-analysis.conf.example` to `/etc/laghu/` and
providing a distribution Chromium ELF below `/usr/lib` or `/usr/lib64` (not a
shell launcher). Debian Bookworm uses `/usr/lib/chromium/chromium`. The service
is supported on Linux x86_64 and aarch64 only, and requires
Bubblewrap 0.8.0 or newer and an enabled unprivileged-user-namespace policy.
Each captured document runs in fresh user, mount, network, and PID namespaces,
with only the dedicated Chromium runtime tree, exact discovered ELF files, and
the fixed read-only Fontconfig roots `/etc/fonts`, `/usr/share/fontconfig`, and
`/usr/share/fonts` visible. An AF_UNIX-only socket filter and bounded DevTools
Fetch interception abort every non-job request, then require every interception
reply before publishing its report. If startup cannot establish the boundary or
validate the configured Chromium runtime, the worker exits `78` without
processing HTML. A per-job Chrome or CDP protocol failure produces no report
and never falls back to a less-isolated browser.

EL9 packages intentionally omit the optional Chrome executable, unit, and
example because their base Bubblewrap is below the supported version. Homebrew
and combined NGINX/Apache containers also leave Chrome analysis unavailable;
keep `ChromeAnalysisQueue` unset there. This preserves the heuristic fallback
without offering a best-effort browser sandbox.
