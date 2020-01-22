Deterministic builds with LLVM's GN build
=========================================

Summary: Use the following args.gn, and create a bunch of symlinks so that
the relative compiler path and the default relative symlink paths work.

    clang_base_path = "../../llvm/utils/gn/toolchain/win"
    use_deterministic_relative_paths_in_debug_info = true
    use_sysroot = true

It is possible to produce [universally deterministic][1] builds of LLVM
with the GN build. It requires some configuration though.

1. Make debug info use relative paths by setting
   `use_deterministic_relative_paths_in_debug_info = true` in your `args.gn`
   file. With this set, current debuggers need minor configuration to keep
   working.  See "Getting to local determinism" and "Getting debuggers to work
   well with locally deterministic builds" in the [deterministic builds][1]
   documentation for details.

2. Make the build use a sysroot for headers by setting `use_sysroot = true`
   in your `args.gn` file. You need to create a symlink to your sysroot
   at a fixed location for things to work.

    * Linux: `ln -s .../real/path/to/sysroot llvm/utils/gn/sysroot/linux/x64`
    * Mac: `ln -s .../real/path/to/mac/sdk llvm/utils/gn/sysroot/mac`. You could
      use `$(xcrun -show-sdk-path)` as `../real/path/to/mac/sdk`, but the idea
      in using a sysroot is that you don't need to register your sdk of choice
      with `xcrun`, so normally it'd just be a local directory containing a
      fixed SDK.
    * Windows: (XXX also, dia) (/h needs privs; /j doesn't)

        >mkdir llvm\utils\gn\sysroot
        >mkdir llvm\utils\gn\sysroot\win
        >mkdir llvm\utils\gn\sysroot\win\sdk
        >mkdir llvm\utils\gn\sysroot\win\sdk\include
        
        >mklink /j llvm\utils\gn\sysroot\win\sdk\include\ucrt c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Include\10.0.18362.0\ucrt
        Junction created for llvm\utils\gn\sysroot\win\sdk\include\ucrt <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Include\10.0.18362.0\ucrt

        >mklink /j llvm\utils\gn\sysroot\win\sdk\include\shared c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Include\10.0.18362.0\shared
        Junction created for llvm\utils\gn\sysroot\win\sdk\include\shared <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Include\10.0.18362.0\shared
        
        >mklink /j llvm\utils\gn\sysroot\win\sdk\include\um c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Include\10.0.18362.0\um
        Junction created for llvm\utils\gn\sysroot\win\sdk\include\um <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Include\10.0.18362.0\um
        
        >mklink /j llvm\utils\gn\sysroot\win\sdk\include\winrt c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Include\10.0.18362.0\winrt
        Junction created for llvm\utils\gn\sysroot\win\sdk\include\winrt <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Include\10.0.18362.0\winrt
        
        
        >mkdir llvm\utils\gn\sysroot\win\sdk\lib
        
        >mklink /j llvm\utils\gn\sysroot\win\sdk\lib\ucrt c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Lib\10.0.18362.0\ucrt
        Junction created for llvm\utils\gn\sysroot\win\sdk\lib\ucrt <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Lib\10.0.18362.0\ucrt
        
        >mklink /j llvm\utils\gn\sysroot\win\sdk\lib\shared c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Lib\10.0.18362.0\shared
        Junction created for llvm\utils\gn\sysroot\win\sdk\lib\shared <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Lib\10.0.18362.0\shared

        >mklink /j llvm\utils\gn\sysroot\win\sdk\lib\um c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Lib\10.0.18362.0\um
        Junction created for llvm\utils\gn\sysroot\win\sdk\lib\um <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Lib\10.0.18362.0\um
        
        >mklink /j llvm\utils\gn\sysroot\win\sdk\lib\winrt c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Lib\10.0.18362.0\winrt
        Junction created for llvm\utils\gn\sysroot\win\sdk\lib\winrt <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\win_sdk\Lib\10.0.18362.0\winrt
        
        
        >mkdir llvm\utils\gn\sysroot\win\msvc
        
        >mklink /j llvm\utils\gn\sysroot\win\msvc\include c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\VC\Tools\MSVC\14.23.28105\include
        Junction created for llvm\utils\gn\sysroot\win\msvc\include <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\VC\Tools\MSVC\14.23.28105\include
        
        >mklink /j llvm\utils\gn\sysroot\win\msvc\lib c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\VC\Tools\MSVC\14.23.28105\lib
        Junction created for llvm\utils\gn\sysroot\win\msvc\lib <<===>> c:\src\chrome\src\third_party\depot_tools\win_toolchain\vs_files\8f58c55897a3282ed617055775a77ec3db771b88\VC\Tools\MSVC\14.23.28105\lib


3. Use the same host compiler / linker. (FIXME: Bootstrap builds)
   FIXME: Also need a relative path to this! Else won't ever get cache hits
   from what I can tell. Use
   `clang_base_path = "../../llvm/utils/gn/toolchain/win"`

        >mklink /j llvm\utils\gn\toolchain\win "c:\src\chrome\src\third_party\llvm-build\Release+Asserts"
        Junction created for llvm\utils\gn\toolchain\win <<===>> c:\src\chrome\src\third_party\llvm-build\Release+Asserts

4. XXX mac ld64

1: http://blog.llvm.org/2019/11/deterministic-builds-with-clang-and-lld.html
