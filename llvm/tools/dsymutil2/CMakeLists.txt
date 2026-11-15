set(LLVM_TARGET_DEFINITIONS Options.td)
llvm_tablegen(Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(Dsymutil2TableGen)

set(LLVM_LINK_COMPONENTS
  BinaryFormat
  DebugInfoDWARF
  Object
  Option
  Support
  )

add_llvm_tool(dsymutil2
  dsymutil2.cpp
  AccelTable.cpp
  DebugMap.cpp
  DsymWriter.cpp
  Dwarf.cpp
  DwarfLinker.cpp
  FrameTable.cpp
  LineTable.cpp
  MachOFile.cpp
  ObjectFile.cpp
  StringTable.cpp

  DEPENDS
  Dsymutil2TableGen
  )
