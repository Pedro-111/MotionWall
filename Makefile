# MotionWall Makefile
CC = gcc
CFLAGS = -g -O2 -Wall -Wextra -std=c99
LDFLAGS = 
LDLIBS = -lX11 -lXext -lXrender -lXrandr

PREFIX = /usr/local
BINDIR = $(PREFIX)/bin
DATADIR = $(PREFIX)/share
DOCDIR = $(DATADIR)/doc/motionwall
MANDIR = $(DATADIR)/man/man1

INSTALL = install
RM = rm -f

TARGET = motionwall
SOURCES = motionwall.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all install clean uninstall package deb rpm appimage

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	$(INSTALL) -d -m 755 '$(DESTDIR)$(BINDIR)'
	$(INSTALL) -d -m 755 '$(DESTDIR)$(DOCDIR)'
	$(INSTALL) -d -m 755 '$(DESTDIR)$(MANDIR)'
	$(INSTALL) -m 755 $(TARGET) '$(DESTDIR)$(BINDIR)'
	$(INSTALL) -m 644 README.md '$(DESTDIR)$(DOCDIR)'
	$(INSTALL) -m 644 examples/*.sh '$(DESTDIR)$(DOCDIR)/examples/'
	$(INSTALL) -m 644 motionwall.1 '$(DESTDIR)$(MANDIR)'

clean:
	$(RM) $(TARGET) $(OBJECTS)

uninstall:
	$(RM) '$(DESTDIR)$(BINDIR)/$(TARGET)'
	$(RM) -r '$(DESTDIR)$(DOCDIR)'
	$(RM) '$(DESTDIR)$(MANDIR)/motionwall.1'

# Package creation targets
package: deb

deb: $(TARGET)
	mkdir -p packaging/deb/DEBIAN
	mkdir -p packaging/deb/usr/bin
	mkdir -p packaging/deb/usr/share/doc/motionwall
	mkdir -p packaging/deb/usr/share/man/man1
	
	cp $(TARGET) packaging/deb/usr/bin/
	cp README.md packaging/deb/usr/share/doc/motionwall/
	cp motionwall.1 packaging/deb/usr/share/man/man1/
	gzip packaging/deb/usr/share/man/man1/motionwall.1
	
	echo "Package: motionwall" > packaging/deb/DEBIAN/control
	echo "Version: 1.0.0" >> packaging/deb/DEBIAN/control
	echo "Section: utils" >> packaging/deb/DEBIAN/control
	echo "Priority: optional" >> packaging/deb/DEBIAN/control
	echo "Architecture: amd64" >> packaging/deb/DEBIAN/control
	echo "Depends: libx11-6, libxext6, libxrender1, libxrandr2" >> packaging/deb/DEBIAN/control
	echo "Maintainer: MotionWall Project" >> packaging/deb/DEBIAN/control
	echo "Description: Advanced Desktop Background Animation Tool" >> packaging/deb/DEBIAN/control
	echo " MotionWall allows you to use videos, GIFs, and animations as" >> packaging/deb/DEBIAN/control
	echo " your desktop wallpaper with multi-monitor support and playlist functionality." >> packaging/deb/DEBIAN/control
	
	dpkg-deb --build packaging/deb motionwall_1.0.0_amd64.deb

rpm: $(TARGET)
	mkdir -p packaging/rpm/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
	
	echo "Name: motionwall" > packaging/rpm/SPECS/motionwall.spec
	echo "Version: 1.0.0" >> packaging/rpm/SPECS/motionwall.spec
	echo "Release: 1" >> packaging/rpm/SPECS/motionwall.spec
	echo "Summary: Advanced Desktop Background Animation Tool" >> packaging/rpm/SPECS/motionwall.spec
	echo "License: MIT" >> packaging/rpm/SPECS/motionwall.spec
	echo "Group: Applications/Multimedia" >> packaging/rpm/SPECS/motionwall.spec
	echo "Requires: libX11, libXext, libXrender, libXrandr" >> packaging/rpm/SPECS/motionwall.spec
	echo "" >> packaging/rpm/SPECS/motionwall.spec
	echo "%description" >> packaging/rpm/SPECS/motionwall.spec
	echo "MotionWall allows you to use videos, GIFs, and animations as your desktop wallpaper." >> packaging/rpm/SPECS/motionwall.spec
	echo "" >> packaging/rpm/SPECS/motionwall.spec
	echo "%files" >> packaging/rpm/SPECS/motionwall.spec
	echo "/usr/bin/motionwall" >> packaging/rpm/SPECS/motionwall.spec
	
	rpmbuild -ba packaging/rpm/SPECS/motionwall.spec

appimage: $(TARGET)
	mkdir -p packaging/AppImage/AppDir/usr/bin
	mkdir -p packaging/AppImage/AppDir/usr/share/applications
	mkdir -p packaging/AppImage/AppDir/usr/share/icons/hicolor/256x256/apps
	
	cp $(TARGET) packaging/AppImage/AppDir/usr/bin/
	
	echo "[Desktop Entry]" > packaging/AppImage/AppDir/motionwall.desktop
	echo "Type=Application" >> packaging/AppImage/AppDir/motionwall.desktop
	echo "Name=MotionWall" >> packaging/AppImage/AppDir/motionwall.desktop
	echo "Comment=Advanced Desktop Background Animation Tool" >> packaging/AppImage/AppDir/motionwall.desktop
	echo "Exec=motionwall" >> packaging/AppImage/AppDir/motionwall.desktop
	echo "Icon=motionwall" >> packaging/AppImage/AppDir/motionwall.desktop
	echo "Categories=AudioVideo;Graphics;" >> packaging/AppImage/AppDir/motionwall.desktop
	
	cp packaging/AppImage/AppDir/motionwall.desktop packaging/AppImage/AppDir/usr/share/applications/
	
	# Download AppImageTool if not present
	if [ ! -f appimagetool ]; then \
		wget -O appimagetool https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-x86_64.AppImage; \
		chmod +x appimagetool; \
	fi
	
	./appimagetool packaging/AppImage/AppDir MotionWall-1.0.0-x86_64.AppImage
