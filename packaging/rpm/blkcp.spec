%define debug_package %{nil}

Name:           blkcp
Version:        1.0.0
Release:        1%{?dist}
Summary:        Next-Gen High-Performance Block Copy Tool for Linux
License:        GPL-3.0-or-later
URL:            https://github.com/Meik1982/blkcp
Source0:        https://github.com/Meik1982/blkcp/archive/refs/tags/v%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  liburing-devel
BuildRequires:  openssl-devel
BuildRequires:  ncurses-devel

Requires:       liburing
Requires:       openssl
Requires:       ncurses

%description
blkcp is a ground-up reimagining and modern modularization of the historical dd
utility, engineered specifically for high-performance Linux storage, NVMe throughput,
CoW filesystems, and human safety. Features 5 I/O engines (io_uring, async ringbuffer,
reflink CoW, splice, sync), Target Safety Guard, and an interactive 2D spatial TUI.

%prep
%autosetup -n %{name}-%{version}

%build
%make_build release

%install
%make_install PREFIX=/usr

%files
%license LICENSE
%doc README.md
%{_bindir}/blkcp
%{_bindir}/blkcp-tui
%{_mandir}/man1/blkcp.1*
%{_datadir}/bash-completion/completions/blkcp
%{_datadir}/zsh/site-functions/_blkcp
%{_datadir}/fish/vendor_completions.d/blkcp.fish

%changelog
* Mon Sep 21 2026 Meik <meik@github.com> - 1.0.0-1
- Initial public release v1.0.0
