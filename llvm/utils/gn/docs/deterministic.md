Deterministic builds with LLVM's GN build
=========================================

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
    * Windows: XXX `mklink /j ...` (or /h? measure perf...) (XXX also, dia)

3. `llvm_allow_tardy_revision` FIXME (probably set explicit value?)

4. Use the same host compiler / linker. (FIXME: Bootstrap builds)

5. XXX mac ld64

1: http://blog.llvm.org/2019/11/deterministic-builds-with-clang-and-lld.html
