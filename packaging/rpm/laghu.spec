# Copyright Codevedas Inc. 2026-present
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

Name: laghu-libvips
%global _lto_cflags %{nil}
Version: 0.1.0
Release: 1%{?dist}
Summary: Shared asynchronous libvips service for Laghu
License: MIT
URL: https://laghu.codevedas.com
Source0: laghu-%{version}.tar.gz
%global laghu_source_dir %{_builddir}/laghu-%{version}
%global _nginx_modsrcdir %{laghu_source_dir}/packaging/nginx-module
%global _nginx_modbuilddir %{laghu_source_dir}/nginx-module-build
BuildRequires: cmake
BuildRequires: bsdtar
BuildRequires: gcc
BuildRequires: httpd-devel
BuildRequires: make
BuildRequires: nginx-mod-devel
BuildRequires: pkgconfig(vips) >= 8.15
BuildRequires: cargo
BuildRequires: rust >= 1.86
Requires: vips >= 8.15
Requires: ffmpeg
Requires(pre): shadow-utils
Requires(post): shadow-utils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd
Obsoletes: laghu-optimizer < %{version}-%{release}

%description
Consumes bounded Laghu image jobs outside web-server processes and atomically
publishes validated variants. It also runs the separately isolated,
provider-allowlisted external font stylesheet fetch service.
It also runs the HTTPS HTML stale-while-revalidate worker and the locked native
SWC JavaScript optimization worker.

%package -n ngx-laghu
Summary: Native Laghu HTTP optimization module for NGINX
Requires: laghu-libvips%{?_isa} = %{version}-%{release}
Provides: nginx-mod-http-laghu = %{version}-%{release}
Obsoletes: nginx-mod-http-laghu < %{version}-%{release}
%nginx_modrequires

%description -n ngx-laghu
Precompiled NGINX dynamic module built against the distribution NGINX ABI.
The module performs policy, cache lookup, and nonblocking service publication.

%package -n mod-laghu
Summary: Native Laghu output-filter module for Apache HTTP Server
Requires: httpd-mmn = %{_httpd_mmn}
Requires: laghu-libvips%{?_isa} = %{version}-%{release}

%description -n mod-laghu
Apache HTTP Server 2.4 output-filter adapter using Laghu's shared policy,
queue, cache, and fail-open delivery contracts.

%package -n laghu
Summary: Standalone Laghu reverse proxy
Requires: laghu-libvips%{?_isa} = %{version}-%{release}

%description -n laghu
Standalone Laghu HTTP reverse proxy using the same policy and transformation
contracts as native modules.

%prep
%setup -q -T -c -n laghu-%{version}
bsdtar -xf %{SOURCE0} --strip-components 1 -C .

%build
%cmake -DLAGHU_WITH_VIPS=ON -DLAGHU_BUILD_TESTS=OFF
%cmake_build
%nginx_modconfigure
%nginx_modbuild
APACHE_BUILD_DIR=%{_builddir}/laghu-apache-module scripts/build-apache-module

%install
install -D -m 0755 %{__cmake_builddir}/servers/laghu/laghu \
  %{buildroot}%{_bindir}/laghu
install -D -m 0755 %{__cmake_builddir}/workers/laghu-libvips/laghu-libvips \
  %{buildroot}%{_bindir}/laghu-libvips
install -D -m 0755 %{__cmake_builddir}/workers/laghu-resource-fetch/laghu-resource-fetch \
  %{buildroot}%{_bindir}/laghu-resource-fetch
install -D -m 0755 %{__cmake_builddir}/workers/laghu-html-refresh/laghu-html-refresh \
  %{buildroot}%{_bindir}/laghu-html-refresh
install -D -m 0755 %{__cmake_builddir}/workers/laghu-js-optimize/cargo/release/laghu-js-optimize \
  %{buildroot}%{_bindir}/laghu-js-optimize
install -D -m 0755 %{__cmake_builddir}/workers/laghu-chrome-analyze/laghu-chrome-analyze \
  %{buildroot}%{_bindir}/laghu-chrome-analyze
install -D -m 0755 %{_nginx_modbuilddir}/ngx_http_laghu_module.so \
  %{buildroot}%{nginx_moddir}/ngx_http_laghu_module.so
install -D -m 0644 packaging/nginx/mod-http-laghu.conf \
  %{buildroot}%{nginx_modconfdir}/mod-http-laghu.conf
sed -i 's|modules/ngx_http_laghu_module.so|%{nginx_moddir}/ngx_http_laghu_module.so|' \
  %{buildroot}%{nginx_modconfdir}/mod-http-laghu.conf
install -D -m 0644 packaging/nginx/laghu.conf \
  %{buildroot}%{_sysconfdir}/nginx/conf.d/laghu.conf
install -D -m 0755 %{_builddir}/laghu-apache-module/.libs/mod_laghu.so \
  %{buildroot}%{_libdir}/httpd/modules/mod_laghu.so
install -D -m 0644 packaging/apache/laghu-rpm.load \
  %{buildroot}%{_sysconfdir}/httpd/conf.modules.d/10-laghu.conf
install -D -m 0644 packaging/apache/laghu-rpm.conf \
  %{buildroot}%{_sysconfdir}/httpd/conf.d/laghu.conf
install -D -m 0644 packaging/systemd/laghu-libvips.service \
  %{buildroot}%{_unitdir}/laghu-libvips.service
install -D -m 0644 packaging/systemd/laghu-resource-fetch.service \
  %{buildroot}%{_unitdir}/laghu-resource-fetch.service
install -D -m 0644 packaging/systemd/laghu-html-refresh@.service \
  %{buildroot}%{_unitdir}/laghu-html-refresh@.service
install -D -m 0644 packaging/systemd/laghu-js-optimize.service \
  %{buildroot}%{_unitdir}/laghu-js-optimize.service
install -D -m 0644 packaging/systemd/laghu-chrome-analyze.service \
  %{buildroot}%{_unitdir}/laghu-chrome-analyze.service
install -D -m 0644 packaging/font-providers.conf \
  %{buildroot}%{_sysconfdir}/laghu/font-providers.conf
install -D -m 0644 packaging/html-refresh.conf.example \
  %{buildroot}%{_docdir}/laghu/html-refresh.conf.example
install -D -m 0644 packaging/chrome-analysis.conf.example \
  %{buildroot}%{_docdir}/laghu/chrome-analysis.conf.example
install -D -m 0644 packaging/javascript-observation.conf \
  %{buildroot}%{_sysconfdir}/laghu/javascript-observation.conf
install -D -m 0644 packaging/javascript-defer.conf \
  %{buildroot}%{_sysconfdir}/laghu/javascript-defer.conf
install -D -m 0644 packaging/tmpfiles/laghu.conf \
  %{buildroot}%{_tmpfilesdir}/laghu.conf

%pre
getent group laghu >/dev/null || groupadd --system laghu
getent passwd laghu >/dev/null || \
  useradd --system --gid laghu --home-dir /nonexistent --shell /sbin/nologin laghu

%post
%systemd_post laghu-libvips.service
%systemd_post laghu-resource-fetch.service
%systemd_post laghu-html-refresh@.service
%systemd_post laghu-js-optimize.service
%systemd_post laghu-chrome-analyze.service
for account in nginx apache; do
  if getent passwd "$account" >/dev/null; then
    usermod -a -G laghu "$account"
  fi
done
systemd-tmpfiles --create laghu.conf >/dev/null 2>&1 || :

%preun
%systemd_preun laghu-libvips.service
%systemd_preun laghu-resource-fetch.service
%systemd_preun laghu-html-refresh@.service
%systemd_preun laghu-js-optimize.service
%systemd_preun laghu-chrome-analyze.service

%postun
%systemd_postun_with_restart laghu-libvips.service
%systemd_postun_with_restart laghu-resource-fetch.service
%systemd_postun_with_restart laghu-html-refresh@.service
%systemd_postun_with_restart laghu-js-optimize.service
%systemd_postun_with_restart laghu-chrome-analyze.service

%post -n ngx-laghu
nginx -t

%post -n mod-laghu
httpd -t

%files
%license LICENSE packaging/NOTICE-libvips.md
%{_bindir}/laghu-libvips
%{_bindir}/laghu-resource-fetch
%{_bindir}/laghu-html-refresh
%{_bindir}/laghu-js-optimize
%{_bindir}/laghu-chrome-analyze
%{_unitdir}/laghu-libvips.service
%{_unitdir}/laghu-resource-fetch.service
%{_unitdir}/laghu-html-refresh@.service
%{_unitdir}/laghu-js-optimize.service
%{_unitdir}/laghu-chrome-analyze.service
%config(noreplace) %{_sysconfdir}/laghu/font-providers.conf
%doc %{_docdir}/laghu/html-refresh.conf.example
%doc %{_docdir}/laghu/chrome-analysis.conf.example
%config(noreplace) %{_sysconfdir}/laghu/javascript-observation.conf
%config(noreplace) %{_sysconfdir}/laghu/javascript-defer.conf
%{_tmpfilesdir}/laghu.conf

%files -n ngx-laghu
%license LICENSE
%{nginx_moddir}/ngx_http_laghu_module.so
%{nginx_modconfdir}/mod-http-laghu.conf
%config(noreplace) %{_sysconfdir}/nginx/conf.d/laghu.conf

%files -n mod-laghu
%license LICENSE
%{_libdir}/httpd/modules/mod_laghu.so
%config(noreplace) %{_sysconfdir}/httpd/conf.modules.d/10-laghu.conf
%config(noreplace) %{_sysconfdir}/httpd/conf.d/laghu.conf

%files -n laghu
%license LICENSE
%{_bindir}/laghu
