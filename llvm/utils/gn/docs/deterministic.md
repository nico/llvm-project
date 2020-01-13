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

2. Set up a symlink

3. `llvm_allow_tardy_revision` FIXME (probably set explicit value?)

4. Use the same host compiler. (FIXME: Bootstrap builds)

1: http://blog.llvm.org/2019/11/deterministic-builds-with-clang-and-lld.html
