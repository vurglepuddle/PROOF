[Developer documentation](../readme.md) / [Compiling Inkscape](./readme.md) /

# Compiling Inkscape on Windows

This page explains how to compile Inkscape on Windows. We use [**MSYS2**](http://www.msys2.org/), which provides all necessary build tools and dependencies.

## Installing Build Dependencies Easily

In the basic case, you can install all the needed tools with a one-click script. This is supported for Inkscape 1.5 or newer, running on standard 64bit Windows (not ARM). In other cases, please see below "Installing Build Dependencies Manually".

1.  Get the buildtools folder from the Inkscape git repository. One way to do this is the following:
    *   Download the following ZIP [https://gitlab.com/inkscape/inkscape/-/archive/master/inkscape-master.zip?path=buildtools](https://gitlab.com/inkscape/inkscape/-/archive/master/inkscape-master.zip?path=buildtools)
    *   Unpack it
2.  Double-click on `windows-deps-clickHere.bat`
    *   If you get a warning "Windows protected your PC. Microsoft Defender prevented an unrecognized app from starting.", click on "More information" and "Run anyway".
    *   Follow the instructions in the Terminal window. (Press Enter to confirm.)
    *   Confirm Administrator rights if Windows asks you to
3.  Wait a few hours until the script says Done. Then you can compile Inkscape as described below.

## Obtaining Inkscape Source

As MSYS2 provides the version control software Git, you do not need to download it separately.

Open the MSYS2 **UCRT64** (!!!) shell from the start menu.

Run the command

```bash
git clone --recurse-submodules https://gitlab.com/inkscape/inkscape.git master
```

This creates a folder called "master" in the current working directory (usually the home folder located at `C:\msys64\home\Your_Username` or similar) in which the clone of the source repository is created.

You can later update it with:


```bash
git pull --recurse-submodules
```

## Building Inkscape with MSYS2

*   **Read carefully:** To compile Inkscape open the MSYS2 **UCRT64** (!!!) shell from the start menu (or launch C:\\msys64\\ucrt64.exe)
*   ⚠️ _Warning:_ Using the right shell type is important so that the right type of Inkscape is built and the right dependencies are installed:
    *   UCRT64 is the standard for compiling 64-bit Inkscape
    *   (MinGW 64bit / 32bit was used until mid 2023 to build 64/32bit versions. ARM may be used in the future to build for ARM processors.)
    *   **Never** use the "MSYS2 MSYS" shell for compiling Inkscape, only use it for updating MSYS2 itself.

**Double-check:** The shell window must show "**UCRT64**" in purple text. If it shows "MSYS", then you have the wrong shell.

Then execute the following commands:


```bash
# change to the directory containing your Inkscape source checkout (has to be
# adjusted to match your system)
cd master

# create a directory for the build (could also be another folder, but we'll
# assume 'build' being used for the rest of the article)
mkdir build
cd build

# create build files with CMake (we generate rules for "Ninja" as it's
# significantly faster then "MinGW Makefiles" which uses mingw32-make)
# note the source path '..' (which in this case is the parent directory) and
# should always point to the root folder of your copy of the Inkscape source.

# ( For details on these build flags, see below "Build Flags" )
cmake -G Ninja -DCMAKE_INSTALL_PREFIX="${PWD}/install_dir" -DCMAKE_BUILD_TYPE=Debug -DBUILD_SHARED_LIBS=OFF -DWITH_INTERNAL_2GEOM=ON  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON ..

# start the compilation and
# install compiled files and all dependencies required to run Inkscape into the
# folder 'build/inkscape/':
ninja install

# Run Inkscape
./install_dir/bin/inkscape.exe
```

_**That's it!**_  
Afterwards, you should have a complete binary distribution of Inkscape in the folder "build/install_dir/" that can be run on any machine running Windows 8.1 or later.

## Running Inkscape

Simply execute `inkscape.exe` from the "build/**install_dir**/bin" directory (not "build/bin") created in the previous step. (The `ninja install` command takes care of copying all required files into this directory.)

## Packaging

If you only want to run Inkscape, you do _not_ need to follow these instructions. See "Running Inkscape" above.

To package those files for distribution (to give it to other people, or make a setup file for other computers), you can use the following commands:

*   `ninja dist-win-7z` – generate binary 7z archive.
*   `ninja dist-win-exe` – generate .exe installer.
*   `ninja dist-win-msi` – generate .msi installer.
*   Additionally there's a `dist-win-all` target (executes all of the above in parallel).

For some additional details which have not been incorporated into this page yet, see the previous instructions at https://wiki.inkscape.org/wiki/Compiling_Inkscape_on_Windows_32-bit#Creating_an_installer (mostly outdated).

## Installing

To install the self-built Inkscape, generate an EXE or MSI installer as described above and run it.

## See also
- [PROOF project overview](../../README.md)
- [Advanced Information on Compiling Inkscape](general_advanced.md)

## Troubleshooting

### Issues with MSYS2

The MSYS2 shell does not open.

Try rebooting.

**I have trouble updating an existing, older MSYS2 installation**

*   Check https://www.msys2.org/news/ for any recent news that may describe your issue and explain how to solve it.
*   The most straightforward solution is often to re-install MSYS2:
    *   Uninstall the old MSYS2. Make sure to completely remove the C:\\msys64 directory if the uninstaller does not do the job but remember to back up any personal data such as your home folder C:\\msys64\\home\\)
    *   Download a fresh installer from https://www.msys2.org/ and start over as described above.

### Issues with building Inkscape

The command \`ninja\` errors out after an MSYS2 update or after pulling new changes from the source reposiotry

Re-run CMake using `rm -rf CMakeCache.txt && cmake -G Ninja ..`.

The first command will delete cached and potentially stale info from the previous run, the second command will run CMake again to update the ninja files.

  
☎ _If you can't solve your issue with the information above, please [ask in the chat](https://chat.inkscape.org/channel/team_devel) or [report a bug](https://inkscape.org/report)_.

## Installing Build Dependencies Manually

**Normally this is not needed.** See "Install Dependencies Easily" above.

For older versions before 1.5.0, or if you do not want the automatic installation, you can also execute all the installation steps by hand:

*   **Step 1** — Install MSYS2
    1.  Download the installer from the [MSYS2 homepage](http://www.msys2.org/) (a 64-bit version of Windows 7 or later is required to run MSYS2 and build Inkscape).  
        Start the installation and follow the instructions on screen. Use the standard installation directory `C:\msys64` .
        Wait until it finishes.
    2.  Start an MSYS2 **MSYS** shell from the start menu (or launch "msys2.exe" in the installation directory).
    3.  Execute the command `pacman -Syuu`. This will start a full system upgrade and ensures that you have the latest versions of all core libraries.
    4.  Repeat the previous step until no new updates are found.
*   **Step 2** — Download dependencies
    1.  Close the MSYS2 **MSYS** shell.
    2.  **Read carefully:** Start an MSYS2 **UCRT64** (!!!) shell from the start menu (or launch "ucrt64.exe" in the installation directory). Using the right shell type is important for ensuring that the correct dependencies are installed.
    3.  **Double-check:** The shell window must show "**UCRT64**" in purple text. If it shows "MSYS" then you have the wrong shell, please go back.
    4.  Execute the command `curl https://gitlab.com/inkscape/inkscape/-/raw/master/buildtools/msys2installdeps.sh | bash`
        (The command downloads and runs the script [msys2installdeps.sh](https://gitlab.com/inkscape/inkscape/blob/master/buildtools/msys2installdeps.sh). Alternatively, you can copy-paste the script into the console or download it, change to the folder containing the file and type `./msys2installdeps.sh`. If you already have a copy of the Inkscape source it should also be included in the "buildtools" folder.)
    5.  Relax and take a break, as this may take some time (a few minutes at best, but it can take significantly longer if you have a slow internet connection or the server load is high).
*   **Step 3** - Install dependencies for creating the installer packages (.EXE, .MSI, .7Z) This step is optional.
    1.  .7z: [7-Zip](http://www.7-zip.org/).
    2.  .exe: [Nullsoft Scriptable Install System (NSIS)](http://nsis.sourceforge.net/) version 3 or later.
    3.  .msi:
        *   You'll need to install [Windows Installer XML (WiX Toolset)](http://wixtoolset.org/) version 4 (for Inkscape 1.4.1 and above). To install it:
            *   In the start menu, type `powershell` and click on "Run as Administrator"
            *   Open Notepad. Open the file buildtools/windows-deps-install-wix4.ps1 from the Inkscape sources. Select all. Copy.
            *   Click in the powershell window with the right mouse button to Paste and run the commands.
        *   For older versions up to 1.4.0, you will instead need WiX version 3 from [https://github.com/wixtoolset/wix3/releases](https://github.com/wixtoolset/wix3/releases)

## Build flags

Some background information on why the build flags were chosen.

- `-DBUILD_SHARED_LIBS=OFF` avoids trying to build with [too many debug symbols](https://stackoverflow.com/questions/47135973/error-export-ordinal-too-large-104116).
- `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` results in `build/compile_commands.json`. IntelliSense in VSCode uses this to avoid reporting false errors in the code.
- The other build flags are the recommended default also for [Linux](linux.md).
