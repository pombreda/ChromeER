// Copyright 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cc/output/gl_renderer.h"

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "base/debug/trace_event.h"
#include "base/logging.h"
#include "cc/base/math_util.h"
#include "cc/layers/video_layer_impl.h"
#include "cc/output/compositor_frame.h"
#include "cc/output/compositor_frame_metadata.h"
#include "cc/output/context_provider.h"
#include "cc/output/copy_output_request.h"
#include "cc/output/geometry_binding.h"
#include "cc/output/gl_frame_data.h"
#include "cc/output/output_surface.h"
#include "cc/output/render_surface_filters.h"
#include "cc/quads/picture_draw_quad.h"
#include "cc/quads/render_pass.h"
#include "cc/quads/stream_video_draw_quad.h"
#include "cc/quads/texture_draw_quad.h"
#include "cc/resources/layer_quad.h"
#include "cc/resources/scoped_gpu_raster.h"
#include "cc/resources/scoped_resource.h"
#include "cc/resources/texture_mailbox_deleter.h"
#include "gpu/GLES2/gl2extchromium.h"
#include "gpu/command_buffer/client/context_support.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "gpu/command_buffer/common/gpu_memory_allocation.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkColorFilter.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkSurface.h"
#include "third_party/skia/include/gpu/GrContext.h"
#include "third_party/skia/include/gpu/GrTexture.h"
#include "third_party/skia/include/gpu/SkGrTexturePixelRef.h"
#include "third_party/skia/include/gpu/gl/GrGLInterface.h"
#include "ui/gfx/geometry/quad_f.h"
#include "ui/gfx/geometry/rect_conversions.h"

using gpu::gles2::GLES2Interface;

namespace cc {
namespace {

bool NeedsIOSurfaceReadbackWorkaround() {
#if defined(OS_MACOSX)
  // This isn't strictly required in DumpRenderTree-mode when Mesa is used,
  // but it doesn't seem to hurt.
  return true;
#else
  return false;
#endif
}

Float4 UVTransform(const TextureDrawQuad* quad) {
  gfx::PointF uv0 = quad->uv_top_left;
  gfx::PointF uv1 = quad->uv_bottom_right;
  Float4 xform = {{uv0.x(), uv0.y(), uv1.x() - uv0.x(), uv1.y() - uv0.y()}};
  if (quad->flipped) {
    xform.data[1] = 1.0f - xform.data[1];
    xform.data[3] = -xform.data[3];
  }
  return xform;
}

Float4 PremultipliedColor(SkColor color) {
  const float factor = 1.0f / 255.0f;
  const float alpha = SkColorGetA(color) * factor;

  Float4 result = {
      {SkColorGetR(color) * factor * alpha, SkColorGetG(color) * factor * alpha,
       SkColorGetB(color) * factor * alpha, alpha}};
  return result;
}

SamplerType SamplerTypeFromTextureTarget(GLenum target) {
  switch (target) {
    case GL_TEXTURE_2D:
      return SamplerType2D;
    case GL_TEXTURE_RECTANGLE_ARB:
      return SamplerType2DRect;
    case GL_TEXTURE_EXTERNAL_OES:
      return SamplerTypeExternalOES;
    default:
      NOTREACHED();
      return SamplerType2D;
  }
}

BlendMode BlendModeFromSkXfermode(SkXfermode::Mode mode) {
  switch (mode) {
    case SkXfermode::kSrcOver_Mode:
      return BlendModeNormal;
    case SkXfermode::kScreen_Mode:
      return BlendModeScreen;
    case SkXfermode::kOverlay_Mode:
      return BlendModeOverlay;
    case SkXfermode::kDarken_Mode:
      return BlendModeDarken;
    case SkXfermode::kLighten_Mode:
      return BlendModeLighten;
    case SkXfermode::kColorDodge_Mode:
      return BlendModeColorDodge;
    case SkXfermode::kColorBurn_Mode:
      return BlendModeColorBurn;
    case SkXfermode::kHardLight_Mode:
      return BlendModeHardLight;
    case SkXfermode::kSoftLight_Mode:
      return BlendModeSoftLight;
    case SkXfermode::kDifference_Mode:
      return BlendModeDifference;
    case SkXfermode::kExclusion_Mode:
      return BlendModeExclusion;
    case SkXfermode::kMultiply_Mode:
      return BlendModeMultiply;
    case SkXfermode::kHue_Mode:
      return BlendModeHue;
    case SkXfermode::kSaturation_Mode:
      return BlendModeSaturation;
    case SkXfermode::kColor_Mode:
      return BlendModeColor;
    case SkXfermode::kLuminosity_Mode:
      return BlendModeLuminosity;
    default:
      NOTREACHED();
      return BlendModeNone;
  }
}

// Smallest unit that impact anti-aliasing output. We use this to
// determine when anti-aliasing is unnecessary.
const float kAntiAliasingEpsilon = 1.0f / 1024.0f;

// Block or crash if the number of pending sync queries reach this high as
// something is seriously wrong on the service side if this happens.
const size_t kMaxPendingSyncQueries = 16;

}  // anonymous namespace

static GLint GetActiveTextureUnit(GLES2Interface* gl) {
  GLint active_unit = 0;
  gl->GetIntegerv(GL_ACTIVE_TEXTURE, &active_unit);
  return active_unit;
}

class GLRenderer::ScopedUseGrContext {
 public:
  static scoped_ptr<ScopedUseGrContext> Create(GLRenderer* renderer,
                                               DrawingFrame* frame) {
    return make_scoped_ptr(new ScopedUseGrContext(renderer, frame));
  }

  ~ScopedUseGrContext() {
    // Pass context control back to GLrenderer.
    scoped_gpu_raster_ = nullptr;
    renderer_->RestoreGLState();
    renderer_->RestoreFramebuffer(frame_);
  }

  GrContext* context() const {
    return renderer_->output_surface_->context_provider()->GrContext();
  }

 private:
  ScopedUseGrContext(GLRenderer* renderer, DrawingFrame* frame)
      : scoped_gpu_raster_(
            new ScopedGpuRaster(renderer->output_surface_->context_provider())),
        renderer_(renderer),
        frame_(frame) {
    // scoped_gpu_raster_ passes context control to Skia.
  }

  scoped_ptr<ScopedGpuRaster> scoped_gpu_raster_;
  GLRenderer* renderer_;
  DrawingFrame* frame_;

  DISALLOW_COPY_AND_ASSIGN(ScopedUseGrContext);
};

struct GLRenderer::PendingAsyncReadPixels {
  PendingAsyncReadPixels() : buffer(0) {}

  scoped_ptr<CopyOutputRequest> copy_request;
  base::CancelableClosure finished_read_pixels_callback;
  unsigned buffer;

 private:
  DISALLOW_COPY_AND_ASSIGN(PendingAsyncReadPixels);
};

class GLRenderer::SyncQuery {
 public:
  explicit SyncQuery(gpu::gles2::GLES2Interface* gl)
      : gl_(gl), query_id_(0u), is_pending_(false), weak_ptr_factory_(this) {
    gl_->GenQueriesEXT(1, &query_id_);
  }
  virtual ~SyncQuery() { gl_->DeleteQueriesEXT(1, &query_id_); }

  scoped_refptr<ResourceProvider::Fence> Begin() {
    DCHECK(!IsPending());
    // Invalidate weak pointer held by old fence.
    weak_ptr_factory_.InvalidateWeakPtrs();
    // Note: In case the set of drawing commands issued before End() do not
    // depend on the query, defer BeginQueryEXT call until Set() is called and
    // query is required.
    return make_scoped_refptr<ResourceProvider::Fence>(
        new Fence(weak_ptr_factory_.GetWeakPtr()));
  }

  void Set() {
    if (is_pending_)
      return;

    // Note: BeginQueryEXT on GL_COMMANDS_COMPLETED_CHROMIUM is effectively a
    // noop relative to GL, so it doesn't matter where it happens but we still
    // make sure to issue this command when Set() is called (prior to issuing
    // any drawing commands that depend on query), in case some future extension
    // can take advantage of this.
    gl_->BeginQueryEXT(GL_COMMANDS_COMPLETED_CHROMIUM, query_id_);
    is_pending_ = true;
  }

  void End() {
    if (!is_pending_)
      return;

    gl_->EndQueryEXT(GL_COMMANDS_COMPLETED_CHROMIUM);
  }

  bool IsPending() {
    if (!is_pending_)
      return false;

    unsigned result_available = 1;
    gl_->GetQueryObjectuivEXT(
        query_id_, GL_QUERY_RESULT_AVAILABLE_EXT, &result_available);
    is_pending_ = !result_available;
    return is_pending_;
  }

  void Wait() {
    if (!is_pending_)
      return;

    unsigned result = 0;
    gl_->GetQueryObjectuivEXT(query_id_, GL_QUERY_RESULT_EXT, &result);
    is_pending_ = false;
  }

 private:
  class Fence : public ResourceProvider::Fence {
   public:
    explicit Fence(base::WeakPtr<GLRenderer::SyncQuery> query)
        : query_(query) {}

    // Overridden from ResourceProvider::Fence:
    void Set() override {
      DCHECK(query_);
      query_->Set();
    }
    bool HasPassed() override { return !query_ || !query_->IsPending(); }
    void Wait() override {
      if (query_)
        query_->Wait();
    }

   private:
    ~Fence() override {}

    base::WeakPtr<SyncQuery> query_;

    DISALLOW_COPY_AND_ASSIGN(Fence);
  };

  gpu::gles2::GLES2Interface* gl_;
  unsigned query_id_;
  bool is_pending_;
  base::WeakPtrFactory<SyncQuery> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(SyncQuery);
};

scoped_ptr<GLRenderer> GLRenderer::Create(
    RendererClient* client,
    const RendererSettings* settings,
    OutputSurface* output_surface,
    ResourceProvider* resource_provider,
    TextureMailboxDeleter* texture_mailbox_deleter,
    int highp_threshold_min) {
  return make_scoped_ptr(new GLRenderer(client,
                                        settings,
                                        output_surface,
                                        resource_provider,
                                        texture_mailbox_deleter,
                                        highp_threshold_min));
}

GLRenderer::GLRenderer(RendererClient* client,
                       const RendererSettings* settings,
                       OutputSurface* output_surface,
                       ResourceProvider* resource_provider,
                       TextureMailboxDeleter* texture_mailbox_deleter,
                       int highp_threshold_min)
    : DirectRenderer(client, settings, output_surface, resource_provider),
      offscreen_framebuffer_id_(0),
      shared_geometry_quad_(QuadVertexRect()),
      gl_(output_surface->context_provider()->ContextGL()),
      context_support_(output_surface->context_provider()->ContextSupport()),
      texture_mailbox_deleter_(texture_mailbox_deleter),
      is_backbuffer_discarded_(false),
      is_scissor_enabled_(false),
      scissor_rect_needs_reset_(true),
      stencil_shadow_(false),
      blend_shadow_(false),
      highp_threshold_min_(highp_threshold_min),
      highp_threshold_cache_(0),
      use_sync_query_(false),
      on_demand_tile_raster_resource_id_(0) {
  DCHECK(gl_);
  DCHECK(context_support_);

  ContextProvider::Capabilities context_caps =
      output_surface_->context_provider()->ContextCapabilities();

  capabilities_.using_partial_swap =
      settings_->partial_swap_enabled && context_caps.gpu.post_sub_buffer;

  DCHECK(!context_caps.gpu.iosurface || context_caps.gpu.texture_rectangle);

  capabilities_.using_egl_image = context_caps.gpu.egl_image_external;

  capabilities_.max_texture_size = resource_provider_->max_texture_size();
  capabilities_.best_texture_format = resource_provider_->best_texture_format();

  // The updater can access textures while the GLRenderer is using them.
  capabilities_.allow_partial_texture_updates = true;

  capabilities_.using_image = context_caps.gpu.image;

  capabilities_.using_discard_framebuffer =
      context_caps.gpu.discard_framebuffer;

  capabilities_.allow_rasterize_on_demand = true;

  use_sync_query_ = context_caps.gpu.sync_query;
  use_blend_equation_advanced_ = context_caps.gpu.blend_equation_advanced;
  use_blend_equation_advanced_coherent_ =
      context_caps.gpu.blend_equation_advanced_coherent;

  InitializeSharedObjects();
}

GLRenderer::~GLRenderer() {
  while (!pending_async_read_pixels_.empty()) {
    PendingAsyncReadPixels* pending_read = pending_async_read_pixels_.back();
    pending_read->finished_read_pixels_callback.Cancel();
    pending_async_read_pixels_.pop_back();
  }

  in_use_overlay_resources_.clear();

  CleanupSharedObjects();
}

const RendererCapabilitiesImpl& GLRenderer::Capabilities() const {
  return capabilities_;
}

void GLRenderer::DebugGLCall(GLES2Interface* gl,
                             const char* command,
                             const char* file,
                             int line) {
  GLuint error = gl->GetError();
  if (error != GL_NO_ERROR)
    LOG(ERROR) << "GL command failed: File: " << file << "\n\tLine " << line
               << "\n\tcommand: " << command << ", error "
               << static_cast<int>(error) << "\n";
}

void GLRenderer::DidChangeVisibility() {
  EnforceMemoryPolicy();

  context_support_->SetSurfaceVisible(visible());
}

void GLRenderer::ReleaseRenderPassTextures() { render_pass_textures_.clear(); }

void GLRenderer::DiscardPixels(bool has_external_stencil_test,
                               bool draw_rect_covers_full_surface) {
  if (has_external_stencil_test || !draw_rect_covers_full_surface ||
      !capabilities_.using_discard_framebuffer)
    return;
  bool using_default_framebuffer =
      !current_framebuffer_lock_ &&
      output_surface_->capabilities().uses_default_gl_framebuffer;
  GLenum attachments[] = {static_cast<GLenum>(
      using_default_framebuffer ? GL_COLOR_EXT : GL_COLOR_ATTACHMENT0_EXT)};
  gl_->DiscardFramebufferEXT(
      GL_FRAMEBUFFER, arraysize(attachments), attachments);
}

void GLRenderer::ClearFramebuffer(DrawingFrame* frame,
                                  bool has_external_stencil_test) {
  // It's unsafe to clear when we have a stencil test because glClear ignores
  // stencil.
  if (has_external_stencil_test) {
    DCHECK(!frame->current_render_pass->has_transparent_background);
    return;
  }

  // On DEBUG builds, opaque render passes are cleared to blue to easily see
  // regions that were not drawn on the screen.
  if (frame->current_render_pass->has_transparent_background)
    GLC(gl_, gl_->ClearColor(0, 0, 0, 0));
  else
    GLC(gl_, gl_->ClearColor(0, 0, 1, 1));

  bool always_clear = false;
#ifndef NDEBUG
  always_clear = true;
#endif
  if (always_clear || frame->current_render_pass->has_transparent_background) {
    GLbitfield clear_bits = GL_COLOR_BUFFER_BIT;
    if (always_clear)
      clear_bits |= GL_STENCIL_BUFFER_BIT;
    gl_->Clear(clear_bits);
  }
}

static ResourceProvider::ResourceId WaitOnResourceSyncPoints(
    ResourceProvider* resource_provider,
    ResourceProvider::ResourceId resource_id) {
  resource_provider->WaitSyncPointIfNeeded(resource_id);
  return resource_id;
}

void GLRenderer::BeginDrawingFrame(DrawingFrame* frame) {
  TRACE_EVENT0("cc", "GLRenderer::BeginDrawingFrame");

  scoped_refptr<ResourceProvider::Fence> read_lock_fence;
  if (use_sync_query_) {
    // Block until oldest sync query has passed if the number of pending queries
    // ever reach kMaxPendingSyncQueries.
    if (pending_sync_queries_.size() >= kMaxPendingSyncQueries) {
      LOG(ERROR) << "Reached limit of pending sync queries.";

      pending_sync_queries_.front()->Wait();
      DCHECK(!pending_sync_queries_.front()->IsPending());
    }

    while (!pending_sync_queries_.empty()) {
      if (pending_sync_queries_.front()->IsPending())
        break;

      available_sync_queries_.push_back(pending_sync_queries_.take_front());
    }

    current_sync_query_ = available_sync_queries_.empty()
                              ? make_scoped_ptr(new SyncQuery(gl_))
                              : available_sync_queries_.take_front();

    read_lock_fence = current_sync_query_->Begin();
  } else {
    read_lock_fence =
        make_scoped_refptr(new ResourceProvider::SynchronousFence(gl_));
  }
  resource_provider_->SetReadLockFence(read_lock_fence.get());

  // Insert WaitSyncPointCHROMIUM on quad resources prior to drawing the frame,
  // so that drawing can proceed without GL context switching interruptions.
  DrawQuad::ResourceIteratorCallback wait_on_resource_syncpoints_callback =
      base::Bind(&WaitOnResourceSyncPoints, resource_provider_);

  for (const auto& pass : *frame->render_passes_in_draw_order) {
    for (const auto& quad : pass->quad_list)
      quad->IterateResources(wait_on_resource_syncpoints_callback);
  }

  // TODO(enne): Do we need to reinitialize all of this state per frame?
  ReinitializeGLState();
}

void GLRenderer::DoNoOp() {
  GLC(gl_, gl_->BindFramebuffer(GL_FRAMEBUFFER, 0));
  GLC(gl_, gl_->Flush());
}

void GLRenderer::DoDrawQuad(DrawingFrame* frame, const DrawQuad* quad) {
  DCHECK(quad->rect.Contains(quad->visible_rect));
  if (quad->material != DrawQuad::TEXTURE_CONTENT) {
    FlushTextureQuadCache();
  }

  switch (quad->material) {
    case DrawQuad::INVALID:
      NOTREACHED();
      break;
    case DrawQuad::CHECKERBOARD:
      DrawCheckerboardQuad(frame, CheckerboardDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::DEBUG_BORDER:
      DrawDebugBorderQuad(frame, DebugBorderDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::IO_SURFACE_CONTENT:
      DrawIOSurfaceQuad(frame, IOSurfaceDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::PICTURE_CONTENT:
      DrawPictureQuad(frame, PictureDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::RENDER_PASS:
      DrawRenderPassQuad(frame, RenderPassDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::SOLID_COLOR:
      DrawSolidColorQuad(frame, SolidColorDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::STREAM_VIDEO_CONTENT:
      DrawStreamVideoQuad(frame, StreamVideoDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::SURFACE_CONTENT:
      // Surface content should be fully resolved to other quad types before
      // reaching a direct renderer.
      NOTREACHED();
      break;
    case DrawQuad::TEXTURE_CONTENT:
      EnqueueTextureQuad(frame, TextureDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::TILED_CONTENT:
      DrawTileQuad(frame, TileDrawQuad::MaterialCast(quad));
      break;
    case DrawQuad::YUV_VIDEO_CONTENT:
      DrawYUVVideoQuad(frame, YUVVideoDrawQuad::MaterialCast(quad));
      break;
  }
}

void GLRenderer::DrawCheckerboardQuad(const DrawingFrame* frame,
                                      const CheckerboardDrawQuad* quad) {
  SetBlendEnabled(quad->ShouldDrawWithBlending());

  const TileCheckerboardProgram* program = GetTileCheckerboardProgram();
  DCHECK(program && (program->initialized() || IsContextLost()));
  SetUseProgram(program->program());

  SkColor color = quad->color;
  GLC(gl_,
      gl_->Uniform4f(program->fragment_shader().color_location(),
                     SkColorGetR(color) * (1.0f / 255.0f),
                     SkColorGetG(color) * (1.0f / 255.0f),
                     SkColorGetB(color) * (1.0f / 255.0f),
                     1));

  const int checkerboard_width = 16;
  float frequency = 1.0f / checkerboard_width;

  gfx::Rect tile_rect = quad->rect;
  float tex_offset_x = tile_rect.x() % checkerboard_width;
  float tex_offset_y = tile_rect.y() % checkerboard_width;
  float tex_scale_x = tile_rect.width();
  float tex_scale_y = tile_rect.height();
  GLC(gl_,
      gl_->Uniform4f(program->fragment_shader().tex_transform_location(),
                     tex_offset_x,
                     tex_offset_y,
                     tex_scale_x,
                     tex_scale_y));

  GLC(gl_,
      gl_->Uniform1f(program->fragment_shader().frequency_location(),
                     frequency));

  SetShaderOpacity(quad->opacity(),
                   program->fragment_shader().alpha_location());
  DrawQuadGeometry(frame,
                   quad->quadTransform(),
                   quad->rect,
                   program->vertex_shader().matrix_location());
}

void GLRenderer::DrawDebugBorderQuad(const DrawingFrame* frame,
                                     const DebugBorderDrawQuad* quad) {
  SetBlendEnabled(quad->ShouldDrawWithBlending());

  static float gl_matrix[16];
  const DebugBorderProgram* program = GetDebugBorderProgram();
  DCHECK(program && (program->initialized() || IsContextLost()));
  SetUseProgram(program->program());

  // Use the full quad_rect for debug quads to not move the edges based on
  // partial swaps.
  gfx::Rect layer_rect = quad->rect;
  gfx::Transform render_matrix;
  QuadRectTransform(&render_matrix, quad->quadTransform(), layer_rect);
  GLRenderer::ToGLMatrix(&gl_matrix[0],
                         frame->projection_matrix * render_matrix);
  GLC(gl_,
      gl_->UniformMatrix4fv(
          program->vertex_shader().matrix_location(), 1, false, &gl_matrix[0]));

  SkColor color = quad->color;
  float alpha = SkColorGetA(color) * (1.0f / 255.0f);

  GLC(gl_,
      gl_->Uniform4f(program->fragment_shader().color_location(),
                     (SkColorGetR(color) * (1.0f / 255.0f)) * alpha,
                     (SkColorGetG(color) * (1.0f / 255.0f)) * alpha,
                     (SkColorGetB(color) * (1.0f / 255.0f)) * alpha,
                     alpha));

  GLC(gl_, gl_->LineWidth(quad->width));

  // The indices for the line are stored in the same array as the triangle
  // indices.
  GLC(gl_, gl_->DrawElements(GL_LINE_LOOP, 4, GL_UNSIGNED_SHORT, 0));
}

static skia::RefPtr<SkImage> ApplyImageFilter(
    scoped_ptr<GLRenderer::ScopedUseGrContext> use_gr_context,
    ResourceProvider* resource_provider,
    const gfx::Point& origin,
    const gfx::Vector2dF& scale,
    SkImageFilter* filter,
    ScopedResource* source_texture_resource) {
  if (!filter)
    return skia::RefPtr<SkImage>();

  if (!use_gr_context)
    return skia::RefPtr<SkImage>();

  ResourceProvider::ScopedReadLockGL lock(resource_provider,
                                          source_texture_resource->id());

  // Wrap the source texture in a Ganesh platform texture.
  GrBackendTextureDesc backend_texture_description;
  backend_texture_description.fWidth = source_texture_resource->size().width();
  backend_texture_description.fHeight =
      source_texture_resource->size().height();
  backend_texture_description.fConfig = kSkia8888_GrPixelConfig;
  backend_texture_description.fTextureHandle = lock.texture_id();
  backend_texture_description.fOrigin = kBottomLeft_GrSurfaceOrigin;
  skia::RefPtr<GrTexture> texture =
      skia::AdoptRef(use_gr_context->context()->wrapBackendTexture(
          backend_texture_description));
  if (!texture) {
    TRACE_EVENT_INSTANT0("cc",
                         "ApplyImageFilter wrap background texture failed",
                         TRACE_EVENT_SCOPE_THREAD);
    return skia::RefPtr<SkImage>();
  }

  SkImageInfo info =
      SkImageInfo::MakeN32Premul(source_texture_resource->size().width(),
                                 source_texture_resource->size().height());
  // Place the platform texture inside an SkBitmap.
  SkBitmap source;
  source.setInfo(info);
  skia::RefPtr<SkGrPixelRef> pixel_ref =
      skia::AdoptRef(new SkGrPixelRef(info, texture.get()));
  source.setPixelRef(pixel_ref.get());

  // Create a scratch texture for backing store.
  GrTextureDesc desc;
  desc.fFlags = kRenderTarget_GrTextureFlagBit | kNoStencil_GrTextureFlagBit;
  desc.fSampleCnt = 0;
  desc.fWidth = source.width();
  desc.fHeight = source.height();
  desc.fConfig = kSkia8888_GrPixelConfig;
  desc.fOrigin = kBottomLeft_GrSurfaceOrigin;
  skia::RefPtr<GrTexture> backing_store =
      skia::AdoptRef(use_gr_context->context()->refScratchTexture(
          desc, GrContext::kExact_ScratchTexMatch));
  if (!backing_store) {
    TRACE_EVENT_INSTANT0("cc",
                         "ApplyImageFilter scratch texture allocation failed",
                         TRACE_EVENT_SCOPE_THREAD);
    return skia::RefPtr<SkImage>();
  }

  // Create surface to draw into.
  skia::RefPtr<SkSurface> surface = skia::AdoptRef(
      SkSurface::NewRenderTargetDirect(backing_store->asRenderTarget()));
  skia::RefPtr<SkCanvas> canvas = skia::SharePtr(surface->getCanvas());

  // Draw the source bitmap through the filter to the canvas.
  SkPaint paint;
  paint.setImageFilter(filter);
  canvas->clear(SK_ColorTRANSPARENT);

  canvas->translate(SkIntToScalar(-origin.x()), SkIntToScalar(-origin.y()));
  canvas->scale(scale.x(), scale.y());
  canvas->drawSprite(source, 0, 0, &paint);

  skia::RefPtr<SkImage> image = skia::AdoptRef(surface->newImageSnapshot());
  if (!image || !image->getTexture()) {
    return skia::RefPtr<SkImage>();
  }

  // Flush the GrContext to ensure all buffered GL calls are drawn to the
  // backing store before we access and return it, and have cc begin using the
  // GL context again.
  canvas->flush();

  return image;
}

bool GLRenderer::CanApplyBlendModeUsingBlendFunc(SkXfermode::Mode blend_mode) {
  return use_blend_equation_advanced_ ||
         blend_mode == SkXfermode::kScreen_Mode ||
         blend_mode == SkXfermode::kSrcOver_Mode;
}

void GLRenderer::ApplyBlendModeUsingBlendFunc(SkXfermode::Mode blend_mode) {
  DCHECK(CanApplyBlendModeUsingBlendFunc(blend_mode));

  // Any modes set here must be reset in RestoreBlendFuncToDefault
  if (use_blend_equation_advanced_) {
    GLenum equation = GL_FUNC_ADD;

    switch (blend_mode) {
      case SkXfermode::kScreen_Mode:
        equation = GL_SCREEN_KHR;
        break;
      case SkXfermode::kOverlay_Mode:
        equation = GL_OVERLAY_KHR;
        break;
      case SkXfermode::kDarken_Mode:
        equation = GL_DARKEN_KHR;
        break;
      case SkXfermode::kLighten_Mode:
        equation = GL_LIGHTEN_KHR;
        break;
      case SkXfermode::kColorDodge_Mode:
        equation = GL_COLORDODGE_KHR;
        break;
      case SkXfermode::kColorBurn_Mode:
        equation = GL_COLORBURN_KHR;
        break;
      case SkXfermode::kHardLight_Mode:
        equation = GL_HARDLIGHT_KHR;
        break;
      case SkXfermode::kSoftLight_Mode:
        equation = GL_SOFTLIGHT_KHR;
        break;
      case SkXfermode::kDifference_Mode:
        equation = GL_DIFFERENCE_KHR;
        break;
      case SkXfermode::kExclusion_Mode:
        equation = GL_EXCLUSION_KHR;
        break;
      case SkXfermode::kMultiply_Mode:
        equation = GL_MULTIPLY_KHR;
        break;
      case SkXfermode::kHue_Mode:
        equation = GL_HSL_HUE_KHR;
        break;
      case SkXfermode::kSaturation_Mode:
        equation = GL_HSL_SATURATION_KHR;
        break;
      case SkXfermode::kColor_Mode:
        equation = GL_HSL_COLOR_KHR;
        break;
      case SkXfermode::kLuminosity_Mode:
        equation = GL_HSL_LUMINOSITY_KHR;
        break;
      default:
        return;
    }

    GLC(gl_, gl_->BlendEquation(equation));
  } else {
    if (blend_mode == SkXfermode::kScreen_Mode) {
      GLC(gl_, gl_->BlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE));
    }
  }
}

void GLRenderer::RestoreBlendFuncToDefault(SkXfermode::Mode blend_mode) {
  if (blend_mode == SkXfermode::kSrcOver_Mode)
    return;

  if (use_blend_equation_advanced_) {
    GLC(gl_, gl_->BlendEquation(GL_FUNC_ADD));
  } else {
    GLC(gl_, gl_->BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
  }
}

bool GLRenderer::ShouldApplyBackgroundFilters(DrawingFrame* frame,
                                              const RenderPassDrawQuad* quad) {
  if (quad->background_filters.IsEmpty())
    return false;

  // TODO(danakj): We only allow background filters on an opaque render surface
  // because other surfaces may contain translucent pixels, and the contents
  // behind those translucent pixels wouldn't have the filter applied.
  if (frame->current_render_pass->has_transparent_background)
    return false;

  // TODO(ajuma): Add support for reference filters once
  // FilterOperations::GetOutsets supports reference filters.
  if (quad->background_filters.HasReferenceFilter())
    return false;
  return true;
}

gfx::Rect GLRenderer::GetBackdropBoundingBoxForRenderPassQuad(
    DrawingFrame* frame,
    const RenderPassDrawQuad* quad,
    const gfx::Transform& contents_device_transform,
    bool use_aa) {
  gfx::Rect backdrop_rect = gfx::ToEnclosingRect(MathUtil::MapClippedRect(
      contents_device_transform, SharedGeometryQuad().BoundingBox()));

  if (ShouldApplyBackgroundFilters(frame, quad)) {
    int top, right, bottom, left;
    quad->background_filters.GetOutsets(&top, &right, &bottom, &left);
    backdrop_rect.Inset(-left, -top, -right, -bottom);
  }

  if (!backdrop_rect.IsEmpty() && use_aa) {
    const int kOutsetForAntialiasing = 1;
    backdrop_rect.Inset(-kOutsetForAntialiasing, -kOutsetForAntialiasing);
  }

  backdrop_rect.Intersect(MoveFromDrawToWindowSpace(
      frame, frame->current_render_pass->output_rect));
  return backdrop_rect;
}

scoped_ptr<ScopedResource> GLRenderer::GetBackdropTexture(
    const gfx::Rect& bounding_rect) {
  scoped_ptr<ScopedResource> device_background_texture =
      ScopedResource::Create(resource_provider_);
  // CopyTexImage2D fails when called on a texture having immutable storage.
  device_background_texture->Allocate(
      bounding_rect.size(), ResourceProvider::TextureHintDefault, RGBA_8888);
  {
    ResourceProvider::ScopedWriteLockGL lock(resource_provider_,
                                             device_background_texture->id());
    GetFramebufferTexture(
        lock.texture_id(), device_background_texture->format(), bounding_rect);
  }
  return device_background_texture.Pass();
}

skia::RefPtr<SkImage> GLRenderer::ApplyBackgroundFilters(
    DrawingFrame* frame,
    const RenderPassDrawQuad* quad,
    ScopedResource* background_texture) {
  DCHECK(ShouldApplyBackgroundFilters(frame, quad));
  skia::RefPtr<SkImageFilter> filter = RenderSurfaceFilters::BuildImageFilter(
      quad->background_filters, background_texture->size());

  skia::RefPtr<SkImage> background_with_filters =
      ApplyImageFilter(ScopedUseGrContext::Create(this, frame),
                       resource_provider_,
                       quad->rect.origin(),
                       quad->filters_scale,
                       filter.get(),
                       background_texture);
  return background_with_filters;
}

void GLRenderer::DrawRenderPassQuad(DrawingFrame* frame,
                                    const RenderPassDrawQuad* quad) {
  ScopedResource* contents_texture =
      render_pass_textures_.get(quad->render_pass_id);
  if (!contents_texture || !contents_texture->id())
    return;

  gfx::Transform quad_rect_matrix;
  QuadRectTransform(&quad_rect_matrix, quad->quadTransform(), quad->rect);
  gfx::Transform contents_device_transform =
      frame->window_matrix * frame->projection_matrix * quad_rect_matrix;
  contents_device_transform.FlattenTo2d();

  // Can only draw surface if device matrix is invertible.
  if (!contents_device_transform.IsInvertible())
    return;

  gfx::QuadF surface_quad = SharedGeometryQuad();
  float edge[24];
  bool use_aa = settings_->allow_antialiasing &&
                ShouldAntialiasQuad(contents_device_transform, quad,
                                    settings_->force_antialiasing);

  if (use_aa)
    SetupQuadForAntialiasing(contents_device_transform, quad,
                             &surface_quad, edge);

  SkXfermode::Mode blend_mode = quad->shared_quad_state->blend_mode;
  bool use_shaders_for_blending =
      !CanApplyBlendModeUsingBlendFunc(blend_mode) ||
      ShouldApplyBackgroundFilters(frame, quad) ||
      settings_->force_blending_with_shaders;

  scoped_ptr<ScopedResource> background_texture;
  skia::RefPtr<SkImage> background_image;
  gfx::Rect background_rect;
  if (use_shaders_for_blending) {
    // Compute a bounding box around the pixels that will be visible through
    // the quad.
    background_rect = GetBackdropBoundingBoxForRenderPassQuad(
        frame, quad, contents_device_transform, use_aa);

    if (!background_rect.IsEmpty()) {
      // The pixels from the filtered background should completely replace the
      // current pixel values.
      if (blend_enabled())
        SetBlendEnabled(false);

      // Read the pixels in the bounding box into a buffer R.
      // This function allocates a texture, which should contribute to the
      // amount of memory used by render surfaces:
      // LayerTreeHost::CalculateMemoryForRenderSurfaces.
      background_texture = GetBackdropTexture(background_rect);

      if (ShouldApplyBackgroundFilters(frame, quad) && background_texture) {
        // Apply the background filters to R, so that it is applied in the
        // pixels' coordinate space.
        background_image =
            ApplyBackgroundFilters(frame, quad, background_texture.get());
      }
    }

    if (!background_texture) {
      // Something went wrong with reading the backdrop.
      DCHECK(!background_image);
      use_shaders_for_blending = false;
    } else if (background_image) {
      background_texture.reset();
    } else if (CanApplyBlendModeUsingBlendFunc(blend_mode) &&
               ShouldApplyBackgroundFilters(frame, quad)) {
      // Something went wrong with applying background filters to the backdrop.
      use_shaders_for_blending = false;
      background_texture.reset();
    }
  }

  SetBlendEnabled(
      !use_shaders_for_blending &&
      (quad->ShouldDrawWithBlending() || !IsDefaultBlendMode(blend_mode)));

  // TODO(senorblanco): Cache this value so that we don't have to do it for both
  // the surface and its replica.  Apply filters to the contents texture.
  skia::RefPtr<SkImage> filter_image;
  SkScalar color_matrix[20];
  bool use_color_matrix = false;
  if (!quad->filters.IsEmpty()) {
    skia::RefPtr<SkImageFilter> filter = RenderSurfaceFilters::BuildImageFilter(
        quad->filters, contents_texture->size());
    if (filter) {
      skia::RefPtr<SkColorFilter> cf;

      {
        SkColorFilter* colorfilter_rawptr = NULL;
        filter->asColorFilter(&colorfilter_rawptr);
        cf = skia::AdoptRef(colorfilter_rawptr);
      }

      if (cf && cf->asColorMatrix(color_matrix) && !filter->getInput(0)) {
        // We have a single color matrix as a filter; apply it locally
        // in the compositor.
        use_color_matrix = true;
      } else {
        filter_image = ApplyImageFilter(ScopedUseGrContext::Create(this, frame),
                                        resource_provider_,
                                        quad->rect.origin(),
                                        quad->filters_scale,
                                        filter.get(),
                                        contents_texture);
      }
    }
  }

  scoped_ptr<ResourceProvider::ScopedSamplerGL> mask_resource_lock;
  unsigned mask_texture_id = 0;
  SamplerType mask_sampler = SamplerTypeNA;
  if (quad->mask_resource_id) {
    mask_resource_lock.reset(new ResourceProvider::ScopedSamplerGL(
        resource_provider_, quad->mask_resource_id, GL_TEXTURE1, GL_LINEAR));
    mask_texture_id = mask_resource_lock->texture_id();
    mask_sampler = SamplerTypeFromTextureTarget(mask_resource_lock->target());
  }

  scoped_ptr<ResourceProvider::ScopedSamplerGL> contents_resource_lock;
  if (filter_image) {
    GrTexture* texture = filter_image->getTexture();
    DCHECK_EQ(GL_TEXTURE0, GetActiveTextureUnit(gl_));
    gl_->BindTexture(GL_TEXTURE_2D, texture->getTextureHandle());
  } else {
    contents_resource_lock =
        make_scoped_ptr(new ResourceProvider::ScopedSamplerGL(
            resource_provider_, contents_texture->id(), GL_LINEAR));
    DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D),
              contents_resource_lock->target());
  }

  if (!use_shaders_for_blending) {
    if (!use_blend_equation_advanced_coherent_ && use_blend_equation_advanced_)
      GLC(gl_, gl_->BlendBarrierKHR());

    ApplyBlendModeUsingBlendFunc(blend_mode);
  }

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_,
      &highp_threshold_cache_,
      highp_threshold_min_,
      quad->shared_quad_state->visible_content_rect.bottom_right());

  int shader_quad_location = -1;
  int shader_edge_location = -1;
  int shader_viewport_location = -1;
  int shader_mask_sampler_location = -1;
  int shader_mask_tex_coord_scale_location = -1;
  int shader_mask_tex_coord_offset_location = -1;
  int shader_matrix_location = -1;
  int shader_alpha_location = -1;
  int shader_color_matrix_location = -1;
  int shader_color_offset_location = -1;
  int shader_tex_transform_location = -1;
  int shader_backdrop_location = -1;
  int shader_backdrop_rect_location = -1;

  DCHECK_EQ(background_texture || background_image, use_shaders_for_blending);
  BlendMode shader_blend_mode = use_shaders_for_blending
                                    ? BlendModeFromSkXfermode(blend_mode)
                                    : BlendModeNone;

  if (use_aa && mask_texture_id && !use_color_matrix) {
    const RenderPassMaskProgramAA* program = GetRenderPassMaskProgramAA(
        tex_coord_precision, mask_sampler, shader_blend_mode);
    SetUseProgram(program->program());
    GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

    shader_quad_location = program->vertex_shader().quad_location();
    shader_edge_location = program->vertex_shader().edge_location();
    shader_viewport_location = program->vertex_shader().viewport_location();
    shader_mask_sampler_location =
        program->fragment_shader().mask_sampler_location();
    shader_mask_tex_coord_scale_location =
        program->fragment_shader().mask_tex_coord_scale_location();
    shader_mask_tex_coord_offset_location =
        program->fragment_shader().mask_tex_coord_offset_location();
    shader_matrix_location = program->vertex_shader().matrix_location();
    shader_alpha_location = program->fragment_shader().alpha_location();
    shader_tex_transform_location =
        program->vertex_shader().tex_transform_location();
    shader_backdrop_location = program->fragment_shader().backdrop_location();
    shader_backdrop_rect_location =
        program->fragment_shader().backdrop_rect_location();
  } else if (!use_aa && mask_texture_id && !use_color_matrix) {
    const RenderPassMaskProgram* program = GetRenderPassMaskProgram(
        tex_coord_precision, mask_sampler, shader_blend_mode);
    SetUseProgram(program->program());
    GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

    shader_mask_sampler_location =
        program->fragment_shader().mask_sampler_location();
    shader_mask_tex_coord_scale_location =
        program->fragment_shader().mask_tex_coord_scale_location();
    shader_mask_tex_coord_offset_location =
        program->fragment_shader().mask_tex_coord_offset_location();
    shader_matrix_location = program->vertex_shader().matrix_location();
    shader_alpha_location = program->fragment_shader().alpha_location();
    shader_tex_transform_location =
        program->vertex_shader().tex_transform_location();
    shader_backdrop_location = program->fragment_shader().backdrop_location();
    shader_backdrop_rect_location =
        program->fragment_shader().backdrop_rect_location();
  } else if (use_aa && !mask_texture_id && !use_color_matrix) {
    const RenderPassProgramAA* program =
        GetRenderPassProgramAA(tex_coord_precision, shader_blend_mode);
    SetUseProgram(program->program());
    GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

    shader_quad_location = program->vertex_shader().quad_location();
    shader_edge_location = program->vertex_shader().edge_location();
    shader_viewport_location = program->vertex_shader().viewport_location();
    shader_matrix_location = program->vertex_shader().matrix_location();
    shader_alpha_location = program->fragment_shader().alpha_location();
    shader_tex_transform_location =
        program->vertex_shader().tex_transform_location();
    shader_backdrop_location = program->fragment_shader().backdrop_location();
    shader_backdrop_rect_location =
        program->fragment_shader().backdrop_rect_location();
  } else if (use_aa && mask_texture_id && use_color_matrix) {
    const RenderPassMaskColorMatrixProgramAA* program =
        GetRenderPassMaskColorMatrixProgramAA(
            tex_coord_precision, mask_sampler, shader_blend_mode);
    SetUseProgram(program->program());
    GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

    shader_matrix_location = program->vertex_shader().matrix_location();
    shader_quad_location = program->vertex_shader().quad_location();
    shader_tex_transform_location =
        program->vertex_shader().tex_transform_location();
    shader_edge_location = program->vertex_shader().edge_location();
    shader_viewport_location = program->vertex_shader().viewport_location();
    shader_alpha_location = program->fragment_shader().alpha_location();
    shader_mask_sampler_location =
        program->fragment_shader().mask_sampler_location();
    shader_mask_tex_coord_scale_location =
        program->fragment_shader().mask_tex_coord_scale_location();
    shader_mask_tex_coord_offset_location =
        program->fragment_shader().mask_tex_coord_offset_location();
    shader_color_matrix_location =
        program->fragment_shader().color_matrix_location();
    shader_color_offset_location =
        program->fragment_shader().color_offset_location();
    shader_backdrop_location = program->fragment_shader().backdrop_location();
    shader_backdrop_rect_location =
        program->fragment_shader().backdrop_rect_location();
  } else if (use_aa && !mask_texture_id && use_color_matrix) {
    const RenderPassColorMatrixProgramAA* program =
        GetRenderPassColorMatrixProgramAA(tex_coord_precision,
                                          shader_blend_mode);
    SetUseProgram(program->program());
    GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

    shader_matrix_location = program->vertex_shader().matrix_location();
    shader_quad_location = program->vertex_shader().quad_location();
    shader_tex_transform_location =
        program->vertex_shader().tex_transform_location();
    shader_edge_location = program->vertex_shader().edge_location();
    shader_viewport_location = program->vertex_shader().viewport_location();
    shader_alpha_location = program->fragment_shader().alpha_location();
    shader_color_matrix_location =
        program->fragment_shader().color_matrix_location();
    shader_color_offset_location =
        program->fragment_shader().color_offset_location();
    shader_backdrop_location = program->fragment_shader().backdrop_location();
    shader_backdrop_rect_location =
        program->fragment_shader().backdrop_rect_location();
  } else if (!use_aa && mask_texture_id && use_color_matrix) {
    const RenderPassMaskColorMatrixProgram* program =
        GetRenderPassMaskColorMatrixProgram(
            tex_coord_precision, mask_sampler, shader_blend_mode);
    SetUseProgram(program->program());
    GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

    shader_matrix_location = program->vertex_shader().matrix_location();
    shader_tex_transform_location =
        program->vertex_shader().tex_transform_location();
    shader_mask_sampler_location =
        program->fragment_shader().mask_sampler_location();
    shader_mask_tex_coord_scale_location =
        program->fragment_shader().mask_tex_coord_scale_location();
    shader_mask_tex_coord_offset_location =
        program->fragment_shader().mask_tex_coord_offset_location();
    shader_alpha_location = program->fragment_shader().alpha_location();
    shader_color_matrix_location =
        program->fragment_shader().color_matrix_location();
    shader_color_offset_location =
        program->fragment_shader().color_offset_location();
    shader_backdrop_location = program->fragment_shader().backdrop_location();
    shader_backdrop_rect_location =
        program->fragment_shader().backdrop_rect_location();
  } else if (!use_aa && !mask_texture_id && use_color_matrix) {
    const RenderPassColorMatrixProgram* program =
        GetRenderPassColorMatrixProgram(tex_coord_precision, shader_blend_mode);
    SetUseProgram(program->program());
    GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

    shader_matrix_location = program->vertex_shader().matrix_location();
    shader_tex_transform_location =
        program->vertex_shader().tex_transform_location();
    shader_alpha_location = program->fragment_shader().alpha_location();
    shader_color_matrix_location =
        program->fragment_shader().color_matrix_location();
    shader_color_offset_location =
        program->fragment_shader().color_offset_location();
    shader_backdrop_location = program->fragment_shader().backdrop_location();
    shader_backdrop_rect_location =
        program->fragment_shader().backdrop_rect_location();
  } else {
    const RenderPassProgram* program =
        GetRenderPassProgram(tex_coord_precision, shader_blend_mode);
    SetUseProgram(program->program());
    GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

    shader_matrix_location = program->vertex_shader().matrix_location();
    shader_alpha_location = program->fragment_shader().alpha_location();
    shader_tex_transform_location =
        program->vertex_shader().tex_transform_location();
    shader_backdrop_location = program->fragment_shader().backdrop_location();
    shader_backdrop_rect_location =
        program->fragment_shader().backdrop_rect_location();
  }
  float tex_scale_x =
      quad->rect.width() / static_cast<float>(contents_texture->size().width());
  float tex_scale_y = quad->rect.height() /
                      static_cast<float>(contents_texture->size().height());
  DCHECK_LE(tex_scale_x, 1.0f);
  DCHECK_LE(tex_scale_y, 1.0f);

  DCHECK(shader_tex_transform_location != -1 || IsContextLost());
  // Flip the content vertically in the shader, as the RenderPass input
  // texture is already oriented the same way as the framebuffer, but the
  // projection transform does a flip.
  GLC(gl_,
      gl_->Uniform4f(shader_tex_transform_location,
                     0.0f,
                     tex_scale_y,
                     tex_scale_x,
                     -tex_scale_y));

  GLint last_texture_unit = 0;
  if (shader_mask_sampler_location != -1) {
    DCHECK_NE(shader_mask_tex_coord_scale_location, 1);
    DCHECK_NE(shader_mask_tex_coord_offset_location, 1);
    GLC(gl_, gl_->Uniform1i(shader_mask_sampler_location, 1));

    gfx::RectF mask_uv_rect = quad->MaskUVRect();
    if (mask_sampler != SamplerType2D) {
      mask_uv_rect.Scale(quad->mask_texture_size.width(),
                         quad->mask_texture_size.height());
    }

    // Mask textures are oriented vertically flipped relative to the framebuffer
    // and the RenderPass contents texture, so we flip the tex coords from the
    // RenderPass texture to find the mask texture coords.
    GLC(gl_,
        gl_->Uniform2f(shader_mask_tex_coord_offset_location,
                       mask_uv_rect.x(),
                       mask_uv_rect.bottom()));
    GLC(gl_,
        gl_->Uniform2f(shader_mask_tex_coord_scale_location,
                       mask_uv_rect.width() / tex_scale_x,
                       -mask_uv_rect.height() / tex_scale_y));

    last_texture_unit = 1;
  }

  if (shader_edge_location != -1)
    GLC(gl_, gl_->Uniform3fv(shader_edge_location, 8, edge));

  if (shader_viewport_location != -1) {
    float viewport[4] = {static_cast<float>(viewport_.x()),
                         static_cast<float>(viewport_.y()),
                         static_cast<float>(viewport_.width()),
                         static_cast<float>(viewport_.height()), };
    GLC(gl_, gl_->Uniform4fv(shader_viewport_location, 1, viewport));
  }

  if (shader_color_matrix_location != -1) {
    float matrix[16];
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j)
        matrix[i * 4 + j] = SkScalarToFloat(color_matrix[j * 5 + i]);
    }
    GLC(gl_,
        gl_->UniformMatrix4fv(shader_color_matrix_location, 1, false, matrix));
  }
  static const float kScale = 1.0f / 255.0f;
  if (shader_color_offset_location != -1) {
    float offset[4];
    for (int i = 0; i < 4; ++i)
      offset[i] = SkScalarToFloat(color_matrix[i * 5 + 4]) * kScale;

    GLC(gl_, gl_->Uniform4fv(shader_color_offset_location, 1, offset));
  }

  scoped_ptr<ResourceProvider::ScopedSamplerGL> shader_background_sampler_lock;
  if (shader_backdrop_location != -1) {
    DCHECK(background_texture || background_image);
    DCHECK_NE(shader_backdrop_location, 0);
    DCHECK_NE(shader_backdrop_rect_location, 0);

    GLC(gl_, gl_->Uniform1i(shader_backdrop_location, ++last_texture_unit));

    GLC(gl_,
        gl_->Uniform4f(shader_backdrop_rect_location,
                       background_rect.x(),
                       background_rect.y(),
                       background_rect.width(),
                       background_rect.height()));

    if (background_image) {
      GrTexture* texture = background_image->getTexture();
      GLC(gl_, gl_->ActiveTexture(GL_TEXTURE0 + last_texture_unit));
      gl_->BindTexture(GL_TEXTURE_2D, texture->getTextureHandle());
      GLC(gl_, gl_->ActiveTexture(GL_TEXTURE0));
    } else {
      shader_background_sampler_lock = make_scoped_ptr(
          new ResourceProvider::ScopedSamplerGL(resource_provider_,
                                                background_texture->id(),
                                                GL_TEXTURE0 + last_texture_unit,
                                                GL_LINEAR));
      DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D),
                shader_background_sampler_lock->target());
    }
  }

  SetShaderOpacity(quad->opacity(), shader_alpha_location);
  SetShaderQuadF(surface_quad, shader_quad_location);
  DrawQuadGeometry(
      frame, quad->quadTransform(), quad->rect, shader_matrix_location);

  // Flush the compositor context before the filter bitmap goes out of
  // scope, so the draw gets processed before the filter texture gets deleted.
  if (filter_image)
    GLC(gl_, gl_->Flush());

  if (!use_shaders_for_blending)
    RestoreBlendFuncToDefault(blend_mode);
}

struct SolidColorProgramUniforms {
  unsigned program;
  unsigned matrix_location;
  unsigned viewport_location;
  unsigned quad_location;
  unsigned edge_location;
  unsigned color_location;
};

template <class T>
static void SolidColorUniformLocation(T program,
                                      SolidColorProgramUniforms* uniforms) {
  uniforms->program = program->program();
  uniforms->matrix_location = program->vertex_shader().matrix_location();
  uniforms->viewport_location = program->vertex_shader().viewport_location();
  uniforms->quad_location = program->vertex_shader().quad_location();
  uniforms->edge_location = program->vertex_shader().edge_location();
  uniforms->color_location = program->fragment_shader().color_location();
}

static gfx::QuadF GetDeviceQuadWithAntialiasingOnExteriorEdges(
    const LayerQuad& device_layer_edges,
    const gfx::Transform& device_transform,
    const DrawQuad* quad) {
  gfx::Rect tile_rect = quad->visible_rect;
  gfx::PointF bottom_right = tile_rect.bottom_right();
  gfx::PointF bottom_left = tile_rect.bottom_left();
  gfx::PointF top_left = tile_rect.origin();
  gfx::PointF top_right = tile_rect.top_right();
  bool clipped = false;

  // Map points to device space. We ignore |clipped|, since the result of
  // |MapPoint()| still produces a valid point to draw the quad with. When
  // clipped, the point will be outside of the viewport. See crbug.com/416367.
  bottom_right = MathUtil::MapPoint(device_transform, bottom_right, &clipped);
  bottom_left = MathUtil::MapPoint(device_transform, bottom_left, &clipped);
  top_left = MathUtil::MapPoint(device_transform, top_left, &clipped);
  top_right = MathUtil::MapPoint(device_transform, top_right, &clipped);

  LayerQuad::Edge bottom_edge(bottom_right, bottom_left);
  LayerQuad::Edge left_edge(bottom_left, top_left);
  LayerQuad::Edge top_edge(top_left, top_right);
  LayerQuad::Edge right_edge(top_right, bottom_right);

  // Only apply anti-aliasing to edges not clipped by culling or scissoring.
  if (quad->IsTopEdge() && tile_rect.y() == quad->rect.y())
    top_edge = device_layer_edges.top();
  if (quad->IsLeftEdge() && tile_rect.x() == quad->rect.x())
    left_edge = device_layer_edges.left();
  if (quad->IsRightEdge() && tile_rect.right() == quad->rect.right())
    right_edge = device_layer_edges.right();
  if (quad->IsBottomEdge() && tile_rect.bottom() == quad->rect.bottom())
    bottom_edge = device_layer_edges.bottom();

  float sign = gfx::QuadF(tile_rect).IsCounterClockwise() ? -1 : 1;
  bottom_edge.scale(sign);
  left_edge.scale(sign);
  top_edge.scale(sign);
  right_edge.scale(sign);

  // Create device space quad.
  return LayerQuad(left_edge, top_edge, right_edge, bottom_edge).ToQuadF();
}

// static
bool GLRenderer::ShouldAntialiasQuad(const gfx::Transform& device_transform,
                                     const DrawQuad* quad,
                                     bool force_antialiasing) {
  bool is_render_pass_quad = (quad->material == DrawQuad::RENDER_PASS);
  // For render pass quads, |device_transform| already contains quad's rect.
  // TODO(rosca@adobe.com): remove branching on is_render_pass_quad
  // crbug.com/429702
  if (!is_render_pass_quad && !quad->IsEdge())
    return false;
  gfx::RectF content_rect =
      is_render_pass_quad ? QuadVertexRect() : quad->visibleContentRect();

  bool clipped = false;
  gfx::QuadF device_layer_quad =
      MathUtil::MapQuad(device_transform, gfx::QuadF(content_rect), &clipped);

  if (device_layer_quad.BoundingBox().IsEmpty())
    return false;

  bool is_axis_aligned_in_target = device_layer_quad.IsRectilinear();
  bool is_nearest_rect_within_epsilon =
      is_axis_aligned_in_target &&
      gfx::IsNearestRectWithinDistance(device_layer_quad.BoundingBox(),
                                       kAntiAliasingEpsilon);
  // AAing clipped quads is not supported by the code yet.
  bool use_aa = !clipped && !is_nearest_rect_within_epsilon;
  return use_aa || force_antialiasing;
}

// static
void GLRenderer::SetupQuadForAntialiasing(
    const gfx::Transform& device_transform,
    const DrawQuad* quad,
    gfx::QuadF* local_quad,
    float edge[24]) {
  bool is_render_pass_quad = (quad->material == DrawQuad::RENDER_PASS);
  gfx::RectF content_rect =
      is_render_pass_quad ? QuadVertexRect() : quad->visibleContentRect();

  bool clipped = false;
  gfx::QuadF device_layer_quad =
      MathUtil::MapQuad(device_transform, gfx::QuadF(content_rect), &clipped);

  LayerQuad device_layer_bounds(gfx::QuadF(device_layer_quad.BoundingBox()));
  device_layer_bounds.InflateAntiAliasingDistance();

  LayerQuad device_layer_edges(device_layer_quad);
  device_layer_edges.InflateAntiAliasingDistance();

  device_layer_edges.ToFloatArray(edge);
  device_layer_bounds.ToFloatArray(&edge[12]);

  bool use_aa_on_all_four_edges =
      is_render_pass_quad ||
      (quad->IsTopEdge() && quad->IsLeftEdge() && quad->IsBottomEdge() &&
       quad->IsRightEdge() && quad->visible_rect == quad->rect);

  gfx::QuadF device_quad =
      use_aa_on_all_four_edges
          ? device_layer_edges.ToQuadF()
          : GetDeviceQuadWithAntialiasingOnExteriorEdges(
                device_layer_edges, device_transform, quad);

  // Map device space quad to local space. device_transform has no 3d
  // component since it was flattened, so we don't need to project.  We should
  // have already checked that the transform was uninvertible above.
  gfx::Transform inverse_device_transform(gfx::Transform::kSkipInitialization);
  bool did_invert = device_transform.GetInverse(&inverse_device_transform);
  DCHECK(did_invert);
  *local_quad =
      MathUtil::MapQuad(inverse_device_transform, device_quad, &clipped);
  // We should not DCHECK(!clipped) here, because anti-aliasing inflation may
  // cause device_quad to become clipped. To our knowledge this scenario does
  // not need to be handled differently than the unclipped case.
}

void GLRenderer::DrawSolidColorQuad(const DrawingFrame* frame,
                                    const SolidColorDrawQuad* quad) {
  gfx::Rect tile_rect = quad->visible_rect;

  SkColor color = quad->color;
  float opacity = quad->opacity();
  float alpha = (SkColorGetA(color) * (1.0f / 255.0f)) * opacity;

  // Early out if alpha is small enough that quad doesn't contribute to output.
  if (alpha < std::numeric_limits<float>::epsilon() &&
      quad->ShouldDrawWithBlending())
    return;

  gfx::Transform device_transform =
      frame->window_matrix * frame->projection_matrix * quad->quadTransform();
  device_transform.FlattenTo2d();
  if (!device_transform.IsInvertible())
    return;

  bool force_aa = false;
  gfx::QuadF local_quad = gfx::QuadF(gfx::RectF(tile_rect));
  float edge[24];
  bool use_aa = settings_->allow_antialiasing &&
                !quad->force_anti_aliasing_off &&
                ShouldAntialiasQuad(device_transform, quad, force_aa);

  SolidColorProgramUniforms uniforms;
  if (use_aa) {
    SetupQuadForAntialiasing(device_transform, quad, &local_quad, edge);
    SolidColorUniformLocation(GetSolidColorProgramAA(), &uniforms);
  } else {
    SolidColorUniformLocation(GetSolidColorProgram(), &uniforms);
  }
  SetUseProgram(uniforms.program);

  GLC(gl_,
      gl_->Uniform4f(uniforms.color_location,
                     (SkColorGetR(color) * (1.0f / 255.0f)) * alpha,
                     (SkColorGetG(color) * (1.0f / 255.0f)) * alpha,
                     (SkColorGetB(color) * (1.0f / 255.0f)) * alpha,
                     alpha));
  if (use_aa) {
    float viewport[4] = {static_cast<float>(viewport_.x()),
                         static_cast<float>(viewport_.y()),
                         static_cast<float>(viewport_.width()),
                         static_cast<float>(viewport_.height()), };
    GLC(gl_, gl_->Uniform4fv(uniforms.viewport_location, 1, viewport));
    GLC(gl_, gl_->Uniform3fv(uniforms.edge_location, 8, edge));
  }

  // Enable blending when the quad properties require it or if we decided
  // to use antialiasing.
  SetBlendEnabled(quad->ShouldDrawWithBlending() || use_aa);

  // Normalize to tile_rect.
  local_quad.Scale(1.0f / tile_rect.width(), 1.0f / tile_rect.height());

  SetShaderQuadF(local_quad, uniforms.quad_location);

  // The transform and vertex data are used to figure out the extents that the
  // un-antialiased quad should have and which vertex this is and the float
  // quad passed in via uniform is the actual geometry that gets used to draw
  // it. This is why this centered rect is used and not the original quad_rect.
  gfx::RectF centered_rect(
      gfx::PointF(-0.5f * tile_rect.width(), -0.5f * tile_rect.height()),
      tile_rect.size());
  DrawQuadGeometry(
      frame, quad->quadTransform(), centered_rect, uniforms.matrix_location);
}

struct TileProgramUniforms {
  unsigned program;
  unsigned matrix_location;
  unsigned viewport_location;
  unsigned quad_location;
  unsigned edge_location;
  unsigned vertex_tex_transform_location;
  unsigned sampler_location;
  unsigned fragment_tex_transform_location;
  unsigned alpha_location;
};

template <class T>
static void TileUniformLocation(T program, TileProgramUniforms* uniforms) {
  uniforms->program = program->program();
  uniforms->matrix_location = program->vertex_shader().matrix_location();
  uniforms->viewport_location = program->vertex_shader().viewport_location();
  uniforms->quad_location = program->vertex_shader().quad_location();
  uniforms->edge_location = program->vertex_shader().edge_location();
  uniforms->vertex_tex_transform_location =
      program->vertex_shader().vertex_tex_transform_location();

  uniforms->sampler_location = program->fragment_shader().sampler_location();
  uniforms->alpha_location = program->fragment_shader().alpha_location();
  uniforms->fragment_tex_transform_location =
      program->fragment_shader().fragment_tex_transform_location();
}

void GLRenderer::DrawTileQuad(const DrawingFrame* frame,
                              const TileDrawQuad* quad) {
  DrawContentQuad(frame, quad, quad->resource_id);
}

void GLRenderer::DrawContentQuad(const DrawingFrame* frame,
                                 const ContentDrawQuadBase* quad,
                                 ResourceProvider::ResourceId resource_id) {
  gfx::Transform device_transform =
      frame->window_matrix * frame->projection_matrix * quad->quadTransform();
  device_transform.FlattenTo2d();

  bool use_aa = settings_->allow_antialiasing &&
                ShouldAntialiasQuad(device_transform, quad, false);

  // TODO(timav): simplify coordinate transformations in DrawContentQuadAA
  // similar to the way DrawContentQuadNoAA works and then consider
  // combining DrawContentQuadAA and DrawContentQuadNoAA into one method.
  if (use_aa)
    DrawContentQuadAA(frame, quad, resource_id, device_transform);
  else
    DrawContentQuadNoAA(frame, quad, resource_id);
}

void GLRenderer::DrawContentQuadAA(const DrawingFrame* frame,
                                   const ContentDrawQuadBase* quad,
                                   ResourceProvider::ResourceId resource_id,
                                   const gfx::Transform& device_transform) {
  if (!device_transform.IsInvertible())
    return;

  gfx::Rect tile_rect = quad->visible_rect;

  gfx::RectF tex_coord_rect = MathUtil::ScaleRectProportional(
      quad->tex_coord_rect, quad->rect, tile_rect);
  float tex_to_geom_scale_x = quad->rect.width() / quad->tex_coord_rect.width();
  float tex_to_geom_scale_y =
      quad->rect.height() / quad->tex_coord_rect.height();

  gfx::RectF clamp_geom_rect(tile_rect);
  gfx::RectF clamp_tex_rect(tex_coord_rect);
  // Clamp texture coordinates to avoid sampling outside the layer
  // by deflating the tile region half a texel or half a texel
  // minus epsilon for one pixel layers. The resulting clamp region
  // is mapped to the unit square by the vertex shader and mapped
  // back to normalized texture coordinates by the fragment shader
  // after being clamped to 0-1 range.
  float tex_clamp_x =
      std::min(0.5f, 0.5f * clamp_tex_rect.width() - kAntiAliasingEpsilon);
  float tex_clamp_y =
      std::min(0.5f, 0.5f * clamp_tex_rect.height() - kAntiAliasingEpsilon);
  float geom_clamp_x =
      std::min(tex_clamp_x * tex_to_geom_scale_x,
               0.5f * clamp_geom_rect.width() - kAntiAliasingEpsilon);
  float geom_clamp_y =
      std::min(tex_clamp_y * tex_to_geom_scale_y,
               0.5f * clamp_geom_rect.height() - kAntiAliasingEpsilon);
  clamp_geom_rect.Inset(geom_clamp_x, geom_clamp_y, geom_clamp_x, geom_clamp_y);
  clamp_tex_rect.Inset(tex_clamp_x, tex_clamp_y, tex_clamp_x, tex_clamp_y);

  // Map clamping rectangle to unit square.
  float vertex_tex_translate_x = -clamp_geom_rect.x() / clamp_geom_rect.width();
  float vertex_tex_translate_y =
      -clamp_geom_rect.y() / clamp_geom_rect.height();
  float vertex_tex_scale_x = tile_rect.width() / clamp_geom_rect.width();
  float vertex_tex_scale_y = tile_rect.height() / clamp_geom_rect.height();

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_, &highp_threshold_cache_, highp_threshold_min_, quad->texture_size);

  gfx::QuadF local_quad = gfx::QuadF(gfx::RectF(tile_rect));
  float edge[24];
  SetupQuadForAntialiasing(device_transform, quad, &local_quad, edge);

  ResourceProvider::ScopedSamplerGL quad_resource_lock(
      resource_provider_, resource_id,
      quad->nearest_neighbor ? GL_NEAREST : GL_LINEAR);
  SamplerType sampler =
      SamplerTypeFromTextureTarget(quad_resource_lock.target());

  float fragment_tex_translate_x = clamp_tex_rect.x();
  float fragment_tex_translate_y = clamp_tex_rect.y();
  float fragment_tex_scale_x = clamp_tex_rect.width();
  float fragment_tex_scale_y = clamp_tex_rect.height();

  // Map to normalized texture coordinates.
  if (sampler != SamplerType2DRect) {
    gfx::Size texture_size = quad->texture_size;
    DCHECK(!texture_size.IsEmpty());
    fragment_tex_translate_x /= texture_size.width();
    fragment_tex_translate_y /= texture_size.height();
    fragment_tex_scale_x /= texture_size.width();
    fragment_tex_scale_y /= texture_size.height();
  }

  TileProgramUniforms uniforms;
  if (quad->swizzle_contents) {
    TileUniformLocation(GetTileProgramSwizzleAA(tex_coord_precision, sampler),
                        &uniforms);
  } else {
    TileUniformLocation(GetTileProgramAA(tex_coord_precision, sampler),
                        &uniforms);
  }

  SetUseProgram(uniforms.program);
  GLC(gl_, gl_->Uniform1i(uniforms.sampler_location, 0));

  float viewport[4] = {
      static_cast<float>(viewport_.x()),
      static_cast<float>(viewport_.y()),
      static_cast<float>(viewport_.width()),
      static_cast<float>(viewport_.height()),
  };
  GLC(gl_, gl_->Uniform4fv(uniforms.viewport_location, 1, viewport));
  GLC(gl_, gl_->Uniform3fv(uniforms.edge_location, 8, edge));

  GLC(gl_,
      gl_->Uniform4f(uniforms.vertex_tex_transform_location,
                     vertex_tex_translate_x,
                     vertex_tex_translate_y,
                     vertex_tex_scale_x,
                     vertex_tex_scale_y));
  GLC(gl_,
      gl_->Uniform4f(uniforms.fragment_tex_transform_location,
                     fragment_tex_translate_x,
                     fragment_tex_translate_y,
                     fragment_tex_scale_x,
                     fragment_tex_scale_y));

  // Blending is required for antialiasing.
  SetBlendEnabled(true);

  // Normalize to tile_rect.
  local_quad.Scale(1.0f / tile_rect.width(), 1.0f / tile_rect.height());

  SetShaderOpacity(quad->opacity(), uniforms.alpha_location);
  SetShaderQuadF(local_quad, uniforms.quad_location);

  // The transform and vertex data are used to figure out the extents that the
  // un-antialiased quad should have and which vertex this is and the float
  // quad passed in via uniform is the actual geometry that gets used to draw
  // it. This is why this centered rect is used and not the original quad_rect.
  gfx::RectF centered_rect(
      gfx::PointF(-0.5f * tile_rect.width(), -0.5f * tile_rect.height()),
      tile_rect.size());
  DrawQuadGeometry(
      frame, quad->quadTransform(), centered_rect, uniforms.matrix_location);
}

void GLRenderer::DrawContentQuadNoAA(const DrawingFrame* frame,
                                     const ContentDrawQuadBase* quad,
                                     ResourceProvider::ResourceId resource_id) {
  gfx::RectF tex_coord_rect = MathUtil::ScaleRectProportional(
      quad->tex_coord_rect, quad->rect, quad->visible_rect);
  float tex_to_geom_scale_x = quad->rect.width() / quad->tex_coord_rect.width();
  float tex_to_geom_scale_y =
      quad->rect.height() / quad->tex_coord_rect.height();

  bool scaled = (tex_to_geom_scale_x != 1.f || tex_to_geom_scale_y != 1.f);
  GLenum filter =
      (scaled || !quad->quadTransform().IsIdentityOrIntegerTranslation()) &&
              !quad->nearest_neighbor
          ? GL_LINEAR
          : GL_NEAREST;

  ResourceProvider::ScopedSamplerGL quad_resource_lock(
      resource_provider_, resource_id, filter);
  SamplerType sampler =
      SamplerTypeFromTextureTarget(quad_resource_lock.target());

  float vertex_tex_translate_x = tex_coord_rect.x();
  float vertex_tex_translate_y = tex_coord_rect.y();
  float vertex_tex_scale_x = tex_coord_rect.width();
  float vertex_tex_scale_y = tex_coord_rect.height();

  // Map to normalized texture coordinates.
  if (sampler != SamplerType2DRect) {
    gfx::Size texture_size = quad->texture_size;
    DCHECK(!texture_size.IsEmpty());
    vertex_tex_translate_x /= texture_size.width();
    vertex_tex_translate_y /= texture_size.height();
    vertex_tex_scale_x /= texture_size.width();
    vertex_tex_scale_y /= texture_size.height();
  }

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_, &highp_threshold_cache_, highp_threshold_min_, quad->texture_size);

  TileProgramUniforms uniforms;
  if (quad->ShouldDrawWithBlending()) {
    if (quad->swizzle_contents) {
      TileUniformLocation(GetTileProgramSwizzle(tex_coord_precision, sampler),
                          &uniforms);
    } else {
      TileUniformLocation(GetTileProgram(tex_coord_precision, sampler),
                          &uniforms);
    }
  } else {
    if (quad->swizzle_contents) {
      TileUniformLocation(
          GetTileProgramSwizzleOpaque(tex_coord_precision, sampler), &uniforms);
    } else {
      TileUniformLocation(GetTileProgramOpaque(tex_coord_precision, sampler),
                          &uniforms);
    }
  }

  SetUseProgram(uniforms.program);
  GLC(gl_, gl_->Uniform1i(uniforms.sampler_location, 0));

  GLC(gl_,
      gl_->Uniform4f(uniforms.vertex_tex_transform_location,
                     vertex_tex_translate_x,
                     vertex_tex_translate_y,
                     vertex_tex_scale_x,
                     vertex_tex_scale_y));

  SetBlendEnabled(quad->ShouldDrawWithBlending());

  SetShaderOpacity(quad->opacity(), uniforms.alpha_location);

  // Pass quad coordinates to the uniform in the same order as GeometryBinding
  // does, then vertices will match the texture mapping in the vertex buffer.
  // The method SetShaderQuadF() changes the order of vertices and so it's
  // not used here.

  gfx::RectF tile_rect = quad->visible_rect;
  float gl_quad[8] = {
    tile_rect.x(),
    tile_rect.bottom(),
    tile_rect.x(),
    tile_rect.y(),
    tile_rect.right(),
    tile_rect.y(),
    tile_rect.right(),
    tile_rect.bottom(),
  };
  GLC(gl_, gl_->Uniform2fv(uniforms.quad_location, 4, gl_quad));

  static float gl_matrix[16];
  ToGLMatrix(&gl_matrix[0], frame->projection_matrix * quad->quadTransform());
  GLC(gl_,
      gl_->UniformMatrix4fv(uniforms.matrix_location, 1, false, &gl_matrix[0]));

  GLC(gl_, gl_->DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0));
}

void GLRenderer::DrawYUVVideoQuad(const DrawingFrame* frame,
                                  const YUVVideoDrawQuad* quad) {
  SetBlendEnabled(quad->ShouldDrawWithBlending());

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_,
      &highp_threshold_cache_,
      highp_threshold_min_,
      quad->shared_quad_state->visible_content_rect.bottom_right());

  bool use_alpha_plane = quad->a_plane_resource_id != 0;

  ResourceProvider::ScopedSamplerGL y_plane_lock(
      resource_provider_, quad->y_plane_resource_id, GL_TEXTURE1, GL_LINEAR);
  DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D), y_plane_lock.target());
  ResourceProvider::ScopedSamplerGL u_plane_lock(
      resource_provider_, quad->u_plane_resource_id, GL_TEXTURE2, GL_LINEAR);
  DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D), u_plane_lock.target());
  ResourceProvider::ScopedSamplerGL v_plane_lock(
      resource_provider_, quad->v_plane_resource_id, GL_TEXTURE3, GL_LINEAR);
  DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D), v_plane_lock.target());
  scoped_ptr<ResourceProvider::ScopedSamplerGL> a_plane_lock;
  if (use_alpha_plane) {
    a_plane_lock.reset(new ResourceProvider::ScopedSamplerGL(
        resource_provider_, quad->a_plane_resource_id, GL_TEXTURE4, GL_LINEAR));
    DCHECK_EQ(static_cast<GLenum>(GL_TEXTURE_2D), a_plane_lock->target());
  }

  int matrix_location = -1;
  int tex_scale_location = -1;
  int tex_offset_location = -1;
  int y_texture_location = -1;
  int u_texture_location = -1;
  int v_texture_location = -1;
  int a_texture_location = -1;
  int yuv_matrix_location = -1;
  int yuv_adj_location = -1;
  int alpha_location = -1;
  if (use_alpha_plane) {
    const VideoYUVAProgram* program = GetVideoYUVAProgram(tex_coord_precision);
    DCHECK(program && (program->initialized() || IsContextLost()));
    SetUseProgram(program->program());
    matrix_location = program->vertex_shader().matrix_location();
    tex_scale_location = program->vertex_shader().tex_scale_location();
    tex_offset_location = program->vertex_shader().tex_offset_location();
    y_texture_location = program->fragment_shader().y_texture_location();
    u_texture_location = program->fragment_shader().u_texture_location();
    v_texture_location = program->fragment_shader().v_texture_location();
    a_texture_location = program->fragment_shader().a_texture_location();
    yuv_matrix_location = program->fragment_shader().yuv_matrix_location();
    yuv_adj_location = program->fragment_shader().yuv_adj_location();
    alpha_location = program->fragment_shader().alpha_location();
  } else {
    const VideoYUVProgram* program = GetVideoYUVProgram(tex_coord_precision);
    DCHECK(program && (program->initialized() || IsContextLost()));
    SetUseProgram(program->program());
    matrix_location = program->vertex_shader().matrix_location();
    tex_scale_location = program->vertex_shader().tex_scale_location();
    tex_offset_location = program->vertex_shader().tex_offset_location();
    y_texture_location = program->fragment_shader().y_texture_location();
    u_texture_location = program->fragment_shader().u_texture_location();
    v_texture_location = program->fragment_shader().v_texture_location();
    yuv_matrix_location = program->fragment_shader().yuv_matrix_location();
    yuv_adj_location = program->fragment_shader().yuv_adj_location();
    alpha_location = program->fragment_shader().alpha_location();
  }

  GLC(gl_,
      gl_->Uniform2f(tex_scale_location,
                     quad->tex_coord_rect.width(),
                     quad->tex_coord_rect.height()));
  GLC(gl_,
      gl_->Uniform2f(tex_offset_location,
                     quad->tex_coord_rect.x(),
                     quad->tex_coord_rect.y()));
  GLC(gl_, gl_->Uniform1i(y_texture_location, 1));
  GLC(gl_, gl_->Uniform1i(u_texture_location, 2));
  GLC(gl_, gl_->Uniform1i(v_texture_location, 3));
  if (use_alpha_plane)
    GLC(gl_, gl_->Uniform1i(a_texture_location, 4));

  // These values are magic numbers that are used in the transformation from YUV
  // to RGB color values.  They are taken from the following webpage:
  // http://www.fourcc.org/fccyvrgb.php
  float yuv_to_rgb_rec601[9] = {
      1.164f, 1.164f, 1.164f, 0.0f, -.391f, 2.018f, 1.596f, -.813f, 0.0f,
  };
  float yuv_to_rgb_rec601_jpeg[9] = {
      1.f, 1.f, 1.f, 0.0f, -.34414f, 1.772f, 1.402f, -.71414f, 0.0f,
  };

  // These values map to 16, 128, and 128 respectively, and are computed
  // as a fraction over 256 (e.g. 16 / 256 = 0.0625).
  // They are used in the YUV to RGBA conversion formula:
  //   Y - 16   : Gives 16 values of head and footroom for overshooting
  //   U - 128  : Turns unsigned U into signed U [-128,127]
  //   V - 128  : Turns unsigned V into signed V [-128,127]
  float yuv_adjust_rec601[3] = {
      -0.0625f, -0.5f, -0.5f,
  };

  // Same as above, but without the head and footroom.
  float yuv_adjust_rec601_jpeg[3] = {
      0.0f, -0.5f, -0.5f,
  };

  float* yuv_to_rgb = NULL;
  float* yuv_adjust = NULL;

  switch (quad->color_space) {
    case YUVVideoDrawQuad::REC_601:
      yuv_to_rgb = yuv_to_rgb_rec601;
      yuv_adjust = yuv_adjust_rec601;
      break;
    case YUVVideoDrawQuad::REC_601_JPEG:
      yuv_to_rgb = yuv_to_rgb_rec601_jpeg;
      yuv_adjust = yuv_adjust_rec601_jpeg;
      break;
  }

  GLC(gl_, gl_->UniformMatrix3fv(yuv_matrix_location, 1, 0, yuv_to_rgb));
  GLC(gl_, gl_->Uniform3fv(yuv_adj_location, 1, yuv_adjust));

  SetShaderOpacity(quad->opacity(), alpha_location);
  DrawQuadGeometry(frame, quad->quadTransform(), quad->rect, matrix_location);
}

void GLRenderer::DrawStreamVideoQuad(const DrawingFrame* frame,
                                     const StreamVideoDrawQuad* quad) {
  SetBlendEnabled(quad->ShouldDrawWithBlending());

  static float gl_matrix[16];

  DCHECK(capabilities_.using_egl_image);

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_,
      &highp_threshold_cache_,
      highp_threshold_min_,
      quad->shared_quad_state->visible_content_rect.bottom_right());

  const VideoStreamTextureProgram* program =
      GetVideoStreamTextureProgram(tex_coord_precision);
  SetUseProgram(program->program());

  ToGLMatrix(&gl_matrix[0], quad->matrix);
  GLC(gl_,
      gl_->UniformMatrix4fv(
          program->vertex_shader().tex_matrix_location(), 1, false, gl_matrix));

  ResourceProvider::ScopedReadLockGL lock(resource_provider_,
                                          quad->resource_id);
  DCHECK_EQ(GL_TEXTURE0, GetActiveTextureUnit(gl_));
  GLC(gl_, gl_->BindTexture(GL_TEXTURE_EXTERNAL_OES, lock.texture_id()));

  GLC(gl_, gl_->Uniform1i(program->fragment_shader().sampler_location(), 0));

  SetShaderOpacity(quad->opacity(),
                   program->fragment_shader().alpha_location());
  DrawQuadGeometry(frame,
                   quad->quadTransform(),
                   quad->rect,
                   program->vertex_shader().matrix_location());
}

void GLRenderer::DrawPictureQuad(const DrawingFrame* frame,
                                 const PictureDrawQuad* quad) {
  if (on_demand_tile_raster_bitmap_.width() != quad->texture_size.width() ||
      on_demand_tile_raster_bitmap_.height() != quad->texture_size.height()) {
    on_demand_tile_raster_bitmap_.allocN32Pixels(quad->texture_size.width(),
                                                 quad->texture_size.height());

    if (on_demand_tile_raster_resource_id_)
      resource_provider_->DeleteResource(on_demand_tile_raster_resource_id_);

    on_demand_tile_raster_resource_id_ = resource_provider_->CreateGLTexture(
        quad->texture_size,
        GL_TEXTURE_2D,
        GL_TEXTURE_POOL_UNMANAGED_CHROMIUM,
        GL_CLAMP_TO_EDGE,
        ResourceProvider::TextureHintImmutable,
        quad->texture_format);
  }

  SkCanvas canvas(on_demand_tile_raster_bitmap_);
  quad->raster_source->PlaybackToCanvas(&canvas, quad->content_rect,
                                        quad->contents_scale);

  uint8_t* bitmap_pixels = NULL;
  SkBitmap on_demand_tile_raster_bitmap_dest;
  SkColorType colorType = ResourceFormatToSkColorType(quad->texture_format);
  if (on_demand_tile_raster_bitmap_.colorType() != colorType) {
    on_demand_tile_raster_bitmap_.copyTo(&on_demand_tile_raster_bitmap_dest,
                                         colorType);
    // TODO(kaanb): The GL pipeline assumes a 4-byte alignment for the
    // bitmap data. This check will be removed once crbug.com/293728 is fixed.
    CHECK_EQ(0u, on_demand_tile_raster_bitmap_dest.rowBytes() % 4);
    bitmap_pixels = reinterpret_cast<uint8_t*>(
        on_demand_tile_raster_bitmap_dest.getPixels());
  } else {
    bitmap_pixels =
        reinterpret_cast<uint8_t*>(on_demand_tile_raster_bitmap_.getPixels());
  }

  resource_provider_->SetPixels(on_demand_tile_raster_resource_id_,
                                bitmap_pixels,
                                gfx::Rect(quad->texture_size),
                                gfx::Rect(quad->texture_size),
                                gfx::Vector2d());

  DrawContentQuad(frame, quad, on_demand_tile_raster_resource_id_);
}

struct TextureProgramBinding {
  template <class Program>
  void Set(Program* program) {
    DCHECK(program);
    program_id = program->program();
    sampler_location = program->fragment_shader().sampler_location();
    matrix_location = program->vertex_shader().matrix_location();
    background_color_location =
        program->fragment_shader().background_color_location();
  }
  int program_id;
  int sampler_location;
  int matrix_location;
  int background_color_location;
};

struct TexTransformTextureProgramBinding : TextureProgramBinding {
  template <class Program>
  void Set(Program* program) {
    TextureProgramBinding::Set(program);
    tex_transform_location = program->vertex_shader().tex_transform_location();
    vertex_opacity_location =
        program->vertex_shader().vertex_opacity_location();
  }
  int tex_transform_location;
  int vertex_opacity_location;
};

void GLRenderer::FlushTextureQuadCache() {
  // Check to see if we have anything to draw.
  if (draw_cache_.program_id == -1)
    return;

  // Set the correct blending mode.
  SetBlendEnabled(draw_cache_.needs_blending);

  // Bind the program to the GL state.
  SetUseProgram(draw_cache_.program_id);

  // Bind the correct texture sampler location.
  GLC(gl_, gl_->Uniform1i(draw_cache_.sampler_location, 0));

  // Assume the current active textures is 0.
  ResourceProvider::ScopedSamplerGL locked_quad(
      resource_provider_,
      draw_cache_.resource_id,
      draw_cache_.nearest_neighbor ? GL_NEAREST : GL_LINEAR);
  DCHECK_EQ(GL_TEXTURE0, GetActiveTextureUnit(gl_));
  GLC(gl_, gl_->BindTexture(GL_TEXTURE_2D, locked_quad.texture_id()));

  static_assert(sizeof(Float4) == 4 * sizeof(float),
                "Float4 struct should be densely packed");
  static_assert(sizeof(Float16) == 16 * sizeof(float),
                "Float16 struct should be densely packed");

  // Upload the tranforms for both points and uvs.
  GLC(gl_,
      gl_->UniformMatrix4fv(
          static_cast<int>(draw_cache_.matrix_location),
          static_cast<int>(draw_cache_.matrix_data.size()),
          false,
          reinterpret_cast<float*>(&draw_cache_.matrix_data.front())));
  GLC(gl_,
      gl_->Uniform4fv(
          static_cast<int>(draw_cache_.uv_xform_location),
          static_cast<int>(draw_cache_.uv_xform_data.size()),
          reinterpret_cast<float*>(&draw_cache_.uv_xform_data.front())));

  if (draw_cache_.background_color != SK_ColorTRANSPARENT) {
    Float4 background_color = PremultipliedColor(draw_cache_.background_color);
    GLC(gl_,
        gl_->Uniform4fv(
            draw_cache_.background_color_location, 1, background_color.data));
  }

  GLC(gl_,
      gl_->Uniform1fv(
          static_cast<int>(draw_cache_.vertex_opacity_location),
          static_cast<int>(draw_cache_.vertex_opacity_data.size()),
          static_cast<float*>(&draw_cache_.vertex_opacity_data.front())));

  // Draw the quads!
  GLC(gl_,
      gl_->DrawElements(GL_TRIANGLES,
                        6 * draw_cache_.matrix_data.size(),
                        GL_UNSIGNED_SHORT,
                        0));

  // Clear the cache.
  draw_cache_.program_id = -1;
  draw_cache_.uv_xform_data.resize(0);
  draw_cache_.vertex_opacity_data.resize(0);
  draw_cache_.matrix_data.resize(0);
}

void GLRenderer::EnqueueTextureQuad(const DrawingFrame* frame,
                                    const TextureDrawQuad* quad) {
  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_,
      &highp_threshold_cache_,
      highp_threshold_min_,
      quad->shared_quad_state->visible_content_rect.bottom_right());

  // Choose the correct texture program binding
  TexTransformTextureProgramBinding binding;
  if (quad->premultiplied_alpha) {
    if (quad->background_color == SK_ColorTRANSPARENT) {
      binding.Set(GetTextureProgram(tex_coord_precision));
    } else {
      binding.Set(GetTextureBackgroundProgram(tex_coord_precision));
    }
  } else {
    if (quad->background_color == SK_ColorTRANSPARENT) {
      binding.Set(GetNonPremultipliedTextureProgram(tex_coord_precision));
    } else {
      binding.Set(
          GetNonPremultipliedTextureBackgroundProgram(tex_coord_precision));
    }
  }

  int resource_id = quad->resource_id;

  if (draw_cache_.program_id != binding.program_id ||
      draw_cache_.resource_id != resource_id ||
      draw_cache_.needs_blending != quad->ShouldDrawWithBlending() ||
      draw_cache_.nearest_neighbor != quad->nearest_neighbor ||
      draw_cache_.background_color != quad->background_color ||
      draw_cache_.matrix_data.size() >= 8) {
    FlushTextureQuadCache();
    draw_cache_.program_id = binding.program_id;
    draw_cache_.resource_id = resource_id;
    draw_cache_.needs_blending = quad->ShouldDrawWithBlending();
    draw_cache_.nearest_neighbor = quad->nearest_neighbor;
    draw_cache_.background_color = quad->background_color;

    draw_cache_.uv_xform_location = binding.tex_transform_location;
    draw_cache_.background_color_location = binding.background_color_location;
    draw_cache_.vertex_opacity_location = binding.vertex_opacity_location;
    draw_cache_.matrix_location = binding.matrix_location;
    draw_cache_.sampler_location = binding.sampler_location;
  }

  // Generate the uv-transform
  draw_cache_.uv_xform_data.push_back(UVTransform(quad));

  // Generate the vertex opacity
  const float opacity = quad->opacity();
  draw_cache_.vertex_opacity_data.push_back(quad->vertex_opacity[0] * opacity);
  draw_cache_.vertex_opacity_data.push_back(quad->vertex_opacity[1] * opacity);
  draw_cache_.vertex_opacity_data.push_back(quad->vertex_opacity[2] * opacity);
  draw_cache_.vertex_opacity_data.push_back(quad->vertex_opacity[3] * opacity);

  // Generate the transform matrix
  gfx::Transform quad_rect_matrix;
  QuadRectTransform(&quad_rect_matrix, quad->quadTransform(), quad->rect);
  quad_rect_matrix = frame->projection_matrix * quad_rect_matrix;

  Float16 m;
  quad_rect_matrix.matrix().asColMajorf(m.data);
  draw_cache_.matrix_data.push_back(m);
}

void GLRenderer::DrawIOSurfaceQuad(const DrawingFrame* frame,
                                   const IOSurfaceDrawQuad* quad) {
  SetBlendEnabled(quad->ShouldDrawWithBlending());

  TexCoordPrecision tex_coord_precision = TexCoordPrecisionRequired(
      gl_,
      &highp_threshold_cache_,
      highp_threshold_min_,
      quad->shared_quad_state->visible_content_rect.bottom_right());

  TexTransformTextureProgramBinding binding;
  binding.Set(GetTextureIOSurfaceProgram(tex_coord_precision));

  SetUseProgram(binding.program_id);
  GLC(gl_, gl_->Uniform1i(binding.sampler_location, 0));
  if (quad->orientation == IOSurfaceDrawQuad::FLIPPED) {
    GLC(gl_,
        gl_->Uniform4f(binding.tex_transform_location,
                       0,
                       quad->io_surface_size.height(),
                       quad->io_surface_size.width(),
                       quad->io_surface_size.height() * -1.0f));
  } else {
    GLC(gl_,
        gl_->Uniform4f(binding.tex_transform_location,
                       0,
                       0,
                       quad->io_surface_size.width(),
                       quad->io_surface_size.height()));
  }

  const float vertex_opacity[] = {quad->opacity(), quad->opacity(),
                                  quad->opacity(), quad->opacity()};
  GLC(gl_, gl_->Uniform1fv(binding.vertex_opacity_location, 4, vertex_opacity));

  ResourceProvider::ScopedReadLockGL lock(resource_provider_,
                                          quad->io_surface_resource_id);
  DCHECK_EQ(GL_TEXTURE0, GetActiveTextureUnit(gl_));
  GLC(gl_, gl_->BindTexture(GL_TEXTURE_RECTANGLE_ARB, lock.texture_id()));

  DrawQuadGeometry(
      frame, quad->quadTransform(), quad->rect, binding.matrix_location);

  GLC(gl_, gl_->BindTexture(GL_TEXTURE_RECTANGLE_ARB, 0));
}

void GLRenderer::FinishDrawingFrame(DrawingFrame* frame) {
  if (use_sync_query_) {
    DCHECK(current_sync_query_);
    current_sync_query_->End();
    pending_sync_queries_.push_back(current_sync_query_.Pass());
  }

  current_framebuffer_lock_ = nullptr;
  swap_buffer_rect_.Union(gfx::ToEnclosingRect(frame->root_damage_rect));

  GLC(gl_, gl_->Disable(GL_BLEND));
  blend_shadow_ = false;

  ScheduleOverlays(frame);
}

void GLRenderer::FinishDrawingQuadList() { FlushTextureQuadCache(); }

bool GLRenderer::FlippedFramebuffer(const DrawingFrame* frame) const {
  if (frame->current_render_pass != frame->root_render_pass)
    return true;
  return FlippedRootFramebuffer();
}

bool GLRenderer::FlippedRootFramebuffer() const {
  // GL is normally flipped, so a flipped output results in an unflipping.
  return !output_surface_->capabilities().flipped_output_surface;
}

void GLRenderer::EnsureScissorTestEnabled() {
  if (is_scissor_enabled_)
    return;

  FlushTextureQuadCache();
  GLC(gl_, gl_->Enable(GL_SCISSOR_TEST));
  is_scissor_enabled_ = true;
}

void GLRenderer::EnsureScissorTestDisabled() {
  if (!is_scissor_enabled_)
    return;

  FlushTextureQuadCache();
  GLC(gl_, gl_->Disable(GL_SCISSOR_TEST));
  is_scissor_enabled_ = false;
}

void GLRenderer::CopyCurrentRenderPassToBitmap(
    DrawingFrame* frame,
    scoped_ptr<CopyOutputRequest> request) {
  TRACE_EVENT0("cc", "GLRenderer::CopyCurrentRenderPassToBitmap");
  gfx::Rect copy_rect = frame->current_render_pass->output_rect;
  if (request->has_area())
    copy_rect.Intersect(request->area());
  GetFramebufferPixelsAsync(frame, copy_rect, request.Pass());
}

void GLRenderer::ToGLMatrix(float* gl_matrix, const gfx::Transform& transform) {
  transform.matrix().asColMajorf(gl_matrix);
}

void GLRenderer::SetShaderQuadF(const gfx::QuadF& quad, int quad_location) {
  if (quad_location == -1)
    return;

  float gl_quad[8];
  gl_quad[0] = quad.p1().x();
  gl_quad[1] = quad.p1().y();
  gl_quad[2] = quad.p2().x();
  gl_quad[3] = quad.p2().y();
  gl_quad[4] = quad.p3().x();
  gl_quad[5] = quad.p3().y();
  gl_quad[6] = quad.p4().x();
  gl_quad[7] = quad.p4().y();
  GLC(gl_, gl_->Uniform2fv(quad_location, 4, gl_quad));
}

void GLRenderer::SetShaderOpacity(float opacity, int alpha_location) {
  if (alpha_location != -1)
    GLC(gl_, gl_->Uniform1f(alpha_location, opacity));
}

void GLRenderer::SetStencilEnabled(bool enabled) {
  if (enabled == stencil_shadow_)
    return;

  if (enabled)
    GLC(gl_, gl_->Enable(GL_STENCIL_TEST));
  else
    GLC(gl_, gl_->Disable(GL_STENCIL_TEST));
  stencil_shadow_ = enabled;
}

void GLRenderer::SetBlendEnabled(bool enabled) {
  if (enabled == blend_shadow_)
    return;

  if (enabled)
    GLC(gl_, gl_->Enable(GL_BLEND));
  else
    GLC(gl_, gl_->Disable(GL_BLEND));
  blend_shadow_ = enabled;
}

void GLRenderer::SetUseProgram(unsigned program) {
  if (program == program_shadow_)
    return;
  gl_->UseProgram(program);
  program_shadow_ = program;
}

void GLRenderer::DrawQuadGeometry(const DrawingFrame* frame,
                                  const gfx::Transform& draw_transform,
                                  const gfx::RectF& quad_rect,
                                  int matrix_location) {
  gfx::Transform quad_rect_matrix;
  QuadRectTransform(&quad_rect_matrix, draw_transform, quad_rect);
  static float gl_matrix[16];
  ToGLMatrix(&gl_matrix[0], frame->projection_matrix * quad_rect_matrix);
  GLC(gl_, gl_->UniformMatrix4fv(matrix_location, 1, false, &gl_matrix[0]));

  GLC(gl_, gl_->DrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, 0));
}

void GLRenderer::Finish() {
  TRACE_EVENT0("cc", "GLRenderer::Finish");
  GLC(gl_, gl_->Finish());
}

void GLRenderer::SwapBuffers(const CompositorFrameMetadata& metadata) {
  DCHECK(!is_backbuffer_discarded_);

  TRACE_EVENT0("cc,benchmark", "GLRenderer::SwapBuffers");
  // We're done! Time to swapbuffers!

  gfx::Size surface_size = output_surface_->SurfaceSize();

  CompositorFrame compositor_frame;
  compositor_frame.metadata = metadata;
  compositor_frame.gl_frame_data = make_scoped_ptr(new GLFrameData);
  compositor_frame.gl_frame_data->size = surface_size;
  if (capabilities_.using_partial_swap) {
    // If supported, we can save significant bandwidth by only swapping the
    // damaged/scissored region (clamped to the viewport).
    swap_buffer_rect_.Intersect(gfx::Rect(surface_size));
    int flipped_y_pos_of_rect_bottom = surface_size.height() -
                                       swap_buffer_rect_.y() -
                                       swap_buffer_rect_.height();
    compositor_frame.gl_frame_data->sub_buffer_rect =
        gfx::Rect(swap_buffer_rect_.x(),
                  FlippedRootFramebuffer() ? flipped_y_pos_of_rect_bottom
                                           : swap_buffer_rect_.y(),
                  swap_buffer_rect_.width(),
                  swap_buffer_rect_.height());
  } else {
    compositor_frame.gl_frame_data->sub_buffer_rect =
        gfx::Rect(output_surface_->SurfaceSize());
  }
  output_surface_->SwapBuffers(&compositor_frame);

  // Release previously used overlay resources and hold onto the pending ones
  // until the next swap buffers.
  in_use_overlay_resources_.clear();
  in_use_overlay_resources_.swap(pending_overlay_resources_);

  swap_buffer_rect_ = gfx::Rect();
}

void GLRenderer::EnforceMemoryPolicy() {
  if (!visible()) {
    TRACE_EVENT0("cc", "GLRenderer::EnforceMemoryPolicy dropping resources");
    ReleaseRenderPassTextures();
    DiscardBackbuffer();
    resource_provider_->ReleaseCachedData();
    output_surface_->context_provider()->DeleteCachedResources();
    GLC(gl_, gl_->Flush());
  }
}

void GLRenderer::DiscardBackbuffer() {
  if (is_backbuffer_discarded_)
    return;

  output_surface_->DiscardBackbuffer();

  is_backbuffer_discarded_ = true;

  // Damage tracker needs a full reset every time framebuffer is discarded.
  client_->SetFullRootLayerDamage();
}

void GLRenderer::EnsureBackbuffer() {
  if (!is_backbuffer_discarded_)
    return;

  output_surface_->EnsureBackbuffer();
  is_backbuffer_discarded_ = false;
}

void GLRenderer::GetFramebufferPixelsAsync(
    const DrawingFrame* frame,
    const gfx::Rect& rect,
    scoped_ptr<CopyOutputRequest> request) {
  DCHECK(!request->IsEmpty());
  if (request->IsEmpty())
    return;
  if (rect.IsEmpty())
    return;

  gfx::Rect window_rect = MoveFromDrawToWindowSpace(frame, rect);
  DCHECK_GE(window_rect.x(), 0);
  DCHECK_GE(window_rect.y(), 0);
  DCHECK_LE(window_rect.right(), current_surface_size_.width());
  DCHECK_LE(window_rect.bottom(), current_surface_size_.height());

  if (!request->force_bitmap_result()) {
    bool own_mailbox = !request->has_texture_mailbox();

    GLuint texture_id = 0;
    gpu::Mailbox mailbox;
    if (own_mailbox) {
      GLC(gl_, gl_->GenMailboxCHROMIUM(mailbox.name));
      gl_->GenTextures(1, &texture_id);
      GLC(gl_, gl_->BindTexture(GL_TEXTURE_2D, texture_id));

      GLC(gl_,
          gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
      GLC(gl_,
          gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
      GLC(gl_,
          gl_->TexParameteri(
              GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
      GLC(gl_,
          gl_->TexParameteri(
              GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
      GLC(gl_, gl_->ProduceTextureCHROMIUM(GL_TEXTURE_2D, mailbox.name));
    } else {
      mailbox = request->texture_mailbox().mailbox();
      DCHECK_EQ(static_cast<unsigned>(GL_TEXTURE_2D),
                request->texture_mailbox().target());
      DCHECK(!mailbox.IsZero());
      unsigned incoming_sync_point = request->texture_mailbox().sync_point();
      if (incoming_sync_point)
        GLC(gl_, gl_->WaitSyncPointCHROMIUM(incoming_sync_point));

      texture_id = GLC(
          gl_,
          gl_->CreateAndConsumeTextureCHROMIUM(GL_TEXTURE_2D, mailbox.name));
    }
    GetFramebufferTexture(texture_id, RGBA_8888, window_rect);

    unsigned sync_point = gl_->InsertSyncPointCHROMIUM();
    TextureMailbox texture_mailbox(mailbox, GL_TEXTURE_2D, sync_point);

    scoped_ptr<SingleReleaseCallback> release_callback;
    if (own_mailbox) {
      GLC(gl_, gl_->BindTexture(GL_TEXTURE_2D, 0));
      release_callback = texture_mailbox_deleter_->GetReleaseCallback(
          output_surface_->context_provider(), texture_id);
    } else {
      gl_->DeleteTextures(1, &texture_id);
    }

    request->SendTextureResult(
        window_rect.size(), texture_mailbox, release_callback.Pass());
    return;
  }

  DCHECK(request->force_bitmap_result());

  scoped_ptr<PendingAsyncReadPixels> pending_read(new PendingAsyncReadPixels);
  pending_read->copy_request = request.Pass();
  pending_async_read_pixels_.insert(pending_async_read_pixels_.begin(),
                                    pending_read.Pass());

  bool do_workaround = NeedsIOSurfaceReadbackWorkaround();

  unsigned temporary_texture = 0;
  unsigned temporary_fbo = 0;

  if (do_workaround) {
    // On Mac OS X, calling glReadPixels() against an FBO whose color attachment
    // is an IOSurface-backed texture causes corruption of future glReadPixels()
    // calls, even those on different OpenGL contexts. It is believed that this
    // is the root cause of top crasher
    // http://crbug.com/99393. <rdar://problem/10949687>

    gl_->GenTextures(1, &temporary_texture);
    GLC(gl_, gl_->BindTexture(GL_TEXTURE_2D, temporary_texture));
    GLC(gl_,
        gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GLC(gl_,
        gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GLC(gl_,
        gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLC(gl_,
        gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    // Copy the contents of the current (IOSurface-backed) framebuffer into a
    // temporary texture.
    GetFramebufferTexture(
        temporary_texture, RGBA_8888, gfx::Rect(current_surface_size_));
    gl_->GenFramebuffers(1, &temporary_fbo);
    // Attach this texture to an FBO, and perform the readback from that FBO.
    GLC(gl_, gl_->BindFramebuffer(GL_FRAMEBUFFER, temporary_fbo));
    GLC(gl_,
        gl_->FramebufferTexture2D(GL_FRAMEBUFFER,
                                  GL_COLOR_ATTACHMENT0,
                                  GL_TEXTURE_2D,
                                  temporary_texture,
                                  0));

    DCHECK_EQ(static_cast<unsigned>(GL_FRAMEBUFFER_COMPLETE),
              gl_->CheckFramebufferStatus(GL_FRAMEBUFFER));
  }

  GLuint buffer = 0;
  gl_->GenBuffers(1, &buffer);
  GLC(gl_, gl_->BindBuffer(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, buffer));
  GLC(gl_,
      gl_->BufferData(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM,
                      4 * window_rect.size().GetArea(),
                      NULL,
                      GL_STREAM_READ));

  GLuint query = 0;
  gl_->GenQueriesEXT(1, &query);
  GLC(gl_, gl_->BeginQueryEXT(GL_ASYNC_PIXEL_PACK_COMPLETED_CHROMIUM, query));

  GLC(gl_,
      gl_->ReadPixels(window_rect.x(),
                      window_rect.y(),
                      window_rect.width(),
                      window_rect.height(),
                      GL_RGBA,
                      GL_UNSIGNED_BYTE,
                      NULL));

  GLC(gl_, gl_->BindBuffer(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, 0));

  if (do_workaround) {
    // Clean up.
    GLC(gl_, gl_->BindFramebuffer(GL_FRAMEBUFFER, 0));
    GLC(gl_, gl_->BindTexture(GL_TEXTURE_2D, 0));
    GLC(gl_, gl_->DeleteFramebuffers(1, &temporary_fbo));
    GLC(gl_, gl_->DeleteTextures(1, &temporary_texture));
  }

  base::Closure finished_callback = base::Bind(&GLRenderer::FinishedReadback,
                                               base::Unretained(this),
                                               buffer,
                                               query,
                                               window_rect.size());
  // Save the finished_callback so it can be cancelled.
  pending_async_read_pixels_.front()->finished_read_pixels_callback.Reset(
      finished_callback);
  base::Closure cancelable_callback =
      pending_async_read_pixels_.front()->
          finished_read_pixels_callback.callback();

  // Save the buffer to verify the callbacks happen in the expected order.
  pending_async_read_pixels_.front()->buffer = buffer;

  GLC(gl_, gl_->EndQueryEXT(GL_ASYNC_PIXEL_PACK_COMPLETED_CHROMIUM));
  context_support_->SignalQuery(query, cancelable_callback);

  EnforceMemoryPolicy();
}

void GLRenderer::FinishedReadback(unsigned source_buffer,
                                  unsigned query,
                                  const gfx::Size& size) {
  DCHECK(!pending_async_read_pixels_.empty());

  if (query != 0) {
    GLC(gl_, gl_->DeleteQueriesEXT(1, &query));
  }

  PendingAsyncReadPixels* current_read = pending_async_read_pixels_.back();
  // Make sure we service the readbacks in order.
  DCHECK_EQ(source_buffer, current_read->buffer);

  uint8* src_pixels = NULL;
  scoped_ptr<SkBitmap> bitmap;

  if (source_buffer != 0) {
    GLC(gl_,
        gl_->BindBuffer(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, source_buffer));
    src_pixels = static_cast<uint8*>(gl_->MapBufferCHROMIUM(
        GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, GL_READ_ONLY));

    if (src_pixels) {
      bitmap.reset(new SkBitmap);
      bitmap->allocN32Pixels(size.width(), size.height());
      scoped_ptr<SkAutoLockPixels> lock(new SkAutoLockPixels(*bitmap));
      uint8* dest_pixels = static_cast<uint8*>(bitmap->getPixels());

      size_t row_bytes = size.width() * 4;
      int num_rows = size.height();
      size_t total_bytes = num_rows * row_bytes;
      for (size_t dest_y = 0; dest_y < total_bytes; dest_y += row_bytes) {
        // Flip Y axis.
        size_t src_y = total_bytes - dest_y - row_bytes;
        // Swizzle OpenGL -> Skia byte order.
        for (size_t x = 0; x < row_bytes; x += 4) {
          dest_pixels[dest_y + x + SK_R32_SHIFT / 8] =
              src_pixels[src_y + x + 0];
          dest_pixels[dest_y + x + SK_G32_SHIFT / 8] =
              src_pixels[src_y + x + 1];
          dest_pixels[dest_y + x + SK_B32_SHIFT / 8] =
              src_pixels[src_y + x + 2];
          dest_pixels[dest_y + x + SK_A32_SHIFT / 8] =
              src_pixels[src_y + x + 3];
        }
      }

      GLC(gl_,
          gl_->UnmapBufferCHROMIUM(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM));
    }
    GLC(gl_, gl_->BindBuffer(GL_PIXEL_PACK_TRANSFER_BUFFER_CHROMIUM, 0));
    GLC(gl_, gl_->DeleteBuffers(1, &source_buffer));
  }

  if (bitmap)
    current_read->copy_request->SendBitmapResult(bitmap.Pass());
  pending_async_read_pixels_.pop_back();
}

void GLRenderer::GetFramebufferTexture(unsigned texture_id,
                                       ResourceFormat texture_format,
                                       const gfx::Rect& window_rect) {
  DCHECK(texture_id);
  DCHECK_GE(window_rect.x(), 0);
  DCHECK_GE(window_rect.y(), 0);
  DCHECK_LE(window_rect.right(), current_surface_size_.width());
  DCHECK_LE(window_rect.bottom(), current_surface_size_.height());

  GLC(gl_, gl_->BindTexture(GL_TEXTURE_2D, texture_id));
  GLC(gl_,
      gl_->CopyTexImage2D(GL_TEXTURE_2D,
                          0,
                          GLDataFormat(texture_format),
                          window_rect.x(),
                          window_rect.y(),
                          window_rect.width(),
                          window_rect.height(),
                          0));
  GLC(gl_, gl_->BindTexture(GL_TEXTURE_2D, 0));
}

bool GLRenderer::UseScopedTexture(DrawingFrame* frame,
                                  const ScopedResource* texture,
                                  const gfx::Rect& viewport_rect) {
  DCHECK(texture->id());
  frame->current_render_pass = NULL;
  frame->current_texture = texture;

  return BindFramebufferToTexture(frame, texture, viewport_rect);
}

void GLRenderer::BindFramebufferToOutputSurface(DrawingFrame* frame) {
  current_framebuffer_lock_ = nullptr;
  output_surface_->BindFramebuffer();

  if (output_surface_->HasExternalStencilTest()) {
    SetStencilEnabled(true);
    GLC(gl_, gl_->StencilFunc(GL_EQUAL, 1, 1));
  } else {
    SetStencilEnabled(false);
  }
}

bool GLRenderer::BindFramebufferToTexture(DrawingFrame* frame,
                                          const ScopedResource* texture,
                                          const gfx::Rect& target_rect) {
  DCHECK(texture->id());

  current_framebuffer_lock_ = nullptr;

  SetStencilEnabled(false);
  GLC(gl_, gl_->BindFramebuffer(GL_FRAMEBUFFER, offscreen_framebuffer_id_));
  current_framebuffer_lock_ =
      make_scoped_ptr(new ResourceProvider::ScopedWriteLockGL(
          resource_provider_, texture->id()));
  unsigned texture_id = current_framebuffer_lock_->texture_id();
  GLC(gl_,
      gl_->FramebufferTexture2D(
          GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_id, 0));

  DCHECK(gl_->CheckFramebufferStatus(GL_FRAMEBUFFER) ==
             GL_FRAMEBUFFER_COMPLETE ||
         IsContextLost());

  InitializeViewport(
      frame, target_rect, gfx::Rect(target_rect.size()), target_rect.size());
  return true;
}

void GLRenderer::SetScissorTestRect(const gfx::Rect& scissor_rect) {
  EnsureScissorTestEnabled();

  // Don't unnecessarily ask the context to change the scissor, because it
  // may cause undesired GPU pipeline flushes.
  if (scissor_rect == scissor_rect_ && !scissor_rect_needs_reset_)
    return;

  scissor_rect_ = scissor_rect;
  FlushTextureQuadCache();
  GLC(gl_,
      gl_->Scissor(scissor_rect.x(),
                   scissor_rect.y(),
                   scissor_rect.width(),
                   scissor_rect.height()));

  scissor_rect_needs_reset_ = false;
}

void GLRenderer::SetDrawViewport(const gfx::Rect& window_space_viewport) {
  viewport_ = window_space_viewport;
  GLC(gl_,
      gl_->Viewport(window_space_viewport.x(),
                    window_space_viewport.y(),
                    window_space_viewport.width(),
                    window_space_viewport.height()));
}

void GLRenderer::InitializeSharedObjects() {
  TRACE_EVENT0("cc", "GLRenderer::InitializeSharedObjects");

  // Create an FBO for doing offscreen rendering.
  GLC(gl_, gl_->GenFramebuffers(1, &offscreen_framebuffer_id_));

  shared_geometry_ = make_scoped_ptr(
      new GeometryBinding(gl_, QuadVertexRect()));
}

const GLRenderer::TileCheckerboardProgram*
GLRenderer::GetTileCheckerboardProgram() {
  if (!tile_checkerboard_program_.initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::checkerboardProgram::initalize");
    tile_checkerboard_program_.Initialize(output_surface_->context_provider(),
                                          TexCoordPrecisionNA,
                                          SamplerTypeNA);
  }
  return &tile_checkerboard_program_;
}

const GLRenderer::DebugBorderProgram* GLRenderer::GetDebugBorderProgram() {
  if (!debug_border_program_.initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::debugBorderProgram::initialize");
    debug_border_program_.Initialize(output_surface_->context_provider(),
                                     TexCoordPrecisionNA,
                                     SamplerTypeNA);
  }
  return &debug_border_program_;
}

const GLRenderer::SolidColorProgram* GLRenderer::GetSolidColorProgram() {
  if (!solid_color_program_.initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::solidColorProgram::initialize");
    solid_color_program_.Initialize(output_surface_->context_provider(),
                                    TexCoordPrecisionNA,
                                    SamplerTypeNA);
  }
  return &solid_color_program_;
}

const GLRenderer::SolidColorProgramAA* GLRenderer::GetSolidColorProgramAA() {
  if (!solid_color_program_aa_.initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::solidColorProgramAA::initialize");
    solid_color_program_aa_.Initialize(output_surface_->context_provider(),
                                       TexCoordPrecisionNA,
                                       SamplerTypeNA);
  }
  return &solid_color_program_aa_;
}

const GLRenderer::RenderPassProgram* GLRenderer::GetRenderPassProgram(
    TexCoordPrecision precision,
    BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LT(blend_mode, NumBlendModes);
  RenderPassProgram* program = &render_pass_program_[precision][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassProgram::initialize");
    program->Initialize(output_surface_->context_provider(),
                        precision,
                        SamplerType2D,
                        blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassProgramAA* GLRenderer::GetRenderPassProgramAA(
    TexCoordPrecision precision,
    BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LT(blend_mode, NumBlendModes);
  RenderPassProgramAA* program =
      &render_pass_program_aa_[precision][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassProgramAA::initialize");
    program->Initialize(output_surface_->context_provider(),
                        precision,
                        SamplerType2D,
                        blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassMaskProgram* GLRenderer::GetRenderPassMaskProgram(
    TexCoordPrecision precision,
    SamplerType sampler,
    BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LT(blend_mode, NumBlendModes);
  RenderPassMaskProgram* program =
      &render_pass_mask_program_[precision][sampler][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassMaskProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler, blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassMaskProgramAA*
GLRenderer::GetRenderPassMaskProgramAA(TexCoordPrecision precision,
                                       SamplerType sampler,
                                       BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LT(blend_mode, NumBlendModes);
  RenderPassMaskProgramAA* program =
      &render_pass_mask_program_aa_[precision][sampler][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassMaskProgramAA::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler, blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassColorMatrixProgram*
GLRenderer::GetRenderPassColorMatrixProgram(TexCoordPrecision precision,
                                            BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LT(blend_mode, NumBlendModes);
  RenderPassColorMatrixProgram* program =
      &render_pass_color_matrix_program_[precision][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::renderPassColorMatrixProgram::initialize");
    program->Initialize(output_surface_->context_provider(),
                        precision,
                        SamplerType2D,
                        blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassColorMatrixProgramAA*
GLRenderer::GetRenderPassColorMatrixProgramAA(TexCoordPrecision precision,
                                              BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LT(blend_mode, NumBlendModes);
  RenderPassColorMatrixProgramAA* program =
      &render_pass_color_matrix_program_aa_[precision][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::renderPassColorMatrixProgramAA::initialize");
    program->Initialize(output_surface_->context_provider(),
                        precision,
                        SamplerType2D,
                        blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassMaskColorMatrixProgram*
GLRenderer::GetRenderPassMaskColorMatrixProgram(TexCoordPrecision precision,
                                                SamplerType sampler,
                                                BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LT(blend_mode, NumBlendModes);
  RenderPassMaskColorMatrixProgram* program =
      &render_pass_mask_color_matrix_program_[precision][sampler][blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::renderPassMaskColorMatrixProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler, blend_mode);
  }
  return program;
}

const GLRenderer::RenderPassMaskColorMatrixProgramAA*
GLRenderer::GetRenderPassMaskColorMatrixProgramAA(TexCoordPrecision precision,
                                                  SamplerType sampler,
                                                  BlendMode blend_mode) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  DCHECK_GE(blend_mode, 0);
  DCHECK_LT(blend_mode, NumBlendModes);
  RenderPassMaskColorMatrixProgramAA* program =
      &render_pass_mask_color_matrix_program_aa_[precision][sampler]
                                                [blend_mode];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::renderPassMaskColorMatrixProgramAA::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler, blend_mode);
  }
  return program;
}

const GLRenderer::TileProgram* GLRenderer::GetTileProgram(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  TileProgram* program = &tile_program_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramOpaque* GLRenderer::GetTileProgramOpaque(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  TileProgramOpaque* program = &tile_program_opaque_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramOpaque::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramAA* GLRenderer::GetTileProgramAA(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  TileProgramAA* program = &tile_program_aa_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramAA::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramSwizzle* GLRenderer::GetTileProgramSwizzle(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  TileProgramSwizzle* program = &tile_program_swizzle_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramSwizzle::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramSwizzleOpaque*
GLRenderer::GetTileProgramSwizzleOpaque(TexCoordPrecision precision,
                                        SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  TileProgramSwizzleOpaque* program =
      &tile_program_swizzle_opaque_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramSwizzleOpaque::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TileProgramSwizzleAA* GLRenderer::GetTileProgramSwizzleAA(
    TexCoordPrecision precision,
    SamplerType sampler) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  DCHECK_GE(sampler, 0);
  DCHECK_LT(sampler, NumSamplerTypes);
  TileProgramSwizzleAA* program = &tile_program_swizzle_aa_[precision][sampler];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::tileProgramSwizzleAA::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, sampler);
  }
  return program;
}

const GLRenderer::TextureProgram* GLRenderer::GetTextureProgram(
    TexCoordPrecision precision) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  TextureProgram* program = &texture_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::textureProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, SamplerType2D);
  }
  return program;
}

const GLRenderer::NonPremultipliedTextureProgram*
GLRenderer::GetNonPremultipliedTextureProgram(TexCoordPrecision precision) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  NonPremultipliedTextureProgram* program =
      &nonpremultiplied_texture_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::NonPremultipliedTextureProgram::Initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, SamplerType2D);
  }
  return program;
}

const GLRenderer::TextureBackgroundProgram*
GLRenderer::GetTextureBackgroundProgram(TexCoordPrecision precision) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  TextureBackgroundProgram* program = &texture_background_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::textureProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, SamplerType2D);
  }
  return program;
}

const GLRenderer::NonPremultipliedTextureBackgroundProgram*
GLRenderer::GetNonPremultipliedTextureBackgroundProgram(
    TexCoordPrecision precision) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  NonPremultipliedTextureBackgroundProgram* program =
      &nonpremultiplied_texture_background_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc",
                 "GLRenderer::NonPremultipliedTextureProgram::Initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, SamplerType2D);
  }
  return program;
}

const GLRenderer::TextureProgram* GLRenderer::GetTextureIOSurfaceProgram(
    TexCoordPrecision precision) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  TextureProgram* program = &texture_io_surface_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::textureIOSurfaceProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, SamplerType2DRect);
  }
  return program;
}

const GLRenderer::VideoYUVProgram* GLRenderer::GetVideoYUVProgram(
    TexCoordPrecision precision) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  VideoYUVProgram* program = &video_yuv_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::videoYUVProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, SamplerType2D);
  }
  return program;
}

const GLRenderer::VideoYUVAProgram* GLRenderer::GetVideoYUVAProgram(
    TexCoordPrecision precision) {
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  VideoYUVAProgram* program = &video_yuva_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::videoYUVAProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, SamplerType2D);
  }
  return program;
}

const GLRenderer::VideoStreamTextureProgram*
GLRenderer::GetVideoStreamTextureProgram(TexCoordPrecision precision) {
  if (!Capabilities().using_egl_image)
    return NULL;
  DCHECK_GE(precision, 0);
  DCHECK_LT(precision, NumTexCoordPrecisions);
  VideoStreamTextureProgram* program =
      &video_stream_texture_program_[precision];
  if (!program->initialized()) {
    TRACE_EVENT0("cc", "GLRenderer::streamTextureProgram::initialize");
    program->Initialize(
        output_surface_->context_provider(), precision, SamplerTypeExternalOES);
  }
  return program;
}

void GLRenderer::CleanupSharedObjects() {
  shared_geometry_ = nullptr;

  for (int i = 0; i < NumTexCoordPrecisions; ++i) {
    for (int j = 0; j < NumSamplerTypes; ++j) {
      tile_program_[i][j].Cleanup(gl_);
      tile_program_opaque_[i][j].Cleanup(gl_);
      tile_program_swizzle_[i][j].Cleanup(gl_);
      tile_program_swizzle_opaque_[i][j].Cleanup(gl_);
      tile_program_aa_[i][j].Cleanup(gl_);
      tile_program_swizzle_aa_[i][j].Cleanup(gl_);

      for (int k = 0; k < NumBlendModes; k++) {
        render_pass_mask_program_[i][j][k].Cleanup(gl_);
        render_pass_mask_program_aa_[i][j][k].Cleanup(gl_);
        render_pass_mask_color_matrix_program_aa_[i][j][k].Cleanup(gl_);
        render_pass_mask_color_matrix_program_[i][j][k].Cleanup(gl_);
      }
    }
    for (int j = 0; j < NumBlendModes; j++) {
      render_pass_program_[i][j].Cleanup(gl_);
      render_pass_program_aa_[i][j].Cleanup(gl_);
      render_pass_color_matrix_program_[i][j].Cleanup(gl_);
      render_pass_color_matrix_program_aa_[i][j].Cleanup(gl_);
    }

    texture_program_[i].Cleanup(gl_);
    nonpremultiplied_texture_program_[i].Cleanup(gl_);
    texture_background_program_[i].Cleanup(gl_);
    nonpremultiplied_texture_background_program_[i].Cleanup(gl_);
    texture_io_surface_program_[i].Cleanup(gl_);

    video_yuv_program_[i].Cleanup(gl_);
    video_yuva_program_[i].Cleanup(gl_);
    video_stream_texture_program_[i].Cleanup(gl_);
  }

  tile_checkerboard_program_.Cleanup(gl_);

  debug_border_program_.Cleanup(gl_);
  solid_color_program_.Cleanup(gl_);
  solid_color_program_aa_.Cleanup(gl_);

  if (offscreen_framebuffer_id_)
    GLC(gl_, gl_->DeleteFramebuffers(1, &offscreen_framebuffer_id_));

  if (on_demand_tile_raster_resource_id_)
    resource_provider_->DeleteResource(on_demand_tile_raster_resource_id_);

  ReleaseRenderPassTextures();
}

void GLRenderer::ReinitializeGLState() {
  is_scissor_enabled_ = false;
  scissor_rect_needs_reset_ = true;
  stencil_shadow_ = false;
  blend_shadow_ = true;
  program_shadow_ = 0;

  RestoreGLState();
}

void GLRenderer::RestoreGLState() {
  // This restores the current GLRenderer state to the GL context.

  shared_geometry_->PrepareForDraw();

  GLC(gl_, gl_->Disable(GL_DEPTH_TEST));
  GLC(gl_, gl_->Disable(GL_CULL_FACE));
  GLC(gl_, gl_->ColorMask(true, true, true, true));
  GLC(gl_, gl_->BlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
  GLC(gl_, gl_->ActiveTexture(GL_TEXTURE0));

  if (program_shadow_)
    gl_->UseProgram(program_shadow_);

  if (stencil_shadow_)
    GLC(gl_, gl_->Enable(GL_STENCIL_TEST));
  else
    GLC(gl_, gl_->Disable(GL_STENCIL_TEST));

  if (blend_shadow_)
    GLC(gl_, gl_->Enable(GL_BLEND));
  else
    GLC(gl_, gl_->Disable(GL_BLEND));

  if (is_scissor_enabled_) {
    GLC(gl_, gl_->Enable(GL_SCISSOR_TEST));
    GLC(gl_,
        gl_->Scissor(scissor_rect_.x(),
                     scissor_rect_.y(),
                     scissor_rect_.width(),
                     scissor_rect_.height()));
  } else {
    GLC(gl_, gl_->Disable(GL_SCISSOR_TEST));
  }
}

void GLRenderer::RestoreFramebuffer(DrawingFrame* frame) {
  UseRenderPass(frame, frame->current_render_pass);
}

bool GLRenderer::IsContextLost() {
  return output_surface_->context_provider()->IsContextLost();
}

void GLRenderer::ScheduleOverlays(DrawingFrame* frame) {
  if (!frame->overlay_list.size())
    return;

  ResourceProvider::ResourceIdArray resources;
  OverlayCandidateList& overlays = frame->overlay_list;
  OverlayCandidateList::iterator it;
  for (it = overlays.begin(); it != overlays.end(); ++it) {
    const OverlayCandidate& overlay = *it;
    // Skip primary plane.
    if (overlay.plane_z_order == 0)
      continue;

    pending_overlay_resources_.push_back(
        make_scoped_ptr(new ResourceProvider::ScopedReadLockGL(
            resource_provider_, overlay.resource_id)));

    context_support_->ScheduleOverlayPlane(
        overlay.plane_z_order,
        overlay.transform,
        pending_overlay_resources_.back()->texture_id(),
        overlay.display_rect,
        overlay.uv_rect);
  }
}

}  // namespace cc
