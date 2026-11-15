set(LLVM_TARGET_DEFINITIONS Options.td)
llvm_tablegen(Options.inc -gen-opt-parser-defs)
add_public_tablegen_target(Dsymutil2TableGen)

set(LLVM_LINK_COMPONENTS
  Option
  Support
  )

add_llvm_tool(dsymutil2
  dsymutil2.cpp

  DEPENDS
  Dsymutil2TableGen
  )
