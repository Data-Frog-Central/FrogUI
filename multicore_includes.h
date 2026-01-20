#ifndef MULTICORE_INCLUDES_H
#define MULTICORE_INCLUDES_H
#include <stdio.h>
#include <ctype.h>

#ifdef SF2000
#include "../../../stockfw.h"
#include "../../../debug.h"
#include "../../../core.h"
#include "../../../dirent.h"
#else
#include <dirent.h>
#define xlog printf // For non-SF2000 builds, use printf as fallback
#endif

#endif