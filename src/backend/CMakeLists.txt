# The backend is named after the LLVM it uses, because the point of the design
# record is that *which* backend is a decision and not a default (`codegen.md`,
# § *Non-goals*, on writing our own). A second backend would be a sibling
# directory and not a rewrite of this one.
add_subdirectory(llvm)
