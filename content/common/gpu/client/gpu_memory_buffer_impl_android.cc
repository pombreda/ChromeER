// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/common/gpu/client/gpu_memory_buffer_impl.h"

#include "content/common/gpu/client/gpu_memory_buffer_impl_shared_memory.h"
#include "content/common/gpu/client/gpu_memory_buffer_impl_surface_texture.h"

namespace content {

// static
scoped_ptr<GpuMemoryBufferImpl> GpuMemoryBufferImpl::Create(
    const gfx::Size& size,
    unsigned internalformat,
    unsigned usage) {
  if (GpuMemoryBufferImplSharedMemory::IsConfigurationSupported(
          size, internalformat, usage)) {
    scoped_ptr<GpuMemoryBufferImplSharedMemory> buffer(
        new GpuMemoryBufferImplSharedMemory(size, internalformat));
    if (!buffer->Initialize())
      return scoped_ptr<GpuMemoryBufferImpl>();

    return buffer.PassAs<GpuMemoryBufferImpl>();
  }

  return scoped_ptr<GpuMemoryBufferImpl>();
}

// static
void GpuMemoryBufferImpl::AllocateForChildProcess(
    const gfx::Size& size,
    unsigned internalformat,
    unsigned usage,
    base::ProcessHandle child_process,
    int child_id,
    const AllocationCallback& callback) {
  if (GpuMemoryBufferImplSharedMemory::IsConfigurationSupported(
          size, internalformat, usage)) {
    GpuMemoryBufferImplSharedMemory::AllocateSharedMemoryForChildProcess(
        size, internalformat, child_process, callback);
    return;
  }

  callback.Run(gfx::GpuMemoryBufferHandle());
}

// static
void GpuMemoryBufferImpl::DeletedByChildProcess(
    gfx::GpuMemoryBufferType type,
    const gfx::GpuMemoryBufferId& id,
    base::ProcessHandle child_process) {
}

// static
scoped_ptr<GpuMemoryBufferImpl> GpuMemoryBufferImpl::CreateFromHandle(
    const gfx::GpuMemoryBufferHandle& handle,
    const gfx::Size& size,
    unsigned internalformat) {
  switch (handle.type) {
    case gfx::SHARED_MEMORY_BUFFER: {
      scoped_ptr<GpuMemoryBufferImplSharedMemory> buffer(
          new GpuMemoryBufferImplSharedMemory(size, internalformat));
      if (!buffer->InitializeFromHandle(handle))
        return scoped_ptr<GpuMemoryBufferImpl>();

      return buffer.PassAs<GpuMemoryBufferImpl>();
    }
    case gfx::SURFACE_TEXTURE_BUFFER: {
      scoped_ptr<GpuMemoryBufferImplSurfaceTexture> buffer(
          new GpuMemoryBufferImplSurfaceTexture(size, internalformat));
      if (!buffer->InitializeFromHandle(handle))
        return scoped_ptr<GpuMemoryBufferImpl>();

      return buffer.PassAs<GpuMemoryBufferImpl>();
    }
    default:
      return scoped_ptr<GpuMemoryBufferImpl>();
  }
}

}  // namespace content
