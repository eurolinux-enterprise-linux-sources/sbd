#
# spec file for package sbd
#
# Copyright (c) 2014 SUSE LINUX Products GmbH, Nuernberg, Germany.
# Copyright (c) 2013 Lars Marowsky-Bree
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via http://bugs.opensuse.org/
#
%global commit c511b0692784a7085df4b1ae35748fb318fa79ee
%global shortcommit %(c=%{commit}; echo ${c:0:7})
%global github_owner Clusterlabs
%global buildnum 31

Name:           sbd
Summary:        Storage-based death
%if %{defined _unitdir}
License:        GPLv2+
%else
# initscript is Revised BSD
License:        GPLv2+ and BSD
%endif
Group:          System Environment/Daemons
Version:        1.2.1
#Release:        0.%{buildnum}.%{shortcommit}.git%{?dist}
Release:        %{buildnum}%{?dist}
Url:            https://github.com/%{github_owner}/%{name}
Source0:        https://github.com/%{github_owner}/%{name}/archive/%{commit}/%{name}-%{commit}.tar.gz
Source1:        sbd.8.pod
BuildRoot:      %{_tmppath}/%{name}-%{version}-build
BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  libuuid-devel
BuildRequires:  glib2-devel
BuildRequires:  libaio-devel
BuildRequires:  corosync-devel
BuildRequires:  pacemaker-libs-devel > 1.1.12
BuildRequires:  libtool
BuildRequires:  libuuid-devel
BuildRequires:  libxml2-devel
BuildRequires:  pkgconfig
BuildRequires:  python-devel
Patch1:         sbd-no-storage-option.patch
Patch6:         0001-Add-LSB-init.patch
Patch7:         0002-build-steal-CONFIGDIR-INITDIR-from-pacemaker.patch

%if 0%{?rhel} > 0
ExclusiveArch: i686 x86_64
%endif

%if %{defined systemd_requires}
%systemd_requires
%endif

%description

This package contains the storage-based death functionality.
Currently it is limited to watchdog integration.

###########################################################

%prep
%setup -n %{name}-%{commit}
cp %SOURCE1 man
%patch1 -p1
%patch6 -p1
%patch7 -p1

###########################################################

%build
autoreconf -i
export CFLAGS="$RPM_OPT_FLAGS -Wall -Werror"
%configure --disable-shared-disk
make %{?_smp_mflags}

###########################################################

%install

make DESTDIR=$RPM_BUILD_ROOT LIBDIR=%{_libdir} install
rm -rf ${RPM_BUILD_ROOT}%{_libdir}/stonith

%if %{defined _unitdir}
install -D -m 0644 src/sbd.service $RPM_BUILD_ROOT/%{_unitdir}/sbd.service
install -D -m 0644 src/sbd_remote.service $RPM_BUILD_ROOT/%{_unitdir}/sbd_remote.service
%else
install -D -m 0755 src/sbd_helper $RPM_BUILD_ROOT/%{_initddir}/sbd_helper
install -D -m 0755 src/sbd_remote_helper $RPM_BUILD_ROOT/%{_initrddir}/sbd_remote_helper
%endif

mkdir -p ${RPM_BUILD_ROOT}%{_sysconfdir}/sysconfig
install -m 644 src/sbd.sysconfig ${RPM_BUILD_ROOT}%{_sysconfdir}/sysconfig/sbd

###########################################################

%clean
rm -rf %{buildroot}

%if %{defined _unitdir}
%post
%systemd_post sbd.service
%systemd_post sbd_remote.service

%preun
%systemd_preun sbd.service
%systemd_preun sbd_remote.service

%postun
%systemd_postun sbd.service
%systemd_postun sbd_remote.service
%else
%post
/sbin/chkconfig --add sbd_helper || :
/sbin/chkconfig --add sbd_remote_helper || :

%preun
/sbin/service sbd_helper stop  &>/dev/null || :
/sbin/service sbd_remote_helper stop  &>/dev/null || :
if [ $1 -eq 0 ]; then
    # Package removal, not upgrade
    /sbin/chkconfig --del sbd_helper || :
    /sbin/chkconfig --del sbd_remote_helper || :
fi
%endif

%files
###########################################################
%defattr(-,root,root)
%config(noreplace) %{_sysconfdir}/sysconfig/sbd
%{_sbindir}/sbd
#%{_datadir}/sbd
%doc %{_mandir}/man8/sbd*
%if %{defined _unitdir}
%{_unitdir}/sbd.service
%{_unitdir}/sbd_remote.service
%else
%{_initddir}/sbd_helper
%{_initrddir}/sbd_remote_helper
%endif
%doc COPYING

%changelog
* Mon Nov 7 2016 <kwenning@redhat.com> - 1.2.1-31
- init-patch updated
- added CONFDIR & INITDIR for distro indep init
- added stripped down manpage
  Resolves: rhbz#1375244

* Tue Oct 11 2016 <kwenning@redhat.com> - 1.2.1-30
- init-patch updated
- Rebuild against RHEL-6.9
  Resolves: rhbz#1375244

-* Mon Apr 4 2016 <kwenning@redhat.com> - 1.2.1-9
- use SBD_OPTS when calling sbd
  Resolves: rhbz#1313246
 
* Fri Mar 18 2016 <kwenning@redhat.com> - 1.2.1-8
- added init
  Resolves: rhbz#1313246
  
* Wed Mar 16 2016 <kwenning@redhat.com> - 1.2.1-7
- removed s390x target
  Resolves: rhbz#1313246

* Tue Mar 15 2016 <kwenning@redhat.com> - 1.2.1-6
- Rebuild for pacemaker (altered to work without autosetup)
  Resolves: rhbz#1313246

* Thu Jul 23 2015 <abeekhof@redhat.com> - 1.2.1-5
- Rebuild for pacemaker

* Mon Jun 02 2015 <abeekhof@redhat.com> - 1.2.1-4
- Include the dist tag in the release string
- Rebuild for new pacemaker

* Mon Jan 12 2015 <abeekhof@redhat.com> - 1.2.1-3
- Correctly parse SBD_WATCHDOG_TIMEOUT into seconds (not milliseconds)

* Mon Oct 27 2014 <abeekhof@redhat.com> - 1.2.1-2
- Correctly enable /proc/pid validation for sbd_lock_running()

* Fri Oct 24 2014 <abeekhof@redhat.com> - 1.2.1-1
- Further improve integration with the el7 environment

* Thu Oct 16 2014 <abeekhof@redhat.com> - 1.2.1-0.5.872e82f3.git
- Disable unsupported functionality (for now)

* Wed Oct 15 2014 <abeekhof@redhat.com> - 1.2.1-0.4.872e82f3.git
- Improved integration with the el7 environment

* Tue Sep 30 2014 <abeekhof@redhat.com> - 1.2.1-0.3.8f912945.git
- Only build on archs supported by the HA Add-on

* Fri Aug 29 2014 <abeekhof@redhat.com> - 1.2.1-0.2.8f912945.git
- Remove some additional SUSE-isms

* Fri Aug 29 2014 <abeekhof@redhat.com> - 1.2.1-0.1.8f912945.git
- Prepare for package review
  Resolves: rhbz#1134245
