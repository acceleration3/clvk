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

#include "gl_sharing.hpp"

#include "config.hpp"
#include "log.hpp"

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#define CVK_GLAPI __stdcall
#else
#include <dlfcn.h>
#define CVK_GLAPI
#endif

// ---------------------------------------------------------------------------
// The slice of GL this needs
//
// Declared here rather than included: pulling in a GL header would make the
// build depend on one, and all that is wanted is a dozen entry points that
// have not changed since 1.5.
// ---------------------------------------------------------------------------

using CvkGLenum = unsigned int;
using CvkGLint = int;
using CvkGLuint = unsigned int;
using CvkGLsizei = int;
using CvkGLintptr = intptr_t;
using CvkGLsizeiptr = intptr_t;

#define GL_NO_ERROR 0
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_RECTANGLE 0x84F5
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_TEXTURE_BINDING_1D 0x8068
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_TEXTURE_BINDING_3D 0x806A
#define GL_TEXTURE_BINDING_RECTANGLE 0x84F6
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514

#define GL_TEXTURE_WIDTH 0x1000
#define GL_TEXTURE_HEIGHT 0x1001
#define GL_TEXTURE_DEPTH 0x8071
#define GL_TEXTURE_INTERNAL_FORMAT 0x1003

#define GL_PACK_ALIGNMENT 0x0D05
#define GL_UNPACK_ALIGNMENT 0x0CF5

#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RG 0x8227

#define GL_BYTE 0x1400
#define GL_UNSIGNED_BYTE 0x1401
#define GL_SHORT 0x1402
#define GL_UNSIGNED_SHORT 0x1403
#define GL_INT 0x1404
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B

#define GL_R8 0x8229
#define GL_R8_SNORM 0x8F94
#define GL_R16 0x822A
#define GL_R16F 0x822D
#define GL_R32F 0x822E
#define GL_R8I 0x8231
#define GL_R8UI 0x8232
#define GL_R16I 0x8233
#define GL_R16UI 0x8234
#define GL_R32I 0x8235
#define GL_R32UI 0x8236
#define GL_RG8 0x822B
#define GL_RG16 0x822C
#define GL_RG16F 0x822F
#define GL_RG32F 0x8230
#define GL_RGB8 0x8051
#define GL_RGB16F 0x881B
#define GL_RGB32F 0x8815
#define GL_RGBA8 0x8058
#define GL_RGBA8_SNORM 0x8F97
#define GL_RGBA16 0x805B
#define GL_RGBA16F 0x881A
#define GL_RGBA32F 0x8814
#define GL_RGBA8I 0x8D8E
#define GL_RGBA8UI 0x8D7C
#define GL_RGBA16I 0x8D88
#define GL_RGBA16UI 0x8D76
#define GL_RGBA32I 0x8D82
#define GL_RGBA32UI 0x8D70
#define GL_SRGB8_ALPHA8 0x8C43

#define GL_ARRAY_BUFFER 0x8892
#define GL_ARRAY_BUFFER_BINDING 0x8894
#define GL_BUFFER_SIZE 0x8764

#define GL_RENDERBUFFER 0x8D41
#define GL_RENDERBUFFER_BINDING 0x8CA7
#define GL_RENDERBUFFER_WIDTH 0x8D42
#define GL_RENDERBUFFER_HEIGHT 0x8D43
#define GL_RENDERBUFFER_INTERNAL_FORMAT 0x8D44
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_COLOR_ATTACHMENT0 0x8CE0

namespace {

struct cvk_gl_api {
    CvkGLenum(CVK_GLAPI* GetError)();
    void(CVK_GLAPI* Finish)();
    void(CVK_GLAPI* GetIntegerv)(CvkGLenum, CvkGLint*);
    void(CVK_GLAPI* PixelStorei)(CvkGLenum, CvkGLint);
    void(CVK_GLAPI* BindTexture)(CvkGLenum, CvkGLuint);
    void(CVK_GLAPI* GetTexImage)(CvkGLenum, CvkGLint, CvkGLenum, CvkGLenum,
                                 void*);
    void(CVK_GLAPI* GetTexLevelParameteriv)(CvkGLenum, CvkGLint, CvkGLenum,
                                            CvkGLint*);
    void(CVK_GLAPI* TexSubImage2D)(CvkGLenum, CvkGLint, CvkGLint, CvkGLint,
                                   CvkGLsizei, CvkGLsizei, CvkGLenum, CvkGLenum,
                                   const void*);
    void(CVK_GLAPI* ReadPixels)(CvkGLint, CvkGLint, CvkGLsizei, CvkGLsizei,
                                CvkGLenum, CvkGLenum, void*);
    // 1.2 and later, resolved through the platform's own getter.
    void(CVK_GLAPI* TexSubImage3D)(CvkGLenum, CvkGLint, CvkGLint, CvkGLint,
                                   CvkGLint, CvkGLsizei, CvkGLsizei, CvkGLsizei,
                                   CvkGLenum, CvkGLenum, const void*);
    void(CVK_GLAPI* BindBuffer)(CvkGLenum, CvkGLuint);
    void(CVK_GLAPI* GetBufferSubData)(CvkGLenum, CvkGLintptr, CvkGLsizeiptr,
                                      void*);
    void(CVK_GLAPI* BufferSubData)(CvkGLenum, CvkGLintptr, CvkGLsizeiptr,
                                   const void*);
    void(CVK_GLAPI* GetBufferParameteriv)(CvkGLenum, CvkGLenum, CvkGLint*);
    void(CVK_GLAPI* BindRenderbuffer)(CvkGLenum, CvkGLuint);
    void(CVK_GLAPI* GetRenderbufferParameteriv)(CvkGLenum, CvkGLenum,
                                                CvkGLint*);
    void(CVK_GLAPI* GenFramebuffers)(CvkGLsizei, CvkGLuint*);
    void(CVK_GLAPI* DeleteFramebuffers)(CvkGLsizei, const CvkGLuint*);
    void(CVK_GLAPI* BindFramebuffer)(CvkGLenum, CvkGLuint);
    void(CVK_GLAPI* FramebufferRenderbuffer)(CvkGLenum, CvkGLenum, CvkGLenum,
                                             CvkGLuint);
    void* (*GetCurrentContext)();

    bool core_loaded;
};

cvk_gl_api gGL;
std::once_flag gGLLoadOnce;

void* gl_library_symbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(
        ::GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void load_gl_api() {
    memset(&gGL, 0, sizeof(gGL));

#ifdef _WIN32
    // The GL an application uses is already in its address space by the time
    // it asks to share with it; LoadLibrary hands back that same module.
    void* lib = ::LoadLibraryA("opengl32.dll");
#else
    void* lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (lib == nullptr) {
        lib = dlopen("libGL.so", RTLD_LAZY | RTLD_LOCAL);
    }
#endif
    if (lib == nullptr) {
        cvk_info_fn("no GL library, cl_khr_gl_sharing will not be reported");
        return;
    }

    // Anything past GL 1.1 on Windows, and anything at all on some Linux
    // stacks, comes from the platform's own getter rather than the library.
    void*(CVK_GLAPI * platform_get_proc)(const char*) = nullptr;
#ifdef _WIN32
    platform_get_proc = reinterpret_cast<decltype(platform_get_proc)>(
        gl_library_symbol(lib, "wglGetProcAddress"));
    gGL.GetCurrentContext = reinterpret_cast<void* (*)()>(
        gl_library_symbol(lib, "wglGetCurrentContext"));
#else
    platform_get_proc = reinterpret_cast<decltype(platform_get_proc)>(
        gl_library_symbol(lib, "glXGetProcAddressARB"));
    if (platform_get_proc == nullptr) {
        platform_get_proc = reinterpret_cast<decltype(platform_get_proc)>(
            gl_library_symbol(lib, "glXGetProcAddress"));
    }
    gGL.GetCurrentContext = reinterpret_cast<void* (*)()>(
        gl_library_symbol(lib, "glXGetCurrentContext"));
#endif

    auto resolve = [lib, platform_get_proc](const char* name) -> void* {
        void* fn = gl_library_symbol(lib, name);
        if (fn == nullptr && platform_get_proc != nullptr) {
            fn = reinterpret_cast<void*>(platform_get_proc(name));
        }
        return fn;
    };

#define RESOLVE(field, name)                                                   \
    gGL.field = reinterpret_cast<decltype(gGL.field)>(resolve(name))

    RESOLVE(GetError, "glGetError");
    RESOLVE(Finish, "glFinish");
    RESOLVE(GetIntegerv, "glGetIntegerv");
    RESOLVE(PixelStorei, "glPixelStorei");
    RESOLVE(BindTexture, "glBindTexture");
    RESOLVE(GetTexImage, "glGetTexImage");
    RESOLVE(GetTexLevelParameteriv, "glGetTexLevelParameteriv");
    RESOLVE(TexSubImage2D, "glTexSubImage2D");
    RESOLVE(ReadPixels, "glReadPixels");
    RESOLVE(TexSubImage3D, "glTexSubImage3D");
    RESOLVE(BindBuffer, "glBindBuffer");
    RESOLVE(GetBufferSubData, "glGetBufferSubData");
    RESOLVE(BufferSubData, "glBufferSubData");
    RESOLVE(GetBufferParameteriv, "glGetBufferParameteriv");
    RESOLVE(BindRenderbuffer, "glBindRenderbuffer");
    RESOLVE(GetRenderbufferParameteriv, "glGetRenderbufferParameteriv");
    RESOLVE(GenFramebuffers, "glGenFramebuffers");
    RESOLVE(DeleteFramebuffers, "glDeleteFramebuffers");
    RESOLVE(BindFramebuffer, "glBindFramebuffer");
    RESOLVE(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
#undef RESOLVE

    // Textures are the part every application uses; buffers and
    // renderbuffers are checked where they are needed.
    gGL.core_loaded =
        gGL.Finish != nullptr && gGL.GetIntegerv != nullptr &&
        gGL.PixelStorei != nullptr && gGL.BindTexture != nullptr &&
        gGL.GetTexImage != nullptr && gGL.GetTexLevelParameteriv != nullptr &&
        gGL.TexSubImage2D != nullptr;

    cvk_info_fn("GL library loaded, entry points %s",
                gGL.core_loaded ? "resolved" : "missing");
}

const cvk_gl_api& gl() {
    std::call_once(gGLLoadOnce, load_gl_api);
    return gGL;
}

bool gl_context_current() {
    if (!gl().core_loaded) {
        return false;
    }
    // A stack whose getter could not be resolved is taken at its word: the
    // extension already makes a current context the caller's responsibility.
    if (gGL.GetCurrentContext == nullptr) {
        return true;
    }
    return gGL.GetCurrentContext() != nullptr;
}

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

struct cvk_gl_format {
    CvkGLenum internal_format;
    cl_channel_order order;
    cl_channel_type data_type;
    CvkGLenum transfer_format;
    CvkGLenum transfer_type;
    unsigned bytes_per_pixel;
};

// Three-channel GL formats are read and written as four: OpenCL has no
// three-channel image of that size, and GL is happy to add the alpha on the
// way out and drop it on the way back in.
const cvk_gl_format gFormats[] = {
    {GL_RGBA8, CL_RGBA, CL_UNORM_INT8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
    {GL_SRGB8_ALPHA8, CL_sRGBA, CL_UNORM_INT8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
    {GL_RGBA8_SNORM, CL_RGBA, CL_SNORM_INT8, GL_RGBA, GL_BYTE, 4},
    {GL_RGBA16, CL_RGBA, CL_UNORM_INT16, GL_RGBA, GL_UNSIGNED_SHORT, 8},
    {GL_RGBA16F, CL_RGBA, CL_HALF_FLOAT, GL_RGBA, GL_HALF_FLOAT, 8},
    {GL_RGBA32F, CL_RGBA, CL_FLOAT, GL_RGBA, GL_FLOAT, 16},
    {GL_RGBA8I, CL_RGBA, CL_SIGNED_INT8, GL_RGBA, GL_BYTE, 4},
    {GL_RGBA8UI, CL_RGBA, CL_UNSIGNED_INT8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
    {GL_RGBA16I, CL_RGBA, CL_SIGNED_INT16, GL_RGBA, GL_SHORT, 8},
    {GL_RGBA16UI, CL_RGBA, CL_UNSIGNED_INT16, GL_RGBA, GL_UNSIGNED_SHORT, 8},
    {GL_RGBA32I, CL_RGBA, CL_SIGNED_INT32, GL_RGBA, GL_INT, 16},
    {GL_RGBA32UI, CL_RGBA, CL_UNSIGNED_INT32, GL_RGBA, GL_UNSIGNED_INT, 16},
    {GL_RGB8, CL_RGBA, CL_UNORM_INT8, GL_RGBA, GL_UNSIGNED_BYTE, 4},
    {GL_RGB16F, CL_RGBA, CL_HALF_FLOAT, GL_RGBA, GL_HALF_FLOAT, 8},
    {GL_RGB32F, CL_RGBA, CL_FLOAT, GL_RGBA, GL_FLOAT, 16},
    {GL_RG8, CL_RG, CL_UNORM_INT8, GL_RG, GL_UNSIGNED_BYTE, 2},
    {GL_RG16, CL_RG, CL_UNORM_INT16, GL_RG, GL_UNSIGNED_SHORT, 4},
    {GL_RG16F, CL_RG, CL_HALF_FLOAT, GL_RG, GL_HALF_FLOAT, 4},
    {GL_RG32F, CL_RG, CL_FLOAT, GL_RG, GL_FLOAT, 8},
    {GL_R8, CL_R, CL_UNORM_INT8, GL_RED, GL_UNSIGNED_BYTE, 1},
    {GL_R8_SNORM, CL_R, CL_SNORM_INT8, GL_RED, GL_BYTE, 1},
    {GL_R16, CL_R, CL_UNORM_INT16, GL_RED, GL_UNSIGNED_SHORT, 2},
    {GL_R16F, CL_R, CL_HALF_FLOAT, GL_RED, GL_HALF_FLOAT, 2},
    {GL_R32F, CL_R, CL_FLOAT, GL_RED, GL_FLOAT, 4},
    {GL_R8I, CL_R, CL_SIGNED_INT8, GL_RED, GL_BYTE, 1},
    {GL_R8UI, CL_R, CL_UNSIGNED_INT8, GL_RED, GL_UNSIGNED_BYTE, 1},
    {GL_R16I, CL_R, CL_SIGNED_INT16, GL_RED, GL_SHORT, 2},
    {GL_R16UI, CL_R, CL_UNSIGNED_INT16, GL_RED, GL_UNSIGNED_SHORT, 2},
    {GL_R32I, CL_R, CL_SIGNED_INT32, GL_RED, GL_INT, 4},
    {GL_R32UI, CL_R, CL_UNSIGNED_INT32, GL_RED, GL_UNSIGNED_INT, 4},
};

const cvk_gl_format* find_format(CvkGLenum internal_format) {
    for (auto& fmt : gFormats) {
        if (fmt.internal_format == internal_format) {
            return &fmt;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// The objects handed out
// ---------------------------------------------------------------------------

struct cvk_gl_object {
    cl_gl_object_type type;
    CvkGLuint name;
    CvkGLenum target;      // what the object is bound to
    CvkGLenum bind_target; // where it is bound, a cube face binds the map
    CvkGLint miplevel;
    size_t width, height, depth;
    const cvk_gl_format* format;
    size_t size; // bytes, for buffers
};

std::mutex gObjectsLock;
std::unordered_map<cl_mem, cvk_gl_object> gObjects;

void CL_CALLBACK forget_gl_object(cl_mem memobj, void*) {
    std::lock_guard<std::mutex> lock(gObjectsLock);
    gObjects.erase(memobj);
}

bool lookup_gl_object(cl_mem mem, cvk_gl_object* out) {
    std::lock_guard<std::mutex> lock(gObjectsLock);
    auto it = gObjects.find(mem);
    if (it == gObjects.end()) {
        return false;
    }
    *out = it->second;
    return true;
}

void remember_gl_object(cl_mem mem, const cvk_gl_object& obj) {
    {
        std::lock_guard<std::mutex> lock(gObjectsLock);
        gObjects[mem] = obj;
    }
    clSetMemObjectDestructorCallback(mem, forget_gl_object, nullptr);
}

CvkGLenum binding_for_target(CvkGLenum target) {
    switch (target) {
    case GL_TEXTURE_1D:
        return GL_TEXTURE_BINDING_1D;
    case GL_TEXTURE_2D:
        return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_3D:
        return GL_TEXTURE_BINDING_3D;
    case GL_TEXTURE_RECTANGLE:
        return GL_TEXTURE_BINDING_RECTANGLE;
    case GL_TEXTURE_CUBE_MAP:
        return GL_TEXTURE_BINDING_CUBE_MAP;
    default:
        return 0;
    }
}

bool is_cube_face(CvkGLenum target) {
    return target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X &&
           target <= GL_TEXTURE_CUBE_MAP_NEGATIVE_Z;
}

// A texture is bound for the duration of a query or a copy and the previous
// binding put back, because the caller's GL state is not ours to change.
class scoped_texture_binding {
public:
    scoped_texture_binding(CvkGLenum bind_target, CvkGLuint texture)
        : m_bind_target(bind_target), m_previous(0) {
        CvkGLint previous = 0;
        gGL.GetIntegerv(binding_for_target(bind_target), &previous);
        m_previous = static_cast<CvkGLuint>(previous);
        gGL.BindTexture(bind_target, texture);
    }
    ~scoped_texture_binding() { gGL.BindTexture(m_bind_target, m_previous); }

private:
    CvkGLenum m_bind_target;
    CvkGLuint m_previous;
};

class scoped_pixel_store {
public:
    scoped_pixel_store() {
        gGL.GetIntegerv(GL_PACK_ALIGNMENT, &m_pack);
        gGL.GetIntegerv(GL_UNPACK_ALIGNMENT, &m_unpack);
        gGL.PixelStorei(GL_PACK_ALIGNMENT, 1);
        gGL.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
    }
    ~scoped_pixel_store() {
        gGL.PixelStorei(GL_PACK_ALIGNMENT, m_pack);
        gGL.PixelStorei(GL_UNPACK_ALIGNMENT, m_unpack);
    }

private:
    CvkGLint m_pack, m_unpack;
};

size_t staging_size(const cvk_gl_object& obj) {
    if (obj.type == CL_GL_OBJECT_BUFFER) {
        return obj.size;
    }
    return obj.width * obj.height * obj.depth * obj.format->bytes_per_pixel;
}

cl_int copy_gl_to_cl(cl_command_queue queue, cl_mem mem,
                     const cvk_gl_object& obj) {
    std::vector<char> staging(staging_size(obj));
    if (staging.empty()) {
        return CL_SUCCESS;
    }

    scoped_pixel_store pixel_store;

    if (obj.type == CL_GL_OBJECT_BUFFER) {
        CvkGLint previous = 0;
        gGL.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
        gGL.BindBuffer(GL_ARRAY_BUFFER, obj.name);
        gGL.GetBufferSubData(GL_ARRAY_BUFFER, 0,
                             static_cast<CvkGLsizeiptr>(staging.size()),
                             staging.data());
        gGL.BindBuffer(GL_ARRAY_BUFFER, static_cast<CvkGLuint>(previous));
        return clEnqueueWriteBuffer(queue, mem, CL_TRUE, 0, staging.size(),
                                    staging.data(), 0, nullptr, nullptr);
    }

    if (obj.type == CL_GL_OBJECT_RENDERBUFFER) {
        CvkGLint previous_fbo = 0;
        gGL.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_fbo);
        CvkGLuint fbo = 0;
        gGL.GenFramebuffers(1, &fbo);
        gGL.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        gGL.FramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                    GL_RENDERBUFFER, obj.name);
        gGL.ReadPixels(0, 0, static_cast<CvkGLsizei>(obj.width),
                       static_cast<CvkGLsizei>(obj.height),
                       obj.format->transfer_format, obj.format->transfer_type,
                       staging.data());
        gGL.BindFramebuffer(GL_READ_FRAMEBUFFER,
                            static_cast<CvkGLuint>(previous_fbo));
        gGL.DeleteFramebuffers(1, &fbo);
    } else {
        scoped_texture_binding binding(obj.bind_target, obj.name);
        gGL.GetTexImage(obj.target, obj.miplevel, obj.format->transfer_format,
                        obj.format->transfer_type, staging.data());
    }

    size_t origin[3] = {0, 0, 0};
    size_t region[3] = {obj.width, obj.height, obj.depth};
    return clEnqueueWriteImage(queue, mem, CL_TRUE, origin, region, 0, 0,
                               staging.data(), 0, nullptr, nullptr);
}

cl_int copy_cl_to_gl(cl_command_queue queue, cl_mem mem,
                     const cvk_gl_object& obj) {
    std::vector<char> staging(staging_size(obj));
    if (staging.empty()) {
        return CL_SUCCESS;
    }

    scoped_pixel_store pixel_store;

    if (obj.type == CL_GL_OBJECT_BUFFER) {
        cl_int err = clEnqueueReadBuffer(queue, mem, CL_TRUE, 0, staging.size(),
                                         staging.data(), 0, nullptr, nullptr);
        if (err != CL_SUCCESS) {
            return err;
        }
        CvkGLint previous = 0;
        gGL.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
        gGL.BindBuffer(GL_ARRAY_BUFFER, obj.name);
        gGL.BufferSubData(GL_ARRAY_BUFFER, 0,
                          static_cast<CvkGLsizeiptr>(staging.size()),
                          staging.data());
        gGL.BindBuffer(GL_ARRAY_BUFFER, static_cast<CvkGLuint>(previous));
        return CL_SUCCESS;
    }

    size_t origin[3] = {0, 0, 0};
    size_t region[3] = {obj.width, obj.height, obj.depth};
    cl_int err = clEnqueueReadImage(queue, mem, CL_TRUE, origin, region, 0, 0,
                                    staging.data(), 0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        return err;
    }

    if (obj.type == CL_GL_OBJECT_RENDERBUFFER) {
        // A renderbuffer has no path back that does not go through a draw,
        // and the extension allows a copy in one direction to be all an
        // implementation offers for one.
        cvk_warn_fn("writing back to a GL renderbuffer is not supported");
        return CL_SUCCESS;
    }

    scoped_texture_binding binding(obj.bind_target, obj.name);
    if (obj.type == CL_GL_OBJECT_TEXTURE3D) {
        if (gGL.TexSubImage3D == nullptr) {
            return CL_OUT_OF_RESOURCES;
        }
        gGL.TexSubImage3D(obj.target, obj.miplevel, 0, 0, 0,
                          static_cast<CvkGLsizei>(obj.width),
                          static_cast<CvkGLsizei>(obj.height),
                          static_cast<CvkGLsizei>(obj.depth),
                          obj.format->transfer_format,
                          obj.format->transfer_type, staging.data());
    } else {
        gGL.TexSubImage2D(
            obj.target, obj.miplevel, 0, 0, static_cast<CvkGLsizei>(obj.width),
            static_cast<CvkGLsizei>(obj.height), obj.format->transfer_format,
            obj.format->transfer_type, staging.data());
    }
    return CL_SUCCESS;
}

cl_int transfer_objects(cl_command_queue command_queue, cl_uint num_objects,
                        const cl_mem* mem_objects,
                        cl_uint num_events_in_wait_list,
                        const cl_event* event_wait_list, cl_event* event,
                        bool acquire) {
    if (command_queue == nullptr) {
        return CL_INVALID_COMMAND_QUEUE;
    }
    if ((num_objects > 0 && mem_objects == nullptr) ||
        (num_objects == 0 && mem_objects != nullptr)) {
        return CL_INVALID_VALUE;
    }
    if ((num_events_in_wait_list > 0 && event_wait_list == nullptr) ||
        (num_events_in_wait_list == 0 && event_wait_list != nullptr)) {
        return CL_INVALID_EVENT_WAIT_LIST;
    }
    if (!gl_context_current()) {
        return CL_INVALID_GL_OBJECT;
    }

    if (num_events_in_wait_list > 0) {
        cl_int err = clWaitForEvents(num_events_in_wait_list, event_wait_list);
        if (err != CL_SUCCESS) {
            return err;
        }
    }

    if (acquire) {
        // Whatever GL was told to do to these objects has to have happened
        // before their pixels are read out of it.
        gGL.Finish();
    } else {
        // And whatever OpenCL was told to do has to have happened before
        // they go back.
        cl_int err = clFinish(command_queue);
        if (err != CL_SUCCESS) {
            return err;
        }
    }

    for (cl_uint i = 0; i < num_objects; i++) {
        cvk_gl_object obj;
        if (!lookup_gl_object(mem_objects[i], &obj)) {
            return CL_INVALID_GL_OBJECT;
        }
        cl_int err = acquire
                         ? copy_gl_to_cl(command_queue, mem_objects[i], obj)
                         : copy_cl_to_gl(command_queue, mem_objects[i], obj);
        if (err != CL_SUCCESS) {
            cvk_error_fn("transfer of GL object %u failed (%d)", obj.name, err);
            return err;
        }
    }

    if (event != nullptr) {
        return clEnqueueMarkerWithWaitList(command_queue, 0, nullptr, event);
    }
    return CL_SUCCESS;
}

cl_mem create_from_gl_texture(cl_context context, cl_mem_flags flags,
                              CvkGLenum target, CvkGLint miplevel,
                              CvkGLuint texture, cl_int* errcode_ret) {
    auto fail = [errcode_ret](cl_int err) -> cl_mem {
        if (errcode_ret != nullptr) {
            *errcode_ret = err;
        }
        return nullptr;
    };

    if (context == nullptr) {
        return fail(CL_INVALID_CONTEXT);
    }
    if (!gl_context_current()) {
        return fail(CL_INVALID_GL_OBJECT);
    }
    if (miplevel < 0) {
        return fail(CL_INVALID_MIP_LEVEL);
    }

    CvkGLenum bind_target = is_cube_face(target) ? GL_TEXTURE_CUBE_MAP : target;
    if (binding_for_target(bind_target) == 0) {
        return fail(CL_INVALID_VALUE);
    }

    CvkGLint width = 0, height = 0, depth = 1, internal_format = 0;
    {
        scoped_texture_binding binding(bind_target, texture);
        gGL.GetTexLevelParameteriv(target, miplevel, GL_TEXTURE_WIDTH, &width);
        gGL.GetTexLevelParameteriv(target, miplevel, GL_TEXTURE_HEIGHT,
                                   &height);
        if (target == GL_TEXTURE_3D) {
            gGL.GetTexLevelParameteriv(target, miplevel, GL_TEXTURE_DEPTH,
                                       &depth);
        }
        gGL.GetTexLevelParameteriv(target, miplevel, GL_TEXTURE_INTERNAL_FORMAT,
                                   &internal_format);
    }

    if (width <= 0 || height <= 0 || depth <= 0) {
        cvk_error_fn("texture %u level %d has no storage", texture, miplevel);
        return fail(CL_INVALID_GL_OBJECT);
    }

    auto format = find_format(static_cast<CvkGLenum>(internal_format));
    if (format == nullptr) {
        cvk_error_fn("GL internal format 0x%x has no OpenCL equivalent",
                     internal_format);
        return fail(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
    }

    cl_image_format image_format;
    image_format.image_channel_order = format->order;
    image_format.image_channel_data_type = format->data_type;

    cl_image_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.image_type =
        target == GL_TEXTURE_3D ? CL_MEM_OBJECT_IMAGE3D : CL_MEM_OBJECT_IMAGE2D;
    desc.image_width = static_cast<size_t>(width);
    desc.image_height = static_cast<size_t>(height);
    desc.image_depth = static_cast<size_t>(depth);

    // The image is clvk's own; only the contents are shared.
    cl_mem_flags mem_flags =
        flags & (CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY);
    if (mem_flags == 0) {
        mem_flags = CL_MEM_READ_WRITE;
    }

    cl_int err;
    cl_mem mem =
        clCreateImage(context, mem_flags, &image_format, &desc, nullptr, &err);
    if (mem == nullptr) {
        return fail(err);
    }

    cvk_gl_object obj;
    obj.type = target == GL_TEXTURE_3D ? CL_GL_OBJECT_TEXTURE3D
                                       : CL_GL_OBJECT_TEXTURE2D;
    obj.name = texture;
    obj.target = target;
    obj.bind_target = bind_target;
    obj.miplevel = miplevel;
    obj.width = static_cast<size_t>(width);
    obj.height = static_cast<size_t>(height);
    obj.depth = target == GL_TEXTURE_3D ? static_cast<size_t>(depth) : 1;
    obj.format = format;
    obj.size = 0;
    remember_gl_object(mem, obj);

    if (errcode_ret != nullptr) {
        *errcode_ret = CL_SUCCESS;
    }
    return mem;
}

} // namespace

bool cvk_gl_sharing_available() {
    if (!config.gl_sharing) {
        return false;
    }
    return gl().core_loaded;
}

// ---------------------------------------------------------------------------
// The entry points
// ---------------------------------------------------------------------------

cl_mem CL_API_CALL clCreateFromGLBuffer(cl_context context, cl_mem_flags flags,
                                        cl_GLuint bufobj, cl_int* errcode_ret) {
    auto fail = [errcode_ret](cl_int err) -> cl_mem {
        if (errcode_ret != nullptr) {
            *errcode_ret = err;
        }
        return nullptr;
    };

    if (context == nullptr) {
        return fail(CL_INVALID_CONTEXT);
    }
    if (!gl_context_current() || gGL.GetBufferParameteriv == nullptr ||
        gGL.GetBufferSubData == nullptr) {
        return fail(CL_INVALID_GL_OBJECT);
    }

    CvkGLint size = 0;
    CvkGLint previous = 0;
    gGL.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &previous);
    gGL.BindBuffer(GL_ARRAY_BUFFER, bufobj);
    gGL.GetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
    gGL.BindBuffer(GL_ARRAY_BUFFER, static_cast<CvkGLuint>(previous));

    if (size <= 0) {
        return fail(CL_INVALID_GL_OBJECT);
    }

    cl_mem_flags mem_flags =
        flags & (CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY);
    if (mem_flags == 0) {
        mem_flags = CL_MEM_READ_WRITE;
    }

    cl_int err;
    cl_mem mem = clCreateBuffer(context, mem_flags, static_cast<size_t>(size),
                                nullptr, &err);
    if (mem == nullptr) {
        return fail(err);
    }

    cvk_gl_object obj;
    memset(&obj, 0, sizeof(obj));
    obj.type = CL_GL_OBJECT_BUFFER;
    obj.name = bufobj;
    obj.size = static_cast<size_t>(size);
    obj.width = obj.size;
    obj.height = 1;
    obj.depth = 1;
    remember_gl_object(mem, obj);

    if (errcode_ret != nullptr) {
        *errcode_ret = CL_SUCCESS;
    }
    return mem;
}

cl_mem CL_API_CALL clCreateFromGLTexture(cl_context context, cl_mem_flags flags,
                                         cl_GLenum target, cl_GLint miplevel,
                                         cl_GLuint texture,
                                         cl_int* errcode_ret) {
    return create_from_gl_texture(context, flags, target, miplevel, texture,
                                  errcode_ret);
}

cl_mem CL_API_CALL clCreateFromGLTexture2D(cl_context context,
                                           cl_mem_flags flags, cl_GLenum target,
                                           cl_GLint miplevel, cl_GLuint texture,
                                           cl_int* errcode_ret) {
    if (target == GL_TEXTURE_3D) {
        if (errcode_ret != nullptr) {
            *errcode_ret = CL_INVALID_VALUE;
        }
        return nullptr;
    }
    return create_from_gl_texture(context, flags, target, miplevel, texture,
                                  errcode_ret);
}

cl_mem CL_API_CALL clCreateFromGLTexture3D(cl_context context,
                                           cl_mem_flags flags, cl_GLenum target,
                                           cl_GLint miplevel, cl_GLuint texture,
                                           cl_int* errcode_ret) {
    if (target != GL_TEXTURE_3D) {
        if (errcode_ret != nullptr) {
            *errcode_ret = CL_INVALID_VALUE;
        }
        return nullptr;
    }
    return create_from_gl_texture(context, flags, target, miplevel, texture,
                                  errcode_ret);
}

cl_mem CL_API_CALL clCreateFromGLRenderbuffer(cl_context context,
                                              cl_mem_flags flags,
                                              cl_GLuint renderbuffer,
                                              cl_int* errcode_ret) {
    auto fail = [errcode_ret](cl_int err) -> cl_mem {
        if (errcode_ret != nullptr) {
            *errcode_ret = err;
        }
        return nullptr;
    };

    if (context == nullptr) {
        return fail(CL_INVALID_CONTEXT);
    }
    if (!gl_context_current() || gGL.BindRenderbuffer == nullptr ||
        gGL.GetRenderbufferParameteriv == nullptr ||
        gGL.GenFramebuffers == nullptr) {
        return fail(CL_INVALID_GL_OBJECT);
    }

    CvkGLint previous = 0, width = 0, height = 0, internal_format = 0;
    gGL.GetIntegerv(GL_RENDERBUFFER_BINDING, &previous);
    gGL.BindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    gGL.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH,
                                   &width);
    gGL.GetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT,
                                   &height);
    gGL.GetRenderbufferParameteriv(
        GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &internal_format);
    gGL.BindRenderbuffer(GL_RENDERBUFFER, static_cast<CvkGLuint>(previous));

    if (width <= 0 || height <= 0) {
        return fail(CL_INVALID_GL_OBJECT);
    }

    auto format = find_format(static_cast<CvkGLenum>(internal_format));
    if (format == nullptr) {
        return fail(CL_INVALID_IMAGE_FORMAT_DESCRIPTOR);
    }

    cl_image_format image_format;
    image_format.image_channel_order = format->order;
    image_format.image_channel_data_type = format->data_type;

    cl_image_desc desc;
    memset(&desc, 0, sizeof(desc));
    desc.image_type = CL_MEM_OBJECT_IMAGE2D;
    desc.image_width = static_cast<size_t>(width);
    desc.image_height = static_cast<size_t>(height);

    cl_mem_flags mem_flags =
        flags & (CL_MEM_READ_WRITE | CL_MEM_WRITE_ONLY | CL_MEM_READ_ONLY);
    if (mem_flags == 0) {
        mem_flags = CL_MEM_READ_WRITE;
    }

    cl_int err;
    cl_mem mem =
        clCreateImage(context, mem_flags, &image_format, &desc, nullptr, &err);
    if (mem == nullptr) {
        return fail(err);
    }

    cvk_gl_object obj;
    memset(&obj, 0, sizeof(obj));
    obj.type = CL_GL_OBJECT_RENDERBUFFER;
    obj.name = renderbuffer;
    obj.width = static_cast<size_t>(width);
    obj.height = static_cast<size_t>(height);
    obj.depth = 1;
    obj.format = format;
    remember_gl_object(mem, obj);

    if (errcode_ret != nullptr) {
        *errcode_ret = CL_SUCCESS;
    }
    return mem;
}

cl_int CL_API_CALL clGetGLObjectInfo(cl_mem memobj,
                                     cl_gl_object_type* gl_object_type,
                                     cl_GLuint* gl_object_name) {
    cvk_gl_object obj;
    if (memobj == nullptr || !lookup_gl_object(memobj, &obj)) {
        return CL_INVALID_GL_OBJECT;
    }
    if (gl_object_type != nullptr) {
        *gl_object_type = obj.type;
    }
    if (gl_object_name != nullptr) {
        *gl_object_name = obj.name;
    }
    return CL_SUCCESS;
}

cl_int CL_API_CALL clGetGLTextureInfo(cl_mem memobj,
                                      cl_gl_texture_info param_name,
                                      size_t param_value_size,
                                      void* param_value,
                                      size_t* param_value_size_ret) {
    cvk_gl_object obj;
    if (memobj == nullptr || !lookup_gl_object(memobj, &obj)) {
        return CL_INVALID_GL_OBJECT;
    }
    if (obj.type != CL_GL_OBJECT_TEXTURE2D &&
        obj.type != CL_GL_OBJECT_TEXTURE3D) {
        return CL_INVALID_MEM_OBJECT;
    }

    cl_GLenum val_target;
    cl_GLint val_int;
    const void* copy_ptr = nullptr;
    size_t size_ret = 0;

    switch (param_name) {
    case CL_GL_TEXTURE_TARGET:
        val_target = obj.target;
        copy_ptr = &val_target;
        size_ret = sizeof(val_target);
        break;
    case CL_GL_MIPMAP_LEVEL:
        val_int = obj.miplevel;
        copy_ptr = &val_int;
        size_ret = sizeof(val_int);
        break;
    case CL_GL_NUM_SAMPLES:
        val_int = 0;
        copy_ptr = &val_int;
        size_ret = sizeof(val_int);
        break;
    default:
        return CL_INVALID_VALUE;
    }

    if (param_value != nullptr) {
        if (param_value_size < size_ret) {
            return CL_INVALID_VALUE;
        }
        memcpy(param_value, copy_ptr, size_ret);
    }
    if (param_value_size_ret != nullptr) {
        *param_value_size_ret = size_ret;
    }
    return CL_SUCCESS;
}

cl_int CL_API_CALL clEnqueueAcquireGLObjects(cl_command_queue command_queue,
                                             cl_uint num_objects,
                                             const cl_mem* mem_objects,
                                             cl_uint num_events_in_wait_list,
                                             const cl_event* event_wait_list,
                                             cl_event* event) {
    return transfer_objects(command_queue, num_objects, mem_objects,
                            num_events_in_wait_list, event_wait_list, event,
                            true);
}

cl_int CL_API_CALL clEnqueueReleaseGLObjects(cl_command_queue command_queue,
                                             cl_uint num_objects,
                                             const cl_mem* mem_objects,
                                             cl_uint num_events_in_wait_list,
                                             const cl_event* event_wait_list,
                                             cl_event* event) {
    return transfer_objects(command_queue, num_objects, mem_objects,
                            num_events_in_wait_list, event_wait_list, event,
                            false);
}

cl_int CL_API_CALL clGetGLContextInfoKHR(
    const cl_context_properties* /*properties*/, cl_gl_context_info param_name,
    size_t param_value_size, void* param_value, size_t* param_value_size_ret) {
    // Every device this platform has can work with any GL context, because
    // the sharing goes through the host either way.
    cl_platform_id platform;
    cl_int err = clGetPlatformIDs(1, &platform, nullptr);
    if (err != CL_SUCCESS) {
        return err;
    }

    cl_uint num_devices = 0;
    err =
        clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &num_devices);
    if (err != CL_SUCCESS || num_devices == 0) {
        if (param_value_size_ret != nullptr) {
            *param_value_size_ret = 0;
        }
        return CL_SUCCESS;
    }

    std::vector<cl_device_id> devices(num_devices);
    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, num_devices,
                         devices.data(), nullptr);
    if (err != CL_SUCCESS) {
        return err;
    }

    size_t size_ret;
    switch (param_name) {
    case CL_CURRENT_DEVICE_FOR_GL_CONTEXT_KHR:
        size_ret = sizeof(cl_device_id);
        break;
    case CL_DEVICES_FOR_GL_CONTEXT_KHR:
        size_ret = devices.size() * sizeof(cl_device_id);
        break;
    default:
        return CL_INVALID_VALUE;
    }

    if (param_value != nullptr) {
        if (param_value_size < size_ret) {
            return CL_INVALID_VALUE;
        }
        memcpy(param_value, devices.data(), size_ret);
    }
    if (param_value_size_ret != nullptr) {
        *param_value_size_ret = size_ret;
    }
    return CL_SUCCESS;
}
