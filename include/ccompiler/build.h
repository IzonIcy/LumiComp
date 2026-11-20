#ifndef CCOMPILER_BUILD_H
#define CCOMPILER_BUILD_H

/* Assembles and links assembly text with the system C toolchain.
 * Returns the toolchain's exit status, or 1 if the toolchain could not be
 * started. The temporary assembly file is removed before returning. */
int cc_build_executable_from_assembly(const char *assembly_text, const char *output_path);

#endif
