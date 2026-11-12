// A translation unit exists for one reason: AUTOMOC only scans sources listed
// in the build, and this header carries Q_OBJECT (the contract includes
// signals). Nothing else lives here -- the moc output is generated from the
// header and compiled into the same object by AUTOMOC.
#include "gui/ioverlayhost.h"
