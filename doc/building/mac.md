[Developer documentation](../readme.md) / [Compiling Inkscape](./readme.md) /

# Compiling Inkscape on MacOS

Inkscape on MacOS can be built with either Homebrew or MacPorts.

## Using Homebrew

### Dependencies

Prerequisites:

- Xcode (AppStore)
- Xcode command line tools (`xcode-select --install`)
- [HomeBrew](https://brew.sh/)

Make sure you don't have any MacPorts stuff in your PATH. <!-- TODO - how? -->

Install packages:
```
brew install \
    adwaita-icon-theme \
    bdw-gc \
    boost \
    cairomm \
    ccache \
    cmake \
    double-conversion \
    gettext \
    gsl \
    gtkmm4 \
    gtksourceview5 \
    icu4c \
    imagemagick \
    intltool \
    lcms2 \
    libxslt \
    ninja \
    pkg-config \
    poppler \
    potrace
```

You may substitute `imagemagick` with `graphicsmagick`.

If you want to include a spell checker, also install `libspelling` using `brew`.

To build version 1.4.x you need `gtkmm3` instead of `gtkmm4` and also install libraries `gspell`, `libomp` and `libsoup@2`.

### Get Inkscape Source
Check out the source if you haven't already:

```
git clone --recurse-submodules https://gitlab.com/inkscape/inkscape.git
cd inkscape
```

### Build Inkscape

Inside the Inkscape directory, run the following commands

```
# use a clean Homebrew environment (optional)
LIBPREFIX="/opt/homebrew"
export PATH="$LIBPREFIX/bin:/usr/bin:/bin:/usr/sbin:/sbin"

# Some keg-only libraries need to be added to PKG_CONFIG_PATH
# Note: icu4c path is version specific (here: @77)
export PKG_CONFIG_PATH="$LIBPREFIX/opt/icu4c@77/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$LIBPREFIX/opt/libsoup@2/lib/pkgconfig"

# prevent picking up libxslt and libxml2 from the (wrong) SDK
export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$LIBPREFIX/opt/libxslt/lib/pkgconfig"
export PKG_CONFIG_PATH="$PKG_CONFIG_PATH:$LIBPREFIX/opt/libxml2/lib/pkgconfig"

mkdir -p build
cd build

cmake \
    -G Ninja \
    -DCMAKE_SHARED_LINKER_FLAGS="-L$LIBPREFIX/lib" \
    -DCMAKE_EXE_LINKER_FLAGS="-L$LIBPREFIX/lib" \
    -DCMAKE_INSTALL_PREFIX="${PWD}/install_dir" \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_BUILD_TYPE=Debug \
    -DWITH_INTERNAL_2GEOM=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DWITH_DBUS=OFF \
    ..

ninja install

# Start Inkscape
./install_dir/bin/inkscape
```

## Using MacPorts

We currently have no working instructions for MacPorts. Feel free to provide instructions.


## Packaging

This section explains how to turn Inkscape into an installable app or DMG.

<!-- TODO: Is the ninja install step really needed? mibap compiles Inkscape again anyway... --->

Compile and copy all relevant files into the `install_dir` folder:
```
ninja install
```

Start Inkscape from that folder:
```
./install_dir/bin/inkscape
```

To turn the folder into an app or a DMG, you can use the [mibap](https://github.com/dehesselle/mibap) toolset together with the resource files inside the inkscape/packaging/macos directory.

Follow the steps to install the toolset, package the app, and create a DMG. You will get the app under `/Users/Shared/work/mibap-*/` and you will get a DMG file in the same place you ran ./build_inkscape.sh. 

<!-- TODO for the reviewer: Is the above OK? The Wiki originally referred to "If using the upstream mibap", but did not explain what was the alternative to that "upstream mibap" -->

## Problems

☎ _If you can't solve your issue, please [ask in the chat](https://chat.inkscape.org/channel/team_devel) or [report a bug](https://inkscape.org/report)_.

Below, we present some typical workarounds. If these workarounds are needed, please still report a bug. Normally the instructions should work out of the box on a standard system.

### No GSettings schemas

If you encounter the following error (when, eg. going to open file dialog):
```
(org.inkscape.Inkscape:91305): GLib-GIO-ERROR **: 11:18:06.449: No GSettings schemas are installed on the system
```

Then you have to modify the following: `export XDG_DATA_DIRS=$XDG_DATA_DIRS:/opt/homebrew/share`

### Wrong library paths

Some libraries can cause trouble if they are picked up from the SDK instead of Homebrew (observed with libxslt and libxml2). Adding them to `$PKG_CONFIG_PATH` should fix this.
<!-- TODO - how to detect this trouble? -->


## See also
- [PROOF project overview](../../README.md)
- [Advanced Information on Compiling Inkscape](general_advanced.md)

