%global tarname mutter-layer-shell

Name:           mutter-layer-shell
Version:        50.1
Release:        1
Summary:        Mutter with wlr-layer-shell protocol support

License:        GPL-2.0-or-later
URL:            https://github.com/Caellian/mutter-layer-shell
Source0:        %{tarname}-%{version}.tar.gz

BuildRequires:  meson >= 1.5.0
BuildRequires:  gcc
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gobject-2.0)
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(cairo)
BuildRequires:  pkgconfig(pango)
BuildRequires:  pkgconfig(fribidi)
BuildRequires:  pkgconfig(atk)
BuildRequires:  pkgconfig(graphene-gobject-1.0)
BuildRequires:  pkgconfig(gnome-desktop-4)
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libadwaita-1)
BuildRequires:  pkgconfig(gmodule-no-export-2.0)
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(xkbregistry)
BuildRequires:  pkgconfig(colord)
BuildRequires:  pkgconfig(lcms2)
BuildRequires:  pkgconfig(libei-1.0)
BuildRequires:  pkgconfig(libeis-1.0)
BuildRequires:  pkgconfig(libdisplay-info)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(wayland-protocols)
BuildRequires:  pkgconfig(libpipewire-0.3)
BuildRequires:  pkgconfig(libinput)
BuildRequires:  pkgconfig(gudev-1.0)
BuildRequires:  pkgconfig(gbm)
BuildRequires:  pkgconfig(libwacom)
BuildRequires:  pkgconfig(libstartup-notification-1.0)
BuildRequires:  pkgconfig(x11)
BuildRequires:  pkgconfig(xcomposite)
BuildRequires:  pkgconfig(xcursor)
BuildRequires:  pkgconfig(xdamage)
BuildRequires:  pkgconfig(xext)
BuildRequires:  pkgconfig(xfixes)
BuildRequires:  pkgconfig(xi)
BuildRequires:  pkgconfig(xrandr)
BuildRequires:  pkgconfig(x11-xcb)
BuildRequires:  pkgconfig(xcb-res)
BuildRequires:  pkgconfig(xinerama)
BuildRequires:  pkgconfig(xau)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(json-glib-1.0)

Provides:       mutter = %{version}-%{release}
Conflicts:      mutter

%description
Mutter window manager and compositor patched with wlr-layer-shell protocol
support. Layer shell surfaces are drawn within the desktop layer, participating
in workspace switch animations and clipping.

%prep
%setup -q -n %{tarname}-%{version}

%build
%meson \
  -Dtests=disabled \
  -Dprofiler=false \
  -Ddocs=false \
  -Dinstalled_tests=false
%meson_build

%install
%meson_install

%files
%license COPYING
%{_bindir}/*
%{_libdir}/*.so.*
%{_libdir}/mutter-*
%{_libexecdir}/mutter-*
%{_datadir}/mutter-*
%{_datadir}/glib-2.0/schemas/*mutter*
%{_datadir}/locale/*/LC_MESSAGES/mutter.mo
%{_datadir}/GConf/gsettings/*
