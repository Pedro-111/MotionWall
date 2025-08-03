# MotionWall - Advanced Desktop Background Animation Tool

MotionWall is a powerful and feature-rich tool that allows you to use videos, GIFs, and animations as your desktop wallpaper. It's a complete rewrite and improvement over xwinwrap with modern features and better desktop environment integration.

![MotionWall Demo](demo.gif)

## Features

### 🖥️ **Multi-Monitor Support**
- **Real multi-monitor detection** using Xrandr
- **Independent content per monitor** or synchronized playback
- **Automatic monitor configuration** detection and adjustment
- **Primary monitor detection** and smart defaults

### 🎵 **Advanced Playlist Support**
- **Directory scanning** for automatic playlist creation
- **Multiple format support**: MP4, AVI, MKV, MOV, WebM, GIF, MP3, WAV
- **Shuffle and loop modes** with configurable timing
- **Automatic media switching** based on duration settings
- **Smart file detection** with recursive directory support

### 🎨 **Desktop Environment Integration**
- **Automatic DE detection**: GNOME, KDE, XFCE, Cinnamon, MATE, LXDE, i3
- **Compositor awareness**: Works with picom, compton, KWin, Muffin
- **Proper window layering** to avoid conflicts with desktop icons
- **Seamless background integration** without visual artifacts

### ⚙️ **Smart Configuration**
- **Auto-resolution detection** using native monitor capabilities
- **Configuration file support** with persistent settings
- **Media player flexibility**: MPV, MPlayer, VLC support
- **Daemon mode** for background operation
- **Debug mode** for troubleshooting

### 📦 **Easy Distribution**
- **Multiple package formats**: DEB, RPM, AppImage
- **One-click installation** packages
- **Dependency management** for different distributions
- **Portable AppImage** for universal compatibility

## Installation

### From Packages (Recommended)

#### Ubuntu/Debian:
```bash
wget https://github.com/Pedro-111/motionwall/releases/latest/download/motionwall_1.0.0_amd64.deb
sudo dpkg -i motionwall_1.0.0_amd64.deb
sudo apt-get install -f  # Fix any dependency issues
