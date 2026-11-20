#include "ccompiler/build.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The x86-64 backend targets whichever object format the host toolchain
 * accepts, so the driver only needs to pin the architecture when the host
 * itself is not x86-64. */
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
#define CC_BUILD_ARCHITECTURE_ARGUMENTS "-arch", "x86_64",
#else
#define CC_BUILD_ARCHITECTURE_ARGUMENTS
#endif

int cc_build_executable_from_assembly(const char *assembly_text, const char *output_path) {
    char assembly_path[1024];
    const char *temporary_directory;
    FILE *assembly_file;
    size_t written;
    pid_t child;
    int status;

    temporary_directory = getenv("TMPDIR");
    if (temporary_directory == NULL || temporary_directory[0] == '\0') {
        temporary_directory = "/tmp";
    }

    snprintf(assembly_path, sizeof(assembly_path), "%s/ccompiler-%ld.s", temporary_directory, (long)getpid());

    assembly_file = fopen(assembly_path, "w");
    if (assembly_file == NULL) {
        fprintf(stderr, "error: cannot write temporary assembly to %s\n", assembly_path);
        return 1;
    }

    written = fwrite(assembly_text, 1, strlen(assembly_text), assembly_file);
    if (written != strlen(assembly_text) || fclose(assembly_file) != 0) {
        fprintf(stderr, "error: cannot write temporary assembly to %s\n", assembly_path);
        remove(assembly_path);
        return 1;
    }

    child = fork();
    if (child < 0) {
        fprintf(stderr, "error: cannot start the system assembler\n");
        remove(assembly_path);
        return 1;
    }

    if (child == 0) {
        execlp("cc", "cc", CC_BUILD_ARCHITECTURE_ARGUMENTS "-x", "assembler", assembly_path,
               "-o", output_path, (char *)NULL);
        fprintf(stderr, "error: cannot run 'cc' to assemble and link\n");
        _exit(127);
    }

    if (waitpid(child, &status, 0) < 0) {
        fprintf(stderr, "error: cannot wait for the system assembler\n");
        remove(assembly_path);
        return 1;
    }

    remove(assembly_path);

    if (!WIFEXITED(status)) {
        return 1;
    }

    return WEXITSTATUS(status);
}
