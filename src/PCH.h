#pragma once

#include "RE/Skyrim.h"
#include "SKSE/SKSE.h"

#define AE_OFFSET(a_ae, a_ae1799) (REL::Module::get().version() >= REL::Version(1, 7, 99, 0) ? (a_ae1799) : (a_ae))

#ifndef LF_ENABLE_PROFILING
#define LF_ENABLE_PROFILING 0
#endif

#if LF_ENABLE_PROFILING
#include "Profiling/Profiler.h"
#else
#define LF_PROFILE_SCOPE(a_name) ((void)0)
#define LF_PROFILE_FUNCTION() ((void)0)
#define LF_PROFILE_REPORT() ((void)0)
#endif

using namespace std::literals;
