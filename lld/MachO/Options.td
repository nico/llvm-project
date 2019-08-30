include "llvm/Option/OptParser.td"

def L: JoinedOrSeparate<["-"], "L">, MetaVarName<"<dir>">,
  HelpText<"Add directory to library search path">;

def arch: Separate<["-"], "arch">, MetaVarName<"<arch-name>">,
  HelpText<"Architecture to link">;

def e: Separate<["-"], "e">, HelpText<"Name of entry point symbol">;

def l: Joined<["-"], "l">, MetaVarName<"<libname>">,
  HelpText<"Base name of library searched for in -L directories">;

def o: Separate<["-"], "o">, MetaVarName<"<path>">,
  HelpText<"Path to file to write output">;

def syslibroot: Separate<["-"], "syslibroot">, MetaVarName<"<dir>">,
  HelpText<"Add path to SDK to all absolute library search paths">;

// Ignored options
def: Flag<["-"], "dead_strip">;
def: Flag<["-"], "demangle">;
def: Flag<["-"], "dynamic">;
def: Flag<["-"], "headerpad_max_install_names">;
def: Flag<["-"], "search_paths_first">;
def: Separate<["-"], "lto_library">;
def: Separate<["-"], "macosx_version_min">;
def: Separate<["-"], "rpath">;
