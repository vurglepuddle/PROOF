[Developer documentation](../readme.md) / [Compiling Inkscape](./readme.md) /

# Compiling Inkscape on Linux


**TODO** - this is currently being merged with instructions on the [Website](https://inkscape.org/develop/getting-started/) and [wiki](https://wiki.inkscape.org/wiki/index.php?title=Compiling_Inkscape).


## Getting Dependencies

For common linux-distributions (Ubuntu, Debian, Fedora, Arch and more) you can use
[a bash-script](https://gitlab.com/inkscape/inkscape-ci-docker/-/raw/master/install_dependencies.sh?inline=false) 
for getting required libraries:

```bash
wget -v https://gitlab.com/inkscape/inkscape-ci-docker/-/raw/master/install_dependencies.sh -O install_dependencies.sh
bash install_dependencies.sh --recommended
```

For a detailed list of all dependencies see [Tracking Dependencies](https://wiki.inkscape.org/wiki/Tracking_Dependencies).

## Getting Inkscape Source


To obtain the latest source code, use the following command (downloads into a subdirectory of your current working directory called "inkscape" by default):
```bash
git clone --recurse-submodules https://gitlab.com/inkscape/inkscape.git
```

Then change into that directory:
```bash
cd inkscape
```

To update the code later use:
```bash
git pull --recurse-submodules && git submodule update --recursive
```

## Compiling

Inkscape is built using CMake and Ninja.

To compile and run Inkscape, run the following in the Inkscape code directory:
```sh
# Create build subdirectory
mkdir build
# Change to it
cd build
# run CMake for initial setup
cmake -DCMAKE_INSTALL_PREFIX="${PWD}/install_dir" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_BUILD_TYPE=Debug -DWITH_INTERNAL_2GEOM=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -G Ninja ..
# compile
ninja install
# run
./install_dir/bin/inkscape
```

To compile again after making changes, you can re-run the `ninja install` command. Make sure you are in the `build` subdirectory.


## Problems

☎ _If you can't solve your issue with the information above, please [ask in the chat](https://chat.inkscape.org/channel/team_devel) or [report a bug](https://inkscape.org/report)_.

## See also
- [PROOF project overview](../../README.md)
- [Advanced Information on Compiling Inkscape](general_advanced.md)
- [Packaging for Ubuntu Snap](../../snap/README.md)
