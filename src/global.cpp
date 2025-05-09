/*!
\file
Provide static instance for global configuration and control data
*/

#include "global.h"

std::unique_ptr<global> global::instance{new global};
