include "llvm/Option/OptParser.td"

class F<string name>: Flag<["--", "-"], name>;

def grp_general : OptionGroup<"dsymutil2">, HelpText<"dsymutil2 Options">;

def help: F<"help">,
  HelpText<"Prints this help output.">,
  Group<grp_general>;
def: Flag<["-"], "h">,
  Alias<help>,
  HelpText<"Alias for --help">,
  Group<grp_general>;

def version: F<"version">,
  HelpText<"Prints the dsymutil2 version.">,
  Group<grp_general>;

def output: Separate<["-", "--"], "o">,
  MetaVarName<"<path>">,
  HelpText<"Specify the dSYM bundle to write. Defaults to <input file>.dSYM">,
  Group<grp_general>;

def flat: F<"flat">,
  HelpText<"Write the dSYM's Mach-O file to the output path, without a "
           "bundle around it. The default path is <input file>.dwarf; - is "
           "the standard output.">,
  Group<grp_general>;
def: Flag<["-"], "f">,
  Alias<flat>,
  HelpText<"Alias for --flat">,
  Group<grp_general>;

def yaml_input: Flag<["-", "--"], "y">,
  HelpText<"Treat the input file as a debug map in YAML, as "
           "`dsymutil --dump-debug-map` prints it, and not as a binary.">,
  Group<grp_general>;

def linker: Separate<["--", "-"], "linker">,
  MetaVarName<"<DWARF linker type>">,
  HelpText<"Ignored: dsymutil chooses one of its two DWARF linkers with "
           "it.">,
  Flags<[HelpHidden]>,
  Group<grp_general>;
def: Joined<["--", "-"], "linker=">, Alias<linker>;

def threads: Separate<["--", "-"], "num-threads">,
  MetaVarName<"<threads>">,
  HelpText<"Specify the number of threads to use. Defaults to all cores.">,
  Group<grp_general>;
def: Separate<["-"], "j">,
  Alias<threads>,
  MetaVarName<"<threads>">,
  HelpText<"Alias for --num-threads">,
  Group<grp_general>;

def oso_prepend_path: Separate<["--", "-"], "oso-prepend-path">,
  MetaVarName<"<path>">,
  HelpText<"Specify a directory to prepend to the paths of object files.">,
  Group<grp_general>;
def: Joined<["--", "-"], "oso-prepend-path=">, Alias<oso_prepend_path>;

def allow_section_header_offset_overflow:
  F<"allow-section-header-offset-overflow">,
  HelpText<"Write the dSYM even if a section starts past 4 GB in it. A "
           "Mach-O section header cannot say so, and not every reader finds "
           "such a section.">,
  Group<grp_general>;

def section_offset_limit: Separate<["--"], "section-offset-limit">,
  MetaVarName<"<bytes>">,
  HelpText<"For tests: take this as the first file offset that a section "
           "header cannot hold, instead of 4 GiB.">,
  Flags<[HelpHidden]>,
  Group<grp_general>;

def dump_debug_map: F<"dump-debug-map">,
  HelpText<"Parse and dump the debug map to standard output. No DWARF link "
           "will take place.">,
  Group<grp_general>;

def time_phases: F<"time-phases">,
  HelpText<"Print how long each phase takes.">,
  Group<grp_general>;
