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
