#ifndef FS_TNFS_H_
#define FS_TNFS_H_

// TNFS network filesystem backend — implements fs_backend.h for FS_BACKEND=TNFS builds.
// Only directory enumeration is implemented in this version.
// File open/read/seek/close/stat return suitable read-only errors until a later milestone.
//
// Select at build time:  cmake -DFS_BACKEND=TNFS  (default)
//                        cmake -DFS_BACKEND=MEMORY
//
// No additional public API; include fs_backend.h for the shared interface.

#endif // FS_TNFS_H_
