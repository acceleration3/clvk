// Copyright 2026 The clvk authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "cl_headers.hpp"

#include "CL/cl_gl.h"

// cl_khr_gl_sharing, by copy.
//
// clvk sits on Vulkan and the GL implementation it shares with may be
// anything at all, so there is no handle the two can agree on. What there
// always is, is pixels: acquiring an object reads it out of GL and writes
// it into the OpenCL object, releasing it does the reverse. The sharing an
// application asks for is honoured; only the zero copy it hopes for is not.
//
// Everything here needs the caller's GL context to be current on the
// calling thread, which is what the extension already requires of
// clEnqueueAcquireGLObjects and friends.

// Whether a GL library was found and its entry points resolved. Called to
// decide whether the extension is worth advertising.
bool cvk_gl_sharing_available();
