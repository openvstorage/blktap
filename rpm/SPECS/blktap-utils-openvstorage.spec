Name: blktap-utils-openvstorage
Summary: blktap user space utilities
Version: 2.0.90
Release: 2.2
License: BSD and LGPLv2+
Group: System Environment/Libraries
URL: http://xen.org
Source0: blktap-utils-openvstorage-2.0.90.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-buildroot
BuildRequires: e2fsprogs-devel, volumedriver-dev
BuildRequires: libuuid-devel, libaio-devel
BuildRequires: automake, autoconf, libtool
%description
This package contains the blktap userspace utilities

%package devel
Summary: BlkTap Development Headers and Libraries
Requires: blktap = %{version}
Group: Development/Libraries

%description devel
This package contains the blktap development libraries and header files.

%prep
%setup -qn blktap ##blktap-utils-2.0.90

./autogen.sh

%configure --disable-static

%build
%{__make} USE_SYSTEM_LIBRARIES=y
find 

%install
rm -rf $RPM_BUILD_ROOT
%{__make} install USE_SYSTEM_LIBRARIES=y DESTDIR=$RPM_BUILD_ROOT LIBDIR=%{_libdir} SBINDIR=%{_sbindir} SYSCONFDIR=%{_sysconfdir} -Wno-format

#removing .la file
rm -rf $RPM_BUILD_ROOT%{_libdir}/libblktapctl.la
%clean
rm -rf $RPM_BUILD_ROOT

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%doc README
%{_libdir}/libvhd.so*
%{_libdir}/libvhdio.so*
%{_libdir}/libblktapctl.so*
%{_bindir}/vhd-*
%{_sbindir}/lvm-util
%{_sbindir}/part-util
%{_sbindir}/tap-ctl
%{_sbindir}/td-rated
%{_sbindir}/td-util
%{_sbindir}/vhdpartx
%{_libexecdir}/tapdisk
%{_sysconfdir}/udev/rules.d/blktap.rules

%files devel
%defattr(-,root,root,-)
%{_includedir}/blktap/*
%{_includedir}/vhd/*
%{_libdir}/libvhd*

%changelog
* Wed Dec 09 2015 Chrysostomos Nanakos <cnanakos@openvstorage.com> [2.0.90-2.2]
- Build blktap with Open vStorage support
