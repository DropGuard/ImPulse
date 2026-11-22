#include "appversion.h"

// The version rides in static storage: a QVersionNumber allocates, so the
// initializer can in principle throw before main. Every consumer reads it
// after QCoreApplication is up, and a bad_alloc at that scale ends the
// process either way - NOLINT keeps the checker armed for every other
// static initializer in the tree.
QVersionNumber appVersion(0, 1, 0); // NOLINT(bugprone-throwing-static-initialization)
