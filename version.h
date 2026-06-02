#ifndef MARINE_DISPLAYS_VERSION_H
#define MARINE_DISPLAYS_VERSION_H

#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 0
#define FW_VERSION_PATCH 0

// Human-readable base version (bump manually for releases)
#define FW_VERSION_BASE "1.0.0"

// FW_VERSION is set by the build script to include the git hash,
// e.g. "1.0.0-85896d6".  If not defined (standalone compile),
// fall back to the base version string.
#ifndef FW_VERSION
#define FW_VERSION FW_VERSION_BASE
#endif

#endif // MARINE_DISPLAYS_VERSION_H
