cmake_minimum_required(VERSION 3.23)
project(Kaleidoscope)
set(CMAKE_CXX_STANDARD 23)
set(LLVM_DIR "H:/LLVM/lib/cmake/llvm")

set(ZLIB_LIBRARY "H:/libz/lib/libz.a")
set(ZLIB_INCLUDE_DIR "H:/libz/include")

set(LIBXML2_INCLUDE_DIR "H:/libxml2/include")
set(LIBXML2_LIBRARY "H:/libxml2/lib/libxml2.a")

find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
# Set your project compile flags.
# E.g. if using the C++ header files
# you will need to enable C++11 support
# for your compiler.
include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

link_directories("H:/llvm/lib")
link_directories("H:/libxml2/lib")
link_directories("H:/zlib/lib")

add_executable(Kaleidoscope main.cpp)
# Find the libraries that correspond to the LLVM components
# that we wish to use
# Link against LLVM libraries
target_link_libraries(Kaleidoscope LLVMWindowsManifest LLVMXRay LLVMLibDriver LLVMDlltoolDriver LLVMCoverage LLVMLineEditor LLVMXCoreDisassembler LLVMXCoreCodeGen LLVMXCoreDesc LLVMXCoreInfo LLVMX86TargetMCA LLVMX86Disassembler LLVMX86AsmParser LLVMX86CodeGen LLVMX86Desc LLVMX86Info LLVMWebAssemblyDisassembler LLVMWebAssemblyAsmParser LLVMWebAssemblyCodeGen LLVMWebAssemblyDesc LLVMWebAssemblyUtils LLVMWebAssemblyInfo LLVMVEDisassembler LLVMVEAsmParser LLVMVECodeGen LLVMVEDesc LLVMVEInfo LLVMSystemZDisassembler LLVMSystemZAsmParser LLVMSystemZCodeGen LLVMSystemZDesc LLVMSystemZInfo LLVMSparcDisassembler LLVMSparcAsmParser LLVMSparcCodeGen LLVMSparcDesc LLVMSparcInfo LLVMRISCVDisassembler LLVMRISCVAsmParser LLVMRISCVCodeGen LLVMRISCVDesc LLVMRISCVInfo LLVMPowerPCDisassembler LLVMPowerPCAsmParser LLVMPowerPCCodeGen LLVMPowerPCDesc LLVMPowerPCInfo LLVMNVPTXCodeGen LLVMNVPTXDesc LLVMNVPTXInfo LLVMMSP430Disassembler LLVMMSP430AsmParser LLVMMSP430CodeGen LLVMMSP430Desc LLVMMSP430Info LLVMMipsDisassembler LLVMMipsAsmParser LLVMMipsCodeGen LLVMMipsDesc LLVMMipsInfo LLVMLanaiDisassembler LLVMLanaiCodeGen LLVMLanaiAsmParser LLVMLanaiDesc LLVMLanaiInfo LLVMHexagonDisassembler LLVMHexagonCodeGen LLVMHexagonAsmParser LLVMHexagonDesc LLVMHexagonInfo LLVMBPFDisassembler LLVMBPFAsmParser LLVMBPFCodeGen LLVMBPFDesc LLVMBPFInfo LLVMAVRDisassembler LLVMAVRAsmParser LLVMAVRCodeGen LLVMAVRDesc LLVMAVRInfo LLVMARMDisassembler LLVMARMAsmParser LLVMARMCodeGen LLVMARMDesc LLVMARMUtils LLVMARMInfo LLVMAMDGPUTargetMCA LLVMAMDGPUDisassembler LLVMAMDGPUAsmParser LLVMAMDGPUCodeGen LLVMAMDGPUDesc LLVMAMDGPUUtils LLVMAMDGPUInfo LLVMAArch64Disassembler LLVMAArch64AsmParser LLVMAArch64CodeGen LLVMAArch64Desc LLVMAArch64Utils LLVMAArch64Info LLVMOrcJIT LLVMMCJIT LLVMJITLink LLVMInterpreter LLVMExecutionEngine LLVMRuntimeDyld LLVMOrcTargetProcess LLVMOrcShared LLVMDWP LLVMSymbolize LLVMDebugInfoPDB LLVMDebugInfoGSYM LLVMOption LLVMObjectYAML LLVMMCA LLVMMCDisassembler LLVMLTO LLVMPasses LLVMCFGuard LLVMCoroutines LLVMObjCARCOpts LLVMipo LLVMVectorize LLVMLinker LLVMInstrumentation LLVMFrontendOpenMP LLVMFrontendOpenACC LLVMExtensions LLVMDWARFLinker LLVMGlobalISel LLVMMIRParser LLVMAsmPrinter LLVMDebugInfoMSF LLVMSelectionDAG LLVMCodeGen LLVMIRReader LLVMAsmParser LLVMInterfaceStub LLVMFileCheck LLVMFuzzMutate LLVMTarget LLVMScalarOpts LLVMInstCombine LLVMAggressiveInstCombine LLVMTransformUtils LLVMBitWriter LLVMAnalysis LLVMProfileData LLVMDebugInfoDWARF LLVMObject LLVMTextAPI LLVMMCParser LLVMMC LLVMDebugInfoCodeView LLVMBitReader LLVMCore LLVMRemarks LLVMBitstreamReader LLVMBinaryFormat LLVMTableGen LLVMSupport LLVMDemangle psapi shell32 ole32 uuid advapi32 z xml2)