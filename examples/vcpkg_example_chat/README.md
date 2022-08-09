The purpose of this project is to demonstrate/test a project that pulls in
GameNetworkingSockets and its dependencies entirely using vcpkg.

# TL;DR

We assume that your project defines its dependences in a `vcpkg.json` file, and
you are pulling in vcpkg's cmake toolchain file.  Then all you need to do is:

* Add `gamenetworkingsockets` as a dependency in your project's `vcpkg.json`
* In your cmake file, add `find_package( GameNetworkingSockets )`
* Link your targets with the appropriate library:
  * `target_link_libraries(<your_target> GameNetworkingSockets::shared)`
  * or `target_link_libraries(<your_target> GameNetworkingSockets::static)`

# Building the example project using vcpkg in manifest mode

Here are some specific instructions that build the example project using vcpkg in
["manifest mode"](https://vcpkg.readthedocs.io/en/latest/users/manifests/).
This example assumes Windows, and also it's recommended to run this from a
Visual Studio command prompt so we know what compiler to use.

First, we bootstrap a project-specific installation of vcpkg ("manifest mode")
in the default location, `<project root>/vcpkg`.  From the project root, run these
commands:

```
> git clone https://github.com/microsoft/vcpkg
> .\vcpkg\bootstrap-vcpkg.bat
```

Now we ask vcpkg to install of the dependencies for our project, which are described by
the file `<project root>/vcpkg.json`.  Note that this step is optional, as cmake will
automatically do this.  But here we are doing it in a separate step so that we can isolate
any problems, because if problems happen here don't have anything to do with your
cmake files.

```
> .\vcpkg\vcpkg install --triplet=x64-windows
```

Next build the project files.  There are different options for telling cmake how
to integrate with vcpkg; here we use `CMAKE_TOOLCHAIN_FILE` on the command line.
Also we select Ninja project generator.

```
> cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=vcpkg\scripts\buildsystems\vcpkg.cmake
```

Finally, build the project:

```
> cd build
> ninja
```
