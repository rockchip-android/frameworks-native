/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#undef LOG_TAG
#define LOG_TAG "Layer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

// #define ENABLE_DEBUG_LOG
#include <log/custom_log.h>


#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>
#include <math.h>

#include <cutils/compiler.h>
#include <cutils/native_handle.h>
#include <cutils/properties.h>

#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/NativeHandle.h>
#include <utils/StopWatch.h>
#include <utils/Trace.h>

#include <ui/GraphicBuffer.h>
#include <ui/PixelFormat.h>

#include <gui/BufferItem.h>
#include <gui/Surface.h>

#include "clz.h"
#include "Colorizer.h"
#include "DisplayDevice.h"
#include "Layer.h"
#include "MonitoredProducer.h"
#include "SurfaceFlinger.h"

#include "DisplayHardware/HWComposer.h"

#include "RenderEngine/RenderEngine.h"

#include <mutex>
#if RK_NV12_10_TO_NV12_BY_RGA
#define UN_NEED_GL
#include <RockchipRga.h>
#endif
#if (RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
#include <dlfcn.h>
#endif
#define DEBUG_RESIZE    0

namespace android {
#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
typedef struct
{
     sp<GraphicBuffer> yuvTexBuffer;
     EGLImageKHR img;
} TexBufferImag;

#define TexBufferMax  2
#define TexKey 0x524f434b
static TexBufferImag yuvTeximg[TexBufferMax] = {{NULL,EGL_NO_IMAGE_KHR},{NULL,EGL_NO_IMAGE_KHR}};
#endif

// ---------------------------------------------------------------------------

int32_t Layer::sSequence = 1;

Layer::Layer(SurfaceFlinger* flinger, const sp<Client>& client,
        const String8& name, uint32_t w, uint32_t h, uint32_t flags)
    :   contentDirty(false),
        sequence(uint32_t(android_atomic_inc(&sSequence))),
        mFlinger(flinger),
        mTextureName(-1U),
        mPremultipliedAlpha(true),
        mName("unnamed"),
        mFormat(PIXEL_FORMAT_NONE),
        mTransactionFlags(0),
        mPendingStateMutex(),
        mPendingStates(),
        mQueuedFrames(0),
        mSidebandStreamChanged(false),
        mCurrentTransform(0),
        mCurrentScalingMode(NATIVE_WINDOW_SCALING_MODE_FREEZE),
        mOverrideScalingMode(-1),
        mCurrentOpacity(true),
        mCurrentFrameNumber(0),
        mRefreshPending(false),
        mFrameLatencyNeeded(false),
        mFiltering(false),
        mNeedsFiltering(false),
        mMesh(Mesh::TRIANGLE_FAN, 4, 2, 2),
#ifndef USE_HWC2
        mIsGlesComposition(false),
#endif
        mProtectedByApp(false),
        mHasSurface(false),
        mClientRef(client),
        mPotentialCursor(false),
        mQueueItemLock(),
        mQueueItemCondition(),
        mQueueItems(),
        mLastFrameNumberReceived(0),
        mUpdateTexImageFailed(false),
        mDrawingScreenshot(false),
        mAutoRefresh(false),
        mFreezePositionUpdates(false)
{
#ifdef USE_HWC2
    ALOGV("Creating Layer %s", name.string());
#endif

    mCurrentCrop.makeInvalid();
    mFlinger->getRenderEngine().genTextures(1, &mTextureName);
    mTexture.init(Texture::TEXTURE_EXTERNAL, mTextureName);

    uint32_t layerFlags = 0;
    if (flags & ISurfaceComposerClient::eHidden)
        layerFlags |= layer_state_t::eLayerHidden;
    if (flags & ISurfaceComposerClient::eOpaque)
        layerFlags |= layer_state_t::eLayerOpaque;
    if (flags & ISurfaceComposerClient::eSecure)
        layerFlags |= layer_state_t::eLayerSecure;

    if (flags & ISurfaceComposerClient::eNonPremultiplied)
        mPremultipliedAlpha = false;

    mName = name;

    mCurrentState.active.w = w;
    mCurrentState.active.h = h;
    mCurrentState.active.transform.set(0, 0);
    mCurrentState.crop.makeInvalid();
    mCurrentState.finalCrop.makeInvalid();
    mCurrentState.z = 0;
#ifdef USE_HWC2
    mCurrentState.alpha = 1.0f;
#else
    mCurrentState.alpha = 0xFF;
#endif
    mCurrentState.layerStack = 0;
    mCurrentState.flags = layerFlags;
    mCurrentState.sequence = 0;
    mCurrentState.requested = mCurrentState.active;

    // drawing state & current state are identical
    mDrawingState = mCurrentState;

#ifdef USE_HWC2
    const auto& hwc = flinger->getHwComposer();
    const auto& activeConfig = hwc.getActiveConfig(HWC_DISPLAY_PRIMARY);
    nsecs_t displayPeriod = activeConfig->getVsyncPeriod();
#else
    nsecs_t displayPeriod =
            flinger->getHwComposer().getRefreshPeriod(HWC_DISPLAY_PRIMARY);
#endif
    mFrameTracker.setDisplayRefreshPeriod(displayPeriod);
}

void Layer::onFirstRef() {
    // Creates a custom BufferQueue for SurfaceFlingerConsumer to use
    sp<IGraphicBufferProducer> producer;
    sp<IGraphicBufferConsumer> consumer;
    BufferQueue::createBufferQueue(&producer, &consumer);
    mProducer = new MonitoredProducer(producer, mFlinger);
    mSurfaceFlingerConsumer = new SurfaceFlingerConsumer(consumer, mTextureName,
            this);
    mSurfaceFlingerConsumer->setConsumerUsageBits(getEffectiveUsage(0));
    mSurfaceFlingerConsumer->setContentsChangedListener(this);
    mSurfaceFlingerConsumer->setName(mName);

#ifdef TARGET_DISABLE_TRIPLE_BUFFERING
#warning "disabling triple buffering"
#else
#if RK_USE_3_LAYER_BUFFER
    mProducer->setMaxDequeuedBufferCount(3);
#else
    mProducer->setMaxDequeuedBufferCount(2);
#endif
#endif

    const sp<const DisplayDevice> hw(mFlinger->getDefaultDisplayDevice());
    updateTransformHint(hw);
}

Layer::~Layer() {
  sp<Client> c(mClientRef.promote());
    if (c != 0) {
        c->detachLayer(this);
    }

    for (auto& point : mRemoteSyncPoints) {
        point->setTransactionApplied();
    }
    for (auto& point : mLocalSyncPoints) {
        point->setFrameAvailable();
    }
    mFlinger->deleteTextureAsync(mTextureName);
    mFrameTracker.logAndResetStats(mName);
}

// ---------------------------------------------------------------------------
// callbacks
// ---------------------------------------------------------------------------

#ifdef USE_HWC2
void Layer::onLayerDisplayed(const sp<Fence>& releaseFence) {
    if (mHwcLayers.empty()) {
        return;
    }
    mSurfaceFlingerConsumer->setReleaseFence(releaseFence);
}
#else
void Layer::onLayerDisplayed(const sp<const DisplayDevice>& /* hw */,
        HWComposer::HWCLayerInterface* layer) {
    if (layer) {
        layer->onDisplayed();
        mSurfaceFlingerConsumer->setReleaseFence(layer->getAndResetReleaseFence());
    }
}
#endif

void Layer::onFrameAvailable(const BufferItem& item) {
    // Add this buffer from our internal queue tracker
    { // Autolock scope
        Mutex::Autolock lock(mQueueItemLock);

        // Reset the frame number tracker when we receive the first buffer after
        // a frame number reset
        if (item.mFrameNumber == 1) {
            mLastFrameNumberReceived = 0;
        }

        // Ensure that callbacks are handled in order
        while (item.mFrameNumber != mLastFrameNumberReceived + 1) {
            status_t result = mQueueItemCondition.waitRelative(mQueueItemLock,
                    ms2ns(500));
            if (result != NO_ERROR) {
                ALOGE("[%s] Timed out waiting on callback", mName.string());
            }
        }

        mQueueItems.push_back(item);
        android_atomic_inc(&mQueuedFrames);

        // Wake up any pending callbacks
        mLastFrameNumberReceived = item.mFrameNumber;
        mQueueItemCondition.broadcast();
    }

    mFlinger->signalLayerUpdate();
}

void Layer::onFrameReplaced(const BufferItem& item) {
    { // Autolock scope
        Mutex::Autolock lock(mQueueItemLock);

        // Ensure that callbacks are handled in order
        while (item.mFrameNumber != mLastFrameNumberReceived + 1) {
            status_t result = mQueueItemCondition.waitRelative(mQueueItemLock,
                    ms2ns(500));
            if (result != NO_ERROR) {
                ALOGE("[%s] Timed out waiting on callback", mName.string());
            }
        }

        if (mQueueItems.empty()) {
            ALOGE("Can't replace a frame on an empty queue");
            return;
        }
        mQueueItems.editItemAt(mQueueItems.size() - 1) = item;

        // Wake up any pending callbacks
        mLastFrameNumberReceived = item.mFrameNumber;
        mQueueItemCondition.broadcast();
    }
}

void Layer::onSidebandStreamChanged() {
    if (android_atomic_release_cas(false, true, &mSidebandStreamChanged) == 0) {
        // mSidebandStreamChanged was false
        mFlinger->signalLayerUpdate();
    }
}

// called with SurfaceFlinger::mStateLock from the drawing thread after
// the layer has been remove from the current state list (and just before
// it's removed from the drawing state list)
void Layer::onRemoved() {
    mSurfaceFlingerConsumer->abandon();
}

// ---------------------------------------------------------------------------
// set-up
// ---------------------------------------------------------------------------

const String8& Layer::getName() const {
    return mName;
}

status_t Layer::setBuffers( uint32_t w, uint32_t h,
                            PixelFormat format, uint32_t flags)
{
    uint32_t const maxSurfaceDims = min(
            mFlinger->getMaxTextureSize(), mFlinger->getMaxViewportDims());

    // never allow a surface larger than what our underlying GL implementation
    // can handle.
    if ((uint32_t(w)>maxSurfaceDims) || (uint32_t(h)>maxSurfaceDims)) {
        ALOGE("dimensions too large %u x %u", uint32_t(w), uint32_t(h));
        return BAD_VALUE;
    }

    mFormat = format;

    mPotentialCursor = (flags & ISurfaceComposerClient::eCursorWindow) ? true : false;
    mProtectedByApp = (flags & ISurfaceComposerClient::eProtectedByApp) ? true : false;
    mCurrentOpacity = getOpacityForFormat(format);

    mSurfaceFlingerConsumer->setDefaultBufferSize(w, h);
    mSurfaceFlingerConsumer->setDefaultBufferFormat(format);
    mSurfaceFlingerConsumer->setConsumerUsageBits(getEffectiveUsage(0));

    return NO_ERROR;
}

/*
 * The layer handle is just a BBinder object passed to the client
 * (remote process) -- we don't keep any reference on our side such that
 * the dtor is called when the remote side let go of its reference.
 *
 * LayerCleaner ensures that mFlinger->onLayerDestroyed() is called for
 * this layer when the handle is destroyed.
 */
class Layer::Handle : public BBinder, public LayerCleaner {
    public:
        Handle(const sp<SurfaceFlinger>& flinger, const sp<Layer>& layer)
            : LayerCleaner(flinger, layer), owner(layer) {}

        wp<Layer> owner;
};

sp<IBinder> Layer::getHandle() {
    Mutex::Autolock _l(mLock);

    LOG_ALWAYS_FATAL_IF(mHasSurface,
            "Layer::getHandle() has already been called");

    mHasSurface = true;

    return new Handle(mFlinger, this);
}

sp<IGraphicBufferProducer> Layer::getProducer() const {
    return mProducer;
}

// ---------------------------------------------------------------------------
// h/w composer set-up
// ---------------------------------------------------------------------------

Rect Layer::getContentCrop() const {
    // this is the crop rectangle that applies to the buffer
    // itself (as opposed to the window)
    Rect crop;
    if (!mCurrentCrop.isEmpty()) {
        // if the buffer crop is defined, we use that
        crop = mCurrentCrop;
    } else if (mActiveBuffer != NULL) {
        // otherwise we use the whole buffer
        crop = mActiveBuffer->getBounds();
    } else {
        // if we don't have a buffer yet, we use an empty/invalid crop
        crop.makeInvalid();
    }
    return crop;
}

static Rect reduce(const Rect& win, const Region& exclude) {
    if (CC_LIKELY(exclude.isEmpty())) {
        return win;
    }
    if (exclude.isRect()) {
        return win.reduce(exclude.getBounds());
    }
    return Region(win).subtract(exclude).getBounds();
}

Rect Layer::computeBounds() const {
    const Layer::State& s(getDrawingState());
    return computeBounds(s.activeTransparentRegion);
}

Rect Layer::computeBounds(const Region& activeTransparentRegion) const {
    const Layer::State& s(getDrawingState());
    Rect win(s.active.w, s.active.h);

    if (!s.crop.isEmpty()) {
        win.intersect(s.crop, &win);
    }
    // subtract the transparent region and snap to the bounds
    return reduce(win, activeTransparentRegion);
}

FloatRect Layer::computeCrop(const sp<const DisplayDevice>& hw) const {
    // the content crop is the area of the content that gets scaled to the
    // layer's size.
    FloatRect crop(getContentCrop());

    // the crop is the area of the window that gets cropped, but not
    // scaled in any ways.
    const State& s(getDrawingState());

    // apply the projection's clipping to the window crop in
    // layerstack space, and convert-back to layer space.
    // if there are no window scaling involved, this operation will map to full
    // pixels in the buffer.
    // FIXME: the 3 lines below can produce slightly incorrect clipping when we have
    // a viewport clipping and a window transform. we should use floating point to fix this.

    Rect activeCrop(s.active.w, s.active.h);
    if (!s.crop.isEmpty()) {
        activeCrop = s.crop;
    }

    activeCrop = s.active.transform.transform(activeCrop);
    if (!activeCrop.intersect(hw->getViewport(), &activeCrop)) {
        activeCrop.clear();
    }
    if (!s.finalCrop.isEmpty()) {
        if(!activeCrop.intersect(s.finalCrop, &activeCrop)) {
            activeCrop.clear();
        }
    }
    activeCrop = s.active.transform.inverse().transform(activeCrop);

    // This needs to be here as transform.transform(Rect) computes the
    // transformed rect and then takes the bounding box of the result before
    // returning. This means
    // transform.inverse().transform(transform.transform(Rect)) != Rect
    // in which case we need to make sure the final rect is clipped to the
    // display bounds.
    if (!activeCrop.intersect(Rect(s.active.w, s.active.h), &activeCrop)) {
        activeCrop.clear();
    }

    // subtract the transparent region and snap to the bounds
    activeCrop = reduce(activeCrop, s.activeTransparentRegion);

    // Transform the window crop to match the buffer coordinate system,
    // which means using the inverse of the current transform set on the
    // SurfaceFlingerConsumer.
    uint32_t invTransform = mCurrentTransform;
    if (mSurfaceFlingerConsumer->getTransformToDisplayInverse()) {
        /*
         * the code below applies the primary display's inverse transform to the
         * buffer
         */
        uint32_t invTransformOrient =
                DisplayDevice::getPrimaryDisplayOrientationTransform();
        // calculate the inverse transform
        if (invTransformOrient & NATIVE_WINDOW_TRANSFORM_ROT_90) {
            invTransformOrient ^= NATIVE_WINDOW_TRANSFORM_FLIP_V |
                    NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }
        // and apply to the current transform
        invTransform = (Transform(invTransformOrient) * Transform(invTransform))
                .getOrientation();
    }

    int winWidth = s.active.w;
    int winHeight = s.active.h;
    if (invTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
        // If the activeCrop has been rotate the ends are rotated but not
        // the space itself so when transforming ends back we can't rely on
        // a modification of the axes of rotation. To account for this we
        // need to reorient the inverse rotation in terms of the current
        // axes of rotation.
        bool is_h_flipped = (invTransform & NATIVE_WINDOW_TRANSFORM_FLIP_H) != 0;
        bool is_v_flipped = (invTransform & NATIVE_WINDOW_TRANSFORM_FLIP_V) != 0;
        if (is_h_flipped == is_v_flipped) {
            invTransform ^= NATIVE_WINDOW_TRANSFORM_FLIP_V |
                    NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }
        winWidth = s.active.h;
        winHeight = s.active.w;
    }
    const Rect winCrop = activeCrop.transform(
            invTransform, s.active.w, s.active.h);

    // below, crop is intersected with winCrop expressed in crop's coordinate space
    float xScale = crop.getWidth()  / float(winWidth);
    float yScale = crop.getHeight() / float(winHeight);

    float insetL = winCrop.left                 * xScale;
    float insetT = winCrop.top                  * yScale;
    float insetR = (winWidth - winCrop.right )  * xScale;
    float insetB = (winHeight - winCrop.bottom) * yScale;

    crop.left   += insetL;
    crop.top    += insetT;
    crop.right  -= insetR;
    crop.bottom -= insetB;

    return crop;
}

#if RK_OPT_DVFS
/**
 * 标识当前是否使用 DVFS 机制优化瞬时的 GPU 性能.
 */
static int dvfs_stat = 0;

static int openDvfsNode()
{
    int fd_dvfs = -1;

#ifdef SF_RK3288
        fd_dvfs = open("/sys/devices/ffa30000.gpu/dvfs", O_RDWR, 0);
#elif SF_RK3399
        fd_dvfs = open("/sys/devices/platform/ff9a0000.gpu/devfreq/ff9a0000.gpu/governor", O_RDWR, 0);
#else
        fd_dvfs = -1;
#endif

    return fd_dvfs;
}

#if SF_RK3399
static bool shouldEnforceGpuHighPerformance()
{
    bool ret = false;

    ssize_t numOfBytesRead = 0;
    char bytesRead[64] = "\0";
    char* governor;

    int fd = -1; // fd_of_dvfs_node.

    fd = openDvfsNode();
    if ( fd < 0 )
    {
        ret = false;
        goto EXIT;
    }

    numOfBytesRead = read(fd, bytesRead, sizeof(bytesRead) );
    if ( numOfBytesRead <= 0 )
    {
        E("fail to read governor of devfreq_gpu, numOfBytesRead : %zd, err : %s.", numOfBytesRead, strerror(errno) );
        goto EXIT;
    }
    D("numOfBytesRead : %zd", numOfBytesRead);
    D_MEM(bytesRead, numOfBytesRead); // 'bytesRead' 中的有效内容以 '\n' 结尾, 而不是 '\0'.

    bytesRead[numOfBytesRead - 1] = '\0';   // "- 2" : 将 "xxx'\n'" 中的 '\n' 置为 '\0'.
    governor = bytesRead;
    D_STR(governor);

    /* 只有当前 governor 是 'simple_ondemand' 才 应该 提高 clk_gpu. */
    if (0 == strcmp(governor, "simple_ondemand") )
    {
        ret = true;
    }
    else
    {
        W("should not enforce gpu high performance, for curr governor : %s", governor);
        ret = false;
    }
    // .trick : 3399 系统通常使用 governor "simple_ondemand", 这里为优化性能, 将临时设置为 "performance".
    //          其他以调试为目的的定频操作, 最好 "不要" 使用 "performance", 而使用 "userspace".

EXIT:
    if ( fd != -1 )
    {
        close(fd);
    }
    return ret;
}
#endif // #if SF_RK3399

static void optimizationDvfs(int on) {
    char value[30];
    int ret = -1;

    int fd = -1; // fd_of_dvfs_node.

    fd = openDvfsNode();
    if (fd< 0) {
        ALOGV("on=%d,fd_dvfs=%d,%s", on, fd, strerror(errno));
        return;
    }

#ifdef SF_RK3288
    if (on) {
        sprintf(value, "on");
        ret = write(fd, value, sizeof(value));
    } else {
        sprintf(value, "off");
        ret = write(fd, value, sizeof(value));
    }
#elif SF_RK3399
    if (on) {
        sprintf(value, "performance");
        ret = write(fd, value, sizeof(value));
    } else {
        sprintf(value, "simple_ondemand");
        ret = write(fd, value, sizeof(value));
    }
#else
    sprintf(value, "nothing");
#endif

    if (ret == -1)
        ALOGV("ret=%d,on=%d,fd_dvfs=%d,%s", ret, on, fd, strerror(errno));

    if ( fd != -1 )
    {
        close(fd);
    }

    return;
}
#endif // #if RK_OPT_DVFS

#ifdef USE_HWC2
void Layer::setGeometry(const sp<const DisplayDevice>& displayDevice)
#else
void Layer::setGeometry(
    const sp<const DisplayDevice>& hw,
        HWComposer::HWCLayerInterface& layer)
#endif
{
#ifdef USE_HWC2
    const auto hwcId = displayDevice->getHwcDisplayId();
    auto& hwcInfo = mHwcLayers[hwcId];
#else
    layer.setDefaultState();
#endif

    // enable this layer
#ifdef USE_HWC2
    hwcInfo.forceClientComposition = false;

    if (isSecure() && !displayDevice->isSecure()) {
        hwcInfo.forceClientComposition = true;
    }

    auto& hwcLayer = hwcInfo.layer;
#else
    layer.setSkip(false);

    if (isSecure() && !hw->isSecure()) {
        layer.setSkip(true);
    }
#endif

    // this gives us only the "orientation" component of the transform
    const State& s(getDrawingState());
#ifdef USE_HWC2
    if (!isOpaque(s) || s.alpha != 1.0f) {
        auto blendMode = mPremultipliedAlpha ?
                HWC2::BlendMode::Premultiplied : HWC2::BlendMode::Coverage;
        auto error = hwcLayer->setBlendMode(blendMode);
        ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set blend mode %s:"
                " %s (%d)", mName.string(), to_string(blendMode).c_str(),
                to_string(error).c_str(), static_cast<int32_t>(error));
    }
#else
    if (!isOpaque(s) || s.alpha != 0xFF) {
#if !RK_USE_DRM
        layer.setBlending((mPremultipliedAlpha ?
                HWC_BLENDING_PREMULT :
                HWC_BLENDING_COVERAGE) | s.alpha<<16);
#else
        layer.setBlending(mPremultipliedAlpha ?
                HWC_BLENDING_PREMULT :
                HWC_BLENDING_COVERAGE);
#endif
    }
#endif

    // apply the layer's transform, followed by the display's global transform
    // here we're guaranteed that the layer's transform preserves rects
    Region activeTransparentRegion(s.activeTransparentRegion);
    if (!s.crop.isEmpty()) {
        Rect activeCrop(s.crop);
        activeCrop = s.active.transform.transform(activeCrop);
#ifdef USE_HWC2
        if(!activeCrop.intersect(displayDevice->getViewport(), &activeCrop)) {
#else
        if(!activeCrop.intersect(hw->getViewport(), &activeCrop)) {
#endif
            activeCrop.clear();
        }
        activeCrop = s.active.transform.inverse().transform(activeCrop, true);
        // This needs to be here as transform.transform(Rect) computes the
        // transformed rect and then takes the bounding box of the result before
        // returning. This means
        // transform.inverse().transform(transform.transform(Rect)) != Rect
        // in which case we need to make sure the final rect is clipped to the
        // display bounds.
        if(!activeCrop.intersect(Rect(s.active.w, s.active.h), &activeCrop)) {
            activeCrop.clear();
        }
        // mark regions outside the crop as transparent
        activeTransparentRegion.orSelf(Rect(0, 0, s.active.w, activeCrop.top));
        activeTransparentRegion.orSelf(Rect(0, activeCrop.bottom,
                s.active.w, s.active.h));
        activeTransparentRegion.orSelf(Rect(0, activeCrop.top,
                activeCrop.left, activeCrop.bottom));
        activeTransparentRegion.orSelf(Rect(activeCrop.right, activeCrop.top,
                s.active.w, activeCrop.bottom));
    }
    Rect frame(s.active.transform.transform(computeBounds(activeTransparentRegion)));
    if (!s.finalCrop.isEmpty()) {
        if(!frame.intersect(s.finalCrop, &frame)) {
            frame.clear();
        }
    }
#ifdef USE_HWC2
    if (!frame.intersect(displayDevice->getViewport(), &frame)) {
        frame.clear();
    }
    const Transform& tr(displayDevice->getTransform());
    Rect transformedFrame = tr.transform(frame);
    auto error = hwcLayer->setDisplayFrame(transformedFrame);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set display frame [%d, %d, %d, %d]: %s (%d)",
                mName.string(), transformedFrame.left, transformedFrame.top,
                transformedFrame.right, transformedFrame.bottom,
                to_string(error).c_str(), static_cast<int32_t>(error));
    } else {
        hwcInfo.displayFrame = transformedFrame;
    }

    FloatRect sourceCrop = computeCrop(displayDevice);
    error = hwcLayer->setSourceCrop(sourceCrop);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set source crop [%.3f, %.3f, %.3f, %.3f]: "
                "%s (%d)", mName.string(), sourceCrop.left, sourceCrop.top,
                sourceCrop.right, sourceCrop.bottom, to_string(error).c_str(),
                static_cast<int32_t>(error));
    } else {
        hwcInfo.sourceCrop = sourceCrop;
    }

    error = hwcLayer->setPlaneAlpha(s.alpha);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set plane alpha %.3f: "
            "%s (%d)", mName.string(), s.alpha, to_string(error).c_str(),
            static_cast<int32_t>(error));

    error = hwcLayer->setZOrder(s.z);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set Z %u: %s (%d)",
            mName.string(), s.z, to_string(error).c_str(),
            static_cast<int32_t>(error));
#else
    if (!frame.intersect(hw->getViewport(), &frame)) {
        frame.clear();
    }
    const Transform& tr(hw->getTransform());
    layer.setFrame(tr.transform(frame));
    layer.setCrop(computeCrop(hw));
    layer.setPlaneAlpha(s.alpha);
#endif

    /*
     * Transformations are applied in this order:
     * 1) buffer orientation/flip/mirror
     * 2) state transformation (window manager)
     * 3) layer orientation (screen orientation)
     * (NOTE: the matrices are multiplied in reverse order)
     */

    const Transform bufferOrientation(mCurrentTransform);
    Transform transform(tr * s.active.transform * bufferOrientation);

    if (mSurfaceFlingerConsumer->getTransformToDisplayInverse()) {
        /*
         * the code below applies the primary display's inverse transform to the
         * buffer
         */
        uint32_t invTransform =
                DisplayDevice::getPrimaryDisplayOrientationTransform();
        // calculate the inverse transform
        if (invTransform & NATIVE_WINDOW_TRANSFORM_ROT_90) {
            invTransform ^= NATIVE_WINDOW_TRANSFORM_FLIP_V |
                    NATIVE_WINDOW_TRANSFORM_FLIP_H;
        }
        // and apply to the current transform
        transform = Transform(invTransform) * transform;
    }

    // this gives us only the "orientation" component of the transform
    const uint32_t orientation = transform.getOrientation();
#ifdef USE_HWC2
    if (orientation & Transform::ROT_INVALID) {
        // we can only handle simple transformation
        hwcInfo.forceClientComposition = true;
    } else {
        auto transform = static_cast<HWC2::Transform>(orientation);
        auto error = hwcLayer->setTransform(transform);
        ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set transform %s: "
                "%s (%d)", mName.string(), to_string(transform).c_str(),
                to_string(error).c_str(), static_cast<int32_t>(error));
    }
#else
    if (orientation & Transform::ROT_INVALID) {
#if RK_OPT_DVFS
        if (dvfs_stat == 0
#if SF_RK3399
            && shouldEnforceGpuHighPerformance()
#endif
        )
        {
            ALOGI("start forcing gpu high performance.");
            optimizationDvfs(1);
            dvfs_stat = 1;
        }
#endif
        // we can only handle simple transformation
        layer.setSkip(true);
    } else {
#if RK_OPT_DVFS
        if (dvfs_stat == 1) {
            ALOGI("stop forcing gpu high performance.");
            optimizationDvfs(0);
            dvfs_stat = 0;
        }
#endif
        layer.setTransform(orientation);
    }
#endif
}

#ifdef USE_HWC2
void Layer::forceClientComposition(int32_t hwcId) {
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("forceClientComposition: no HWC layer found (%d)", hwcId);
        return;
    }

    mHwcLayers[hwcId].forceClientComposition = true;
}
#endif

#ifdef USE_HWC2
void Layer::setPerFrameData(const sp<const DisplayDevice>& displayDevice) {
    // Apply this display's projection's viewport to the visible region
    // before giving it to the HWC HAL.
    const Transform& tr = displayDevice->getTransform();
    const auto& viewport = displayDevice->getViewport();
    Region visible = tr.transform(visibleRegion.intersect(viewport));
    auto hwcId = displayDevice->getHwcDisplayId();
    auto& hwcLayer = mHwcLayers[hwcId].layer;
    auto error = hwcLayer->setVisibleRegion(visible);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set visible region: %s (%d)", mName.string(),
                to_string(error).c_str(), static_cast<int32_t>(error));
        visible.dump(LOG_TAG);
    }

    error = hwcLayer->setSurfaceDamage(surfaceDamageRegion);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set surface damage: %s (%d)", mName.string(),
                to_string(error).c_str(), static_cast<int32_t>(error));
        surfaceDamageRegion.dump(LOG_TAG);
    }

    // Sideband layers
    if (mSidebandStream.get()) {
        setCompositionType(hwcId, HWC2::Composition::Sideband);
        ALOGV("[%s] Requesting Sideband composition", mName.string());
        error = hwcLayer->setSidebandStream(mSidebandStream->handle());
        if (error != HWC2::Error::None) {
            ALOGE("[%s] Failed to set sideband stream %p: %s (%d)",
                    mName.string(), mSidebandStream->handle(),
                    to_string(error).c_str(), static_cast<int32_t>(error));
        }
        return;
    }

    // Client layers
    if (mHwcLayers[hwcId].forceClientComposition ||
            (mActiveBuffer != nullptr && mActiveBuffer->handle == nullptr)) {
        ALOGV("[%s] Requesting Client composition", mName.string());
        setCompositionType(hwcId, HWC2::Composition::Client);
        return;
    }

    // SolidColor layers
    if (mActiveBuffer == nullptr) {
        setCompositionType(hwcId, HWC2::Composition::SolidColor);

        // For now, we only support black for DimLayer
        error = hwcLayer->setColor({0, 0, 0, 255});
        if (error != HWC2::Error::None) {
            ALOGE("[%s] Failed to set color: %s (%d)", mName.string(),
                    to_string(error).c_str(), static_cast<int32_t>(error));
        }

        // Clear out the transform, because it doesn't make sense absent a
        // source buffer
        error = hwcLayer->setTransform(HWC2::Transform::None);
        if (error != HWC2::Error::None) {
            ALOGE("[%s] Failed to clear transform: %s (%d)", mName.string(),
                    to_string(error).c_str(), static_cast<int32_t>(error));
        }

        return;
    }

    // Device or Cursor layers
    if (mPotentialCursor) {
        ALOGV("[%s] Requesting Cursor composition", mName.string());
        setCompositionType(hwcId, HWC2::Composition::Cursor);
    } else {
        ALOGV("[%s] Requesting Device composition", mName.string());
        setCompositionType(hwcId, HWC2::Composition::Device);
    }

    auto acquireFence = mSurfaceFlingerConsumer->getCurrentFence();
    error = hwcLayer->setBuffer(mActiveBuffer->handle, acquireFence);
    if (error != HWC2::Error::None) {
        ALOGE("[%s] Failed to set buffer %p: %s (%d)", mName.string(),
                mActiveBuffer->handle, to_string(error).c_str(),
                static_cast<int32_t>(error));
    }
}
#else
void Layer::setPerFrameData(const sp<const DisplayDevice>& hw,
        HWComposer::HWCLayerInterface& layer) {
    // we have to set the visible region on every frame because
    // we currently free it during onLayerDisplayed(), which is called
    // after HWComposer::commit() -- every frame.
    // Apply this display's projection's viewport to the visible region
    // before giving it to the HWC HAL.
    const Transform& tr = hw->getTransform();
    Region visible = tr.transform(visibleRegion.intersect(hw->getViewport()));
    layer.setVisibleRegionScreen(visible);
    layer.setSurfaceDamage(surfaceDamageRegion);
    mIsGlesComposition = (layer.getCompositionType() == HWC_FRAMEBUFFER);

    if (mSidebandStream.get()) {
        layer.setSidebandStream(mSidebandStream);
    } else {
        // NOTE: buffer can be NULL if the client never drew into this
        // layer yet, or if we ran out of memory
        layer.setBuffer(mActiveBuffer);
    }
#if RK_LAYER_NAME
    layer.setLayername(getName().string());
#endif
#if RK_STEREO && !RK_VR
    layer.setAlreadyStereo(mSurfaceFlingerConsumer->getAlreadyStereo());
    layer.initDisplayStereo();
#endif
}
#endif

#ifdef USE_HWC2
void Layer::updateCursorPosition(const sp<const DisplayDevice>& displayDevice) {
    auto hwcId = displayDevice->getHwcDisplayId();
    if (mHwcLayers.count(hwcId) == 0 ||
            getCompositionType(hwcId) != HWC2::Composition::Cursor) {
        return;
    }

    // This gives us only the "orientation" component of the transform
    const State& s(getCurrentState());

    // Apply the layer's transform, followed by the display's global transform
    // Here we're guaranteed that the layer's transform preserves rects
    Rect win(s.active.w, s.active.h);
    if (!s.crop.isEmpty()) {
        win.intersect(s.crop, &win);
    }
    // Subtract the transparent region and snap to the bounds
    Rect bounds = reduce(win, s.activeTransparentRegion);
    Rect frame(s.active.transform.transform(bounds));
    frame.intersect(displayDevice->getViewport(), &frame);
    if (!s.finalCrop.isEmpty()) {
        frame.intersect(s.finalCrop, &frame);
    }
    auto& displayTransform(displayDevice->getTransform());
    auto position = displayTransform.transform(frame);

    auto error = mHwcLayers[hwcId].layer->setCursorPosition(position.left,
            position.top);
    ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set cursor position "
            "to (%d, %d): %s (%d)", mName.string(), position.left,
            position.top, to_string(error).c_str(),
            static_cast<int32_t>(error));
}
#else
void Layer::setAcquireFence(const sp<const DisplayDevice>& /* hw */,
        HWComposer::HWCLayerInterface& layer) {
    int fenceFd = -1;

    // TODO: there is a possible optimization here: we only need to set the
    // acquire fence the first time a new buffer is acquired on EACH display.
#if RK_COMP_TYPE
    if (layer.getCompositionType() != HWC_FRAMEBUFFER) {
#else
    if (layer.getCompositionType() == HWC_OVERLAY || layer.getCompositionType() == HWC_CURSOR_OVERLAY) {
#endif
        sp<Fence> fence = mSurfaceFlingerConsumer->getCurrentFence();
        if (fence->isValid()) {
            fenceFd = fence->dup();
            if (fenceFd == -1) {
                ALOGW("failed to dup layer fence, skipping sync: %d", errno);
            }
        }
    }
    layer.setAcquireFenceFd(fenceFd);
}

#if RK_STEREO
void Layer::setDisplayStereo(const sp<const DisplayDevice>& /* hw */,
        HWComposer::HWCLayerInterface& layer) {
    displayStereo = layer.getDisplayStereo();
}
#endif

#if RK_VR
bool Layer::isFullScreen(const sp<const DisplayDevice>& hw,HWComposer::HWCLayerInterface& layer){
    Rect rect;
    layer.getFrame(rect);

    float w_ratio = (float)rect.getWidth() / (float)hw->getWidth();
    float h_ratio = (float)rect.getHeight() / (float)hw->getHeight();

    //ALOGD("ljt %f %f",w_ratio,h_ratio);


    if((1.0 == w_ratio) && (h_ratio > 0.8))
        return true;
    else
        return false;
}

bool Layer::isFBRLayer(){
    if (mActiveBuffer == NULL)
        return false;

    int64_t usage = mActiveBuffer->getUsage();

    if (usage & 0x08000000)
        return true;
    else
        return false;
}

void Layer::setAlreadyStereo(HWComposer::HWCLayerInterface& layer,int flag) {
    mStereoMode = flag;
    return layer.setAlreadyStereo(flag);
}

int Layer::getStereoModeToDraw()const{
    return mStereoMode;
}
#endif

Rect Layer::getPosition(
    const sp<const DisplayDevice>& hw)
{
    // this gives us only the "orientation" component of the transform
    const State& s(getCurrentState());

    // apply the layer's transform, followed by the display's global transform
    // here we're guaranteed that the layer's transform preserves rects
    Rect win(s.active.w, s.active.h);
    if (!s.crop.isEmpty()) {
        win.intersect(s.crop, &win);
    }
    // subtract the transparent region and snap to the bounds
    Rect bounds = reduce(win, s.activeTransparentRegion);
    Rect frame(s.active.transform.transform(bounds));
    frame.intersect(hw->getViewport(), &frame);
    if (!s.finalCrop.isEmpty()) {
        frame.intersect(s.finalCrop, &frame);
    }
    const Transform& tr(hw->getTransform());
    return Rect(tr.transform(frame));
}
#endif

// ---------------------------------------------------------------------------
// drawing...
// ---------------------------------------------------------------------------

void Layer::draw(const sp<const DisplayDevice>& hw, const Region& clip) const {
    onDraw(hw, clip, false);
}

void Layer::draw(const sp<const DisplayDevice>& hw,
        bool useIdentityTransform) const {
    onDraw(hw, Region(hw->bounds()), useIdentityTransform);
}

void Layer::draw(const sp<const DisplayDevice>& hw) const {
    onDraw(hw, Region(hw->bounds()), false);
}

#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
/* print time macros. */
#define PRINT_TIME_START        \
    struct timeval tpend1, tpend2;\
    long usec1 = 0;\
    gettimeofday(&tpend1,NULL);\

#define PRINT_TIME_END(tag)        \
    gettimeofday(&tpend2,NULL);\
    usec1 = 1000*(tpend2.tv_sec - tpend1.tv_sec) + (tpend2.tv_usec- tpend1.tv_usec)/1000;\
    if (property_get_bool("sys.hwc.time", 1)) \
    ALOGD_IF(1,"%s use time=%ld ms",tag,usec1);\

 #if RK_NV12_10_TO_NV12_BY_RGA
static int rgaCopyBit(sp<GraphicBuffer> src_buf, sp<GraphicBuffer> dst_buf, const Rect& rect)
{
    rga_info_t src, dst;
    int src_l,src_t,src_r,src_b,src_h,src_stride,src_format;
    int dst_l,dst_t,dst_r,dst_b,dst_h,dst_stride,dst_format;
    RockchipRga& mRga = RockchipRga::get();
    int ret = 0;

    memset(&src, 0, sizeof(rga_info_t));
    memset(&dst, 0, sizeof(rga_info_t));
    src.fd = -1;
    dst.fd = -1;

    src_stride = src_buf->getStride();
    src_format = src_buf->getPixelFormat();
    src_h = src_buf->getHeight();

    dst_stride = dst_buf->getStride();
    dst_format = dst_buf->getPixelFormat();
    dst_h = dst_buf->getHeight();

    dst_l = src_l = rect.left;
    dst_t = src_t = rect.top;
    dst_r = src_r = rect.right;
    dst_b = src_b = rect.bottom;
    rga_set_rect(&src.rect, src_l, src_t, src_r - src_l, src_b - src_t, src_stride, src_h, src_format);
    rga_set_rect(&dst.rect, dst_l, dst_t, dst_buf->getWidth(), dst_buf->getHeight(), dst_stride, dst_h, dst_format);

    src.hnd = src_buf->handle;
    dst.hnd = dst_buf->handle;
//  src.rotation = rga_transform;
//PRINT_TIME_START
    ret = mRga.RkRgaBlit(&src, &dst, NULL);
//PRINT_TIME_END("rgaCopyBit")
    if(ret) {
        ALOGD_IF(1,"rgaCopyBit  : src[x=%d,y=%d,w=%d,h=%d,ws=%d,hs=%d,format=0x%x],dst[x=%d,y=%d,w=%d,h=%d,ws=%d,hs=%d,format=0x%x]",
            src.rect.xoffset, src.rect.yoffset, src.rect.width, src.rect.height, src.rect.wstride, src.rect.hstride, src.rect.format,
            dst.rect.xoffset, dst.rect.yoffset, dst.rect.width, dst.rect.height, dst.rect.wstride, dst.rect.hstride, dst.rect.format);
        ALOGD_IF(1,"rgaCopyBit : src hnd=%p,dst hnd=%p, src_format=0x%x ==> dst_format=0x%x\n",
            (void*)src_buf->handle, (void*)(dst_buf->handle), src_format, dst_format);
        return ret;
    }

    return ret;
}
#endif

#endif

#if RK_HDR
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
#define ARM_P010            0x4000000
#define HDRUSAGE            0x3000000
#define RK_XXX_PATH         "/system/lib64/librockchipxxx.so"
typedef void (*__rockchipxxx)(u8 *src, u8 *dst, int w, int h, int srcStride, int dstStride, int area);

static void* dso = NULL;
static __rockchipxxx rockchipxxx = NULL;

#elif RK_NV12_10_TO_NV12_BY_NENO

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed char s8;
typedef signed short s16;
typedef signed int s32;
#define RK_XXX_PATH         "/system/lib/librockchipxxx.so"
typedef void (*__rockchipxxx3288)(u8 *src, u8 *dst, int w, int h, int srcStride, int dstStride, int area);

static void* dso = NULL;
static __rockchipxxx3288 rockchipxxx3288 = NULL;
#endif

void Layer::onDraw(const sp<const DisplayDevice>& hw, const Region& clip,
        bool useIdentityTransform) const
{
    ATRACE_CALL();
    RenderEngine& engine(mFlinger->getRenderEngine());

    if (CC_UNLIKELY(mActiveBuffer == 0)) {
        // the texture has not been created yet, this Layer has
        // in fact never been drawn into. This happens frequently with
        // SurfaceView because the WindowManager can't know when the client
        // has drawn the first time.

        // If there is nothing under us, we paint the screen in black, otherwise
        // we just skip this update.

        // figure out if there is something below us
        Region under;
        const SurfaceFlinger::LayerVector& drawingLayers(
                mFlinger->mDrawingState.layersSortedByZ);
        const size_t count = drawingLayers.size();
        for (size_t i=0 ; i<count ; ++i) {
            const sp<Layer>& layer(drawingLayers[i]);
            if (layer.get() == static_cast<Layer const*>(this))
                break;
            under.orSelf( hw->getTransform().transform(layer->visibleRegion) );
        }
        // if not everything below us is covered, we plug the holes!
        Region holes(clip.subtract(under));
        if (!holes.isEmpty()) {
            clearWithOpenGL(hw, holes, 0, 0, 0, 1);
        }
        return;
    }

    // Bind the current buffer to the GL texture, and wait for it to be
    // ready for us to draw into.
    status_t err = NO_ERROR;
#if (RK_NV12_10_TO_NV12_BY_RGA | RK_NV12_10_TO_NV12_BY_NENO | RK_HDR)
    if(mActiveBuffer != NULL &&
       mActiveBuffer->getPixelFormat() == HAL_PIXEL_FORMAT_YCrCb_NV12_10 )
    {
#if RK_HDR
        const int yuvTexUsage = GraphicBuffer::USAGE_HW_TEXTURE | ARM_P010;
        const int yuvTexFormat = HAL_PIXEL_FORMAT_YCrCb_NV12_10;
#elif (RK_NV12_10_TO_NV12_BY_NENO | RK_NV12_10_TO_NV12_BY_RGA)
        const int yuvTexUsage = GraphicBuffer::USAGE_HW_TEXTURE;
        const int yuvTexFormat = HAL_PIXEL_FORMAT_YCrCb_NV12;
#endif
        static int yuvcnt;
        int yuvIndex ;
        yuvcnt ++;
        yuvIndex = yuvcnt%2;
#if (RK_HDR | RK_NV12_10_TO_NV12_BY_NENO)
        int src_l,src_t,src_r,src_b,src_stride;
        void *src_vaddr;
        void *dst_vaddr;
        src_l = mCurrentCrop.left;
        src_t = mCurrentCrop.top;
        src_r = mCurrentCrop.right;
        src_b = mCurrentCrop.bottom;
        src_stride = mActiveBuffer->getStride();
        uint32_t w = src_r - src_l;
#elif RK_NV12_10_TO_NV12_BY_RGA
        //Since rga cann't support scalet to bigger than 4096 limit to 4096
        uint32_t w = (mCurrentCrop.getWidth() + 31) & (~31);
#endif
        if((yuvTeximg[yuvIndex].yuvTexBuffer != NULL) &&
                   (yuvTeximg[yuvIndex].yuvTexBuffer->getWidth() != w ||
                    yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() != mActiveBuffer->getHeight()))
        {
            yuvTeximg[yuvIndex].yuvTexBuffer = NULL;
            eglDestroyImageKHR(hw->getEglDisplay(), yuvTeximg[yuvIndex].img);
        }
        if(yuvTeximg[yuvIndex].yuvTexBuffer == NULL)
        {
            yuvTeximg[yuvIndex].yuvTexBuffer = new GraphicBuffer(w, mActiveBuffer->getHeight(),
                                             yuvTexFormat, yuvTexUsage);
            EGLClientBuffer clientBuffer = (EGLClientBuffer)yuvTeximg[yuvIndex].yuvTexBuffer->getNativeBuffer();
            yuvTeximg[yuvIndex].img = eglCreateImageKHR(hw->getEglDisplay(), EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                                clientBuffer, 0);
            if (yuvTeximg[yuvIndex].img == EGL_NO_IMAGE_KHR) {
                ALOGE("%s:line=%d,EGL_NO_IMAGE_KHR",__FUNCTION__,__LINE__);
                return ;
            }
        }

#if (RK_HDR | RK_NV12_10_TO_NV12_BY_NENO)
        mActiveBuffer->lock(GRALLOC_USAGE_SW_READ_OFTEN,&src_vaddr);
        yuvTeximg[yuvIndex].yuvTexBuffer->lock(GRALLOC_USAGE_SW_WRITE_OFTEN,&dst_vaddr);

        //PRINT_TIME_START
        if(dso == NULL)
            dso = dlopen(RK_XXX_PATH, RTLD_NOW | RTLD_LOCAL);

        if (dso == 0) {
            ALOGE("rk_debug can't not find /system/lib64/librockchipxxx.so ! error=%s \n",
                dlerror());
            return;
        }
#if RK_HDR
        if(rockchipxxx == NULL)
            rockchipxxx = (__rockchipxxx)dlsym(dso, "_Z11rockchipxxxPhS_iiiii");
        if(rockchipxxx == NULL)
        {
            ALOGE("rk_debug can't not find target function in /system/lib64/librockchipxxx.so ! \n");
            dlclose(dso);
            return;
        }
        rockchipxxx((u8*)src_vaddr, (u8*)dst_vaddr, src_r - src_l, src_b - src_t, src_stride, (src_r - src_l)*2, 0);
#elif RK_NV12_10_TO_NV12_BY_NENO
        if(rockchipxxx3288 == NULL)
            rockchipxxx3288 = (__rockchipxxx3288)dlsym(dso, "_Z15rockchipxxx3288PhS_iiiii");
        if(rockchipxxx3288 == NULL)
        {
            ALOGE("rk_debug can't not find target function in /system/lib64/librockchipxxx.so ! \n");
            dlclose(dso);
            return;
        }
        rockchipxxx3288((u8*)src_vaddr, (u8*)dst_vaddr, src_r - src_l, src_b - src_t, src_stride, (src_r - src_l), 0);
#endif
        //PRINT_TIME_END("convert10to16_highbit_arm64_neon")
        ALOGV("src_vaddr=%p,dst_vaddr=%p,crop_w=%d,crop_h=%d,stride=%f, src_stride=%d,raw_w=%d,raw_h=%d",
                src_vaddr, dst_vaddr, src_r - src_l,src_b - src_t,
                (src_r - src_l)*1.25+64,src_stride,mActiveBuffer->getWidth(),mActiveBuffer->getHeight());
    //dump data
    static int i =0;
    char pro_value[PROPERTY_VALUE_MAX];

    property_get("sys.dump_out_neon",pro_value,0);
    if(i<10 && !strcmp(pro_value,"true"))
    {
        char data_name[100];

        sprintf(data_name,"/data/dump/dmlayer%d_%d_%d.bin", i,
                yuvTeximg[yuvIndex].yuvTexBuffer->getWidth(),yuvTeximg[yuvIndex].yuvTexBuffer->getHeight());
#if RK_HDR
        int n = yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() * yuvTeximg[yuvIndex].yuvTexBuffer->getStride();
#else
        int n = yuvTeximg[yuvIndex].yuvTexBuffer->getHeight() * yuvTeximg[yuvIndex].yuvTexBuffer->getStride() * 1.5;
#endif
        ALOGD("dump %s size=%d", data_name, n );
        FILE *fp;
        if ((fp = fopen(data_name, "w+")) == NULL)
        {
            printf("can't open output.bin!!!!!\n");
        }
        fwrite(dst_vaddr, n, 1, fp);
        fclose(fp);
        i++;
    }
#elif RK_NV12_10_TO_NV12_BY_RGA
        rgaCopyBit(mActiveBuffer, yuvTeximg[yuvIndex].yuvTexBuffer, mCurrentCrop);
#endif
        if (yuvTeximg[yuvIndex].img == EGL_NO_IMAGE_KHR) {
            ALOGE("%s:line=%d,EGL_NO_IMAGE_KHR",__FUNCTION__,__LINE__);
            return ;
        }

        engine.bindyuvimg(yuvTeximg[yuvIndex].img, mTextureName);
        ALOGV("glEGLImageTargetTexture2DOES,index=%d,img=%p,name=%d",yuvIndex,yuvTeximg[yuvIndex].img,mTextureName);
    }
    else
#endif
    {
        err = mSurfaceFlingerConsumer->bindTextureImage();
    }
    if (err != NO_ERROR) {
        ALOGW("onDraw: bindTextureImage failed (err=%d)", err);
        // Go ahead and draw the buffer anyway; no matter what we do the screen
        // is probably going to have something visibly wrong.
    }

    bool blackOutLayer = isProtected() || (isSecure() && !hw->isSecure());
#if RK_BLACK_NV12_10_LAYER
    blackOutLayer = blackOutLayer || (mActiveBuffer->getPixelFormat()== HAL_PIXEL_FORMAT_YCrCb_NV12_10);
#endif

    if (!blackOutLayer) {
        // TODO: we could be more subtle with isFixedSize()
        const bool useFiltering = getFiltering() || needsFiltering(hw) || isFixedSize();

        // Query the texture matrix given our current filtering mode.
        float textureMatrix[16];
        mSurfaceFlingerConsumer->setFilteringEnabled(useFiltering);
        mSurfaceFlingerConsumer->getTransformMatrix(textureMatrix);

        if (mSurfaceFlingerConsumer->getTransformToDisplayInverse()) {

            /*
             * the code below applies the primary display's inverse transform to
             * the texture transform
             */

            // create a 4x4 transform matrix from the display transform flags
            const mat4 flipH(-1,0,0,0,  0,1,0,0, 0,0,1,0, 1,0,0,1);
            const mat4 flipV( 1,0,0,0, 0,-1,0,0, 0,0,1,0, 0,1,0,1);
            const mat4 rot90( 0,1,0,0, -1,0,0,0, 0,0,1,0, 1,0,0,1);

            mat4 tr;
            uint32_t transform =
                    DisplayDevice::getPrimaryDisplayOrientationTransform();
            if (transform & NATIVE_WINDOW_TRANSFORM_ROT_90)
                tr = tr * rot90;
            if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_H)
                tr = tr * flipH;
            if (transform & NATIVE_WINDOW_TRANSFORM_FLIP_V)
                tr = tr * flipV;

            // calculate the inverse
            tr = inverse(tr);

            // and finally apply it to the original texture matrix
            const mat4 texTransform(mat4(static_cast<const float*>(textureMatrix)) * tr);
            memcpy(textureMatrix, texTransform.asArray(), sizeof(textureMatrix));
        }

        // Set things up for texturing.
        mTexture.setDimensions(mActiveBuffer->getWidth(), mActiveBuffer->getHeight());
        mTexture.setFiltering(useFiltering);
        mTexture.setMatrix(textureMatrix);

        engine.setupLayerTexturing(mTexture);
    } else {
        engine.setupLayerBlackedOut();
    }
    drawWithOpenGL(hw, clip, useIdentityTransform);
    engine.disableTexturing();
}

#if RK_STEREO
void setStereoDraw(const sp<const DisplayDevice>& hw, RenderEngine& engine,
     Mesh& mMesh, int alreadyStereo, int displayStereo)
{
    Mesh::VertexArray<vec2> position(mMesh.getPositionArray<vec2>());
    if(1==displayStereo && !alreadyStereo) {
        position[0].x /= 2;
        position[1].x /= 2;
        position[2].x /= 2;
        position[3].x /= 2;
        engine.drawMesh(mMesh);

        position[0].x += (hw->getWidth()/2);
        position[1].x += (hw->getWidth()/2);
        position[2].x += (hw->getWidth()/2);
        position[3].x += (hw->getWidth()/2);
    }

    if(2==displayStereo && !alreadyStereo) {
        position[0].y /= 2;
        position[1].y /= 2;
        position[2].y /= 2;
        position[3].y /= 2;
        engine.drawMesh(mMesh);

        position[0].y += (hw->getHeight()/2);
        position[1].y += (hw->getHeight()/2);
        position[2].y += (hw->getHeight()/2);
        position[3].y += (hw->getHeight()/2);
    }
}
#endif

#if RK_VR
void setStereoDrawVR(const sp<const DisplayDevice>& hw, RenderEngine& engine,
     Mesh& mMesh, int alreadyStereo, int displayStereo)
{
    char value[PROPERTY_VALUE_MAX];
    property_get("sys.vr.ipd", value, "0.08");
    float ipd = atof(value);

    Mesh::VertexArray<vec2> position(mMesh.getPositionArray<vec2>());

    if(1==displayStereo && !alreadyStereo) {
        position[0].x /= 2;
        position[1].x /= 2;
        position[2].x /= 2;
        position[3].x /= 2;

        position[0].y /= 2;
        position[1].y /= 2;
        position[2].y /= 2;
        position[3].y /= 2;

        position[0].x += (ipd*hw->getWidth()/4);
        position[1].x += (ipd*hw->getWidth()/4);
        position[2].x += (ipd*hw->getWidth()/4);
        position[3].x += (ipd*hw->getWidth()/4);

        position[0].y += (hw->getHeight()/4);
        position[1].y += (hw->getHeight()/4);
        position[2].y += (hw->getHeight()/4);
        position[3].y += (hw->getHeight()/4);

        engine.drawMesh(mMesh);

        position[0].x -= (ipd*hw->getWidth()/2);
        position[1].x -= (ipd*hw->getWidth()/2);
        position[2].x -= (ipd*hw->getWidth()/2);
        position[3].x -= (ipd*hw->getWidth()/2);

        position[0].x += (hw->getWidth()/2);
        position[1].x += (hw->getWidth()/2);
        position[2].x += (hw->getWidth()/2);
        position[3].x += (hw->getWidth()/2);
    }

    if(2==displayStereo && !alreadyStereo) {
        position[0].y /= 2;
        position[1].y /= 2;
        position[2].y /= 2;
        position[3].y /= 2;

        position[0].x /= 2;
        position[1].x /= 2;
        position[2].x /= 2;
        position[3].x /= 2;

        position[0].y += (ipd*hw->getHeight()/4);
        position[1].y += (ipd*hw->getHeight()/4);
        position[2].y += (ipd*hw->getHeight()/4);
        position[3].y += (ipd*hw->getHeight()/4);

        position[0].x += (hw->getWidth()/4);
        position[1].x += (hw->getWidth()/4);
        position[2].x += (hw->getWidth()/4);
        position[3].x += (hw->getWidth()/4);

        engine.drawMesh(mMesh);

        position[0].y -= (ipd*hw->getHeight()/2);
        position[1].y -= (ipd*hw->getHeight()/2);
        position[2].y -= (ipd*hw->getHeight()/2);
        position[3].y -= (ipd*hw->getHeight()/2);

        position[0].y += (hw->getHeight()/2);
        position[1].y += (hw->getHeight()/2);
        position[2].y += (hw->getHeight()/2);
        position[3].y += (hw->getHeight()/2);
    }
}
#endif

void Layer::clearWithOpenGL(const sp<const DisplayDevice>& hw,
        const Region& /* clip */, float red, float green, float blue,
        float alpha) const
{
    RenderEngine& engine(mFlinger->getRenderEngine());
    computeGeometry(hw, mMesh, false);
    engine.setupFillWithColor(red, green, blue, alpha);
#if RK_VR
    setStereoDrawVR(hw, engine, mMesh,
        mSurfaceFlingerConsumer->getAlreadyStereo()/*getStereoModeToDraw()*/, displayStereo);
#elif RK_STEREO
    setStereoDraw(hw, engine, mMesh,
        mSurfaceFlingerConsumer->getAlreadyStereo(), displayStereo);
#endif

    engine.drawMesh(mMesh);
}

void Layer::clearWithOpenGL(
        const sp<const DisplayDevice>& hw, const Region& clip) const {
    clearWithOpenGL(hw, clip, 0,0,0,0);
}

void Layer::drawWithOpenGL(const sp<const DisplayDevice>& hw,
        const Region& /* clip */, bool useIdentityTransform) const {
    const State& s(getDrawingState());

    computeGeometry(hw, mMesh, useIdentityTransform);

    /*
     * NOTE: the way we compute the texture coordinates here produces
     * different results than when we take the HWC path -- in the later case
     * the "source crop" is rounded to texel boundaries.
     * This can produce significantly different results when the texture
     * is scaled by a large amount.
     *
     * The GL code below is more logical (imho), and the difference with
     * HWC is due to a limitation of the HWC API to integers -- a question
     * is suspend is whether we should ignore this problem or revert to
     * GL composition when a buffer scaling is applied (maybe with some
     * minimal value)? Or, we could make GL behave like HWC -- but this feel
     * like more of a hack.
     */
    Rect win(computeBounds());

    if (!s.finalCrop.isEmpty()) {
        win = s.active.transform.transform(win);
        if (!win.intersect(s.finalCrop, &win)) {
            win.clear();
        }
        win = s.active.transform.inverse().transform(win);
        if (!win.intersect(computeBounds(), &win)) {
            win.clear();
        }
    }

    float left   = float(win.left)   / float(s.active.w);
    float top    = float(win.top)    / float(s.active.h);
    float right  = float(win.right)  / float(s.active.w);
    float bottom = float(win.bottom) / float(s.active.h);

    // TODO: we probably want to generate the texture coords with the mesh
    // here we assume that we only have 4 vertices
    Mesh::VertexArray<vec2> texCoords(mMesh.getTexCoordArray<vec2>());
    texCoords[0] = vec2(left, 1.0f - top);
    texCoords[1] = vec2(left, 1.0f - bottom);
    texCoords[2] = vec2(right, 1.0f - bottom);
    texCoords[3] = vec2(right, 1.0f - top);

    RenderEngine& engine(mFlinger->getRenderEngine());
    engine.setupLayerBlending(mPremultipliedAlpha, isOpaque(s), s.alpha);

#if RK_HDR
    if(mActiveBuffer->getPixelFormat() == HAL_PIXEL_FORMAT_YCrCb_NV12_10
        && (mActiveBuffer->getUsage() & HDRUSAGE)) {
        engine.setupHdr(true);
        engine.setupMRatioTexturing();
    }
#endif

#if RK_VR
    setStereoDrawVR(hw, engine, mMesh,
        mSurfaceFlingerConsumer->getAlreadyStereo()/*getStereoModeToDraw()*/, displayStereo);
#elif RK_STEREO
    setStereoDraw(hw, engine, mMesh,
        mSurfaceFlingerConsumer->getAlreadyStereo(), displayStereo);
#endif

    engine.drawMesh(mMesh);
    engine.disableBlending();
#if RK_HDR
    engine.setupHdr(false);
#endif
}

#ifdef USE_HWC2
void Layer::setCompositionType(int32_t hwcId, HWC2::Composition type,
        bool callIntoHwc) {
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("setCompositionType called without a valid HWC layer");
        return;
    }
    auto& hwcInfo = mHwcLayers[hwcId];
    auto& hwcLayer = hwcInfo.layer;
    ALOGV("setCompositionType(%" PRIx64 ", %s, %d)", hwcLayer->getId(),
            to_string(type).c_str(), static_cast<int>(callIntoHwc));
    if (hwcInfo.compositionType != type) {
        ALOGV("    actually setting");
        hwcInfo.compositionType = type;
        if (callIntoHwc) {
            auto error = hwcLayer->setCompositionType(type);
            ALOGE_IF(error != HWC2::Error::None, "[%s] Failed to set "
                    "composition type %s: %s (%d)", mName.string(),
                    to_string(type).c_str(), to_string(error).c_str(),
                    static_cast<int32_t>(error));
        }
    }
}

HWC2::Composition Layer::getCompositionType(int32_t hwcId) const {
    if (hwcId == DisplayDevice::DISPLAY_ID_INVALID) {
        // If we're querying the composition type for a display that does not
        // have a HWC counterpart, then it will always be Client
        return HWC2::Composition::Client;
    }
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("getCompositionType called with an invalid HWC layer");
        return HWC2::Composition::Invalid;
    }
    return mHwcLayers.at(hwcId).compositionType;
}

void Layer::setClearClientTarget(int32_t hwcId, bool clear) {
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("setClearClientTarget called without a valid HWC layer");
        return;
    }
    mHwcLayers[hwcId].clearClientTarget = clear;
}

bool Layer::getClearClientTarget(int32_t hwcId) const {
    if (mHwcLayers.count(hwcId) == 0) {
        ALOGE("getClearClientTarget called without a valid HWC layer");
        return false;
    }
    return mHwcLayers.at(hwcId).clearClientTarget;
}
#endif

uint32_t Layer::getProducerStickyTransform() const {
    int producerStickyTransform = 0;
    int ret = mProducer->query(NATIVE_WINDOW_STICKY_TRANSFORM, &producerStickyTransform);
    if (ret != OK) {
        ALOGW("%s: Error %s (%d) while querying window sticky transform.", __FUNCTION__,
                strerror(-ret), ret);
        return 0;
    }
    return static_cast<uint32_t>(producerStickyTransform);
}

bool Layer::latchUnsignaledBuffers() {
    static bool propertyLoaded = false;
    static bool latch = false;
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    if (!propertyLoaded) {
        char value[PROPERTY_VALUE_MAX] = {};
        property_get("debug.sf.latch_unsignaled", value, "0");
        latch = atoi(value);
        propertyLoaded = true;
    }
    return latch;
}

uint64_t Layer::getHeadFrameNumber() const {
    Mutex::Autolock lock(mQueueItemLock);
    if (!mQueueItems.empty()) {
        return mQueueItems[0].mFrameNumber;
    } else {
        return mCurrentFrameNumber;
    }
}

bool Layer::headFenceHasSignaled() const {
#ifdef USE_HWC2
    if (latchUnsignaledBuffers()) {
        return true;
    }

    Mutex::Autolock lock(mQueueItemLock);
    if (mQueueItems.empty()) {
        return true;
    }
    if (mQueueItems[0].mIsDroppable) {
        // Even though this buffer's fence may not have signaled yet, it could
        // be replaced by another buffer before it has a chance to, which means
        // that it's possible to get into a situation where a buffer is never
        // able to be latched. To avoid this, grab this buffer anyway.
        return true;
    }
    return mQueueItems[0].mFence->getSignalTime() != INT64_MAX;
#else
    return true;
#endif
}

bool Layer::addSyncPoint(const std::shared_ptr<SyncPoint>& point) {
    if (point->getFrameNumber() <= mCurrentFrameNumber) {
        // Don't bother with a SyncPoint, since we've already latched the
        // relevant frame
        return false;
    }

    Mutex::Autolock lock(mLocalSyncPointMutex);
    mLocalSyncPoints.push_back(point);
    return true;
}

void Layer::setFiltering(bool filtering) {
    mFiltering = filtering;
}

bool Layer::getFiltering() const {
    return mFiltering;
}

// As documented in libhardware header, formats in the range
// 0x100 - 0x1FF are specific to the HAL implementation, and
// are known to have no alpha channel
// TODO: move definition for device-specific range into
// hardware.h, instead of using hard-coded values here.
#define HARDWARE_IS_DEVICE_FORMAT(f) ((f) >= 0x100 && (f) <= 0x1FF)

bool Layer::getOpacityForFormat(uint32_t format) {
    if (HARDWARE_IS_DEVICE_FORMAT(format)) {
        return true;
    }
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            return false;
    }
    // in all other case, we have no blending (also for unknown formats)
    return true;
}

// ----------------------------------------------------------------------------
// local state
// ----------------------------------------------------------------------------

static void boundPoint(vec2* point, const Rect& crop) {
    if (point->x < crop.left) {
        point->x = crop.left;
    }
    if (point->x > crop.right) {
        point->x = crop.right;
    }
    if (point->y < crop.top) {
        point->y = crop.top;
    }
    if (point->y > crop.bottom) {
        point->y = crop.bottom;
    }
}

void Layer::computeGeometry(const sp<const DisplayDevice>& hw, Mesh& mesh,
        bool useIdentityTransform) const
{
    const Layer::State& s(getDrawingState());
#if RK_HW_ROTATION
    Transform tr(hw->getTransform());
    const Transform identity;
#else
    const Transform tr(hw->getTransform());
#endif
    const uint32_t hw_h = hw->getHeight();
    Rect win(s.active.w, s.active.h);

    D("mDrawingScreenshot : %d, useIdentityTransform : %d.", mDrawingScreenshot, useIdentityTransform);
#if RK_HW_ROTATION
    if (mDrawingScreenshot) {
        computeHWGeometry(tr, identity, hw);
    }
#endif

    if (!s.crop.isEmpty()) {
        win.intersect(s.crop, &win);
    }
    // subtract the transparent region and snap to the bounds
    win = reduce(win, s.activeTransparentRegion);

    vec2 lt = vec2(win.left, win.top);
    vec2 lb = vec2(win.left, win.bottom);
    vec2 rb = vec2(win.right, win.bottom);
    vec2 rt = vec2(win.right, win.top);

    if (!useIdentityTransform) {
        lt = s.active.transform.transform(lt);
        lb = s.active.transform.transform(lb);
        rb = s.active.transform.transform(rb);
        rt = s.active.transform.transform(rt);
    }

    if (!s.finalCrop.isEmpty()) {
        boundPoint(&lt, s.finalCrop);
        boundPoint(&lb, s.finalCrop);
        boundPoint(&rb, s.finalCrop);
        boundPoint(&rt, s.finalCrop);
    }

    Mesh::VertexArray<vec2> position(mesh.getPositionArray<vec2>());
    position[0] = tr.transform(lt);
    position[1] = tr.transform(lb);
    position[2] = tr.transform(rb);
    position[3] = tr.transform(rt);
    for (size_t i=0 ; i<4 ; i++) {
        position[i].y = hw_h - position[i].y;
    }
}

#if RK_HW_ROTATION
void Layer::computeHWGeometry(Transform& tr,
                              const Transform& layerTransform,
                              const sp<const DisplayDevice>& hw) const
{
    int hwrotation = mFlinger->getHardwareOrientation();
    int hw_offset = hw->getWidth() - hw->getHeight();

    if (hwrotation == DisplayState::eOrientation90) {
        // 90 degree
        tr = hw->getTransform(false) * layerTransform;
        switch (hw->getHardwareRotation()){
        case 0:
            tr.set(tr.tx(), tr.ty());
            break;
        case 1:
            tr.set(tr.tx(), tr.ty()-hw_offset);
            break;
        case 2:
            tr.set(tr.tx()-hw_offset, tr.ty()-hw_offset);
            break;
        case 3:
            tr.set(tr.tx()-hw_offset, tr.ty());
            break;
        }
    } else if (hwrotation == DisplayState::eOrientation180) {
        // 180 degree
        tr = hw->getTransform(false) * layerTransform;
        tr.set(tr.tx(), tr.ty());
    } else if (hwrotation == DisplayState::eOrientation270) {
        // 270 degree
        tr = hw->getTransform(false) * layerTransform;
        switch (hw->getHardwareRotation()){
        case 0:
            tr.set(tr.tx()-hw_offset, tr.ty()-hw_offset);
            break;
        case 1:
            tr.set(tr.tx()-hw_offset, tr.ty());
            break;
        case 2:
            tr.set(tr.tx(), tr.ty());
            break;
        case 3:
            tr.set(tr.tx(), tr.ty()-hw_offset);
            break;
        }
    }
}
#endif

bool Layer::isOpaque(const Layer::State& s) const
{
    // if we don't have a buffer yet, we're translucent regardless of the
    // layer's opaque flag.
    if (mActiveBuffer == 0) {
        return false;
    }

    // if the layer has the opaque flag, then we're always opaque,
    // otherwise we use the current buffer's format.
    return ((s.flags & layer_state_t::eLayerOpaque) != 0) || mCurrentOpacity;
}

bool Layer::isSecure() const
{
    const Layer::State& s(mDrawingState);
    return (s.flags & layer_state_t::eLayerSecure);
}

bool Layer::isProtected() const
{
    const sp<GraphicBuffer>& activeBuffer(mActiveBuffer);
    return (activeBuffer != 0) &&
            (activeBuffer->getUsage() & GRALLOC_USAGE_PROTECTED);
}

bool Layer::isFixedSize() const {
    return getEffectiveScalingMode() != NATIVE_WINDOW_SCALING_MODE_FREEZE;
}

bool Layer::isCropped() const {
    return !mCurrentCrop.isEmpty();
}

bool Layer::needsFiltering(const sp<const DisplayDevice>& hw) const {
    return mNeedsFiltering || hw->needsFiltering();
}

void Layer::setVisibleRegion(const Region& visibleRegion) {
    // always called from main thread
    this->visibleRegion = visibleRegion;
}

void Layer::setCoveredRegion(const Region& coveredRegion) {
    // always called from main thread
    this->coveredRegion = coveredRegion;
}

void Layer::setVisibleNonTransparentRegion(const Region&
        setVisibleNonTransparentRegion) {
    // always called from main thread
    this->visibleNonTransparentRegion = setVisibleNonTransparentRegion;
}

// ----------------------------------------------------------------------------
// transaction
// ----------------------------------------------------------------------------

void Layer::pushPendingState() {
    if (!mCurrentState.modified) {
        return;
    }

    // If this transaction is waiting on the receipt of a frame, generate a sync
    // point and send it to the remote layer.
    if (mCurrentState.handle != nullptr) {
        sp<IBinder> strongBinder = mCurrentState.handle.promote();
        sp<Handle> handle = nullptr;
        sp<Layer> handleLayer = nullptr;
        if (strongBinder != nullptr) {
            handle = static_cast<Handle*>(strongBinder.get());
            handleLayer = handle->owner.promote();
        }
        if (strongBinder == nullptr || handleLayer == nullptr) {
            ALOGE("[%s] Unable to promote Layer handle", mName.string());
            // If we can't promote the layer we are intended to wait on,
            // then it is expired or otherwise invalid. Allow this transaction
            // to be applied as per normal (no synchronization).
            mCurrentState.handle = nullptr;
        } else {
            auto syncPoint = std::make_shared<SyncPoint>(
                    mCurrentState.frameNumber);
            if (handleLayer->addSyncPoint(syncPoint)) {
                mRemoteSyncPoints.push_back(std::move(syncPoint));
            } else {
                // We already missed the frame we're supposed to synchronize
                // on, so go ahead and apply the state update
                mCurrentState.handle = nullptr;
            }
        }

        // Wake us up to check if the frame has been received
        setTransactionFlags(eTransactionNeeded);
    }
    mPendingStates.push_back(mCurrentState);
}

void Layer::popPendingState(State* stateToCommit) {
    auto oldFlags = stateToCommit->flags;
    *stateToCommit = mPendingStates[0];
    stateToCommit->flags = (oldFlags & ~stateToCommit->mask) |
            (stateToCommit->flags & stateToCommit->mask);

    mPendingStates.removeAt(0);
}

bool Layer::applyPendingStates(State* stateToCommit) {
    bool stateUpdateAvailable = false;
    while (!mPendingStates.empty()) {
        if (mPendingStates[0].handle != nullptr) {
            if (mRemoteSyncPoints.empty()) {
                // If we don't have a sync point for this, apply it anyway. It
                // will be visually wrong, but it should keep us from getting
                // into too much trouble.
                ALOGE("[%s] No local sync point found", mName.string());
                popPendingState(stateToCommit);
                stateUpdateAvailable = true;
                continue;
            }

            if (mRemoteSyncPoints.front()->getFrameNumber() !=
                    mPendingStates[0].frameNumber) {
                ALOGE("[%s] Unexpected sync point frame number found",
                        mName.string());

                // Signal our end of the sync point and then dispose of it
                mRemoteSyncPoints.front()->setTransactionApplied();
                mRemoteSyncPoints.pop_front();
                continue;
            }

            if (mRemoteSyncPoints.front()->frameIsAvailable()) {
                // Apply the state update
                popPendingState(stateToCommit);
                stateUpdateAvailable = true;

                // Signal our end of the sync point and then dispose of it
                mRemoteSyncPoints.front()->setTransactionApplied();
                mRemoteSyncPoints.pop_front();
            } else {
                break;
            }
        } else {
            popPendingState(stateToCommit);
            stateUpdateAvailable = true;
        }
    }

    // If we still have pending updates, wake SurfaceFlinger back up and point
    // it at this layer so we can process them
    if (!mPendingStates.empty()) {
        setTransactionFlags(eTransactionNeeded);
        mFlinger->setTransactionFlags(eTraversalNeeded);
    }

    mCurrentState.modified = false;
    return stateUpdateAvailable;
}

void Layer::notifyAvailableFrames() {
    auto headFrameNumber = getHeadFrameNumber();
    bool headFenceSignaled = headFenceHasSignaled();
    Mutex::Autolock lock(mLocalSyncPointMutex);
    for (auto& point : mLocalSyncPoints) {
        if (headFrameNumber >= point->getFrameNumber() && headFenceSignaled) {
            point->setFrameAvailable();
        }
    }
}

uint32_t Layer::doTransaction(uint32_t flags) {
    ATRACE_CALL();

    pushPendingState();
    Layer::State c = getCurrentState();
    if (!applyPendingStates(&c)) {
        return 0;
    }

    const Layer::State& s(getDrawingState());

    const bool sizeChanged = (c.requested.w != s.requested.w) ||
                             (c.requested.h != s.requested.h);

    if (sizeChanged) {
        // the size changed, we need to ask our client to request a new buffer
        ALOGD_IF(DEBUG_RESIZE,
                "doTransaction: geometry (layer=%p '%s'), tr=%02x, scalingMode=%d\n"
                "  current={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                "            requested={ wh={%4u,%4u} }}\n"
                "  drawing={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                "            requested={ wh={%4u,%4u} }}\n",
                this, getName().string(), mCurrentTransform,
                getEffectiveScalingMode(),
                c.active.w, c.active.h,
                c.crop.left,
                c.crop.top,
                c.crop.right,
                c.crop.bottom,
                c.crop.getWidth(),
                c.crop.getHeight(),
                c.requested.w, c.requested.h,
                s.active.w, s.active.h,
                s.crop.left,
                s.crop.top,
                s.crop.right,
                s.crop.bottom,
                s.crop.getWidth(),
                s.crop.getHeight(),
                s.requested.w, s.requested.h);

        // record the new size, form this point on, when the client request
        // a buffer, it'll get the new size.
        mSurfaceFlingerConsumer->setDefaultBufferSize(
                c.requested.w, c.requested.h);
    }

    const bool resizePending = (c.requested.w != c.active.w) ||
            (c.requested.h != c.active.h);
    if (!isFixedSize()) {
        if (resizePending && mSidebandStream == NULL) {
            // don't let Layer::doTransaction update the drawing state
            // if we have a pending resize, unless we are in fixed-size mode.
            // the drawing state will be updated only once we receive a buffer
            // with the correct size.
            //
            // in particular, we want to make sure the clip (which is part
            // of the geometry state) is latched together with the size but is
            // latched immediately when no resizing is involved.
            //
            // If a sideband stream is attached, however, we want to skip this
            // optimization so that transactions aren't missed when a buffer
            // never arrives

            flags |= eDontUpdateGeometryState;
        }
    }

    // always set active to requested, unless we're asked not to
    // this is used by Layer, which special cases resizes.
    if (flags & eDontUpdateGeometryState)  {
    } else {
        Layer::State& editCurrentState(getCurrentState());
        if (mFreezePositionUpdates) {
            float tx = c.active.transform.tx();
            float ty = c.active.transform.ty();
            c.active = c.requested;
            c.active.transform.set(tx, ty);
            editCurrentState.active = c.active;
        } else {
            editCurrentState.active = editCurrentState.requested;
            c.active = c.requested;
        }
    }

    if (s.active != c.active) {
        // invalidate and recompute the visible regions if needed
        flags |= Layer::eVisibleRegion;
    }

    if (c.sequence != s.sequence) {
        // invalidate and recompute the visible regions if needed
        flags |= eVisibleRegion;
        this->contentDirty = true;

        // we may use linear filtering, if the matrix scales us
        const uint8_t type = c.active.transform.getType();
        mNeedsFiltering = (!c.active.transform.preserveRects() ||
                (type >= Transform::SCALE));
    }

    // If the layer is hidden, signal and clear out all local sync points so
    // that transactions for layers depending on this layer's frames becoming
    // visible are not blocked
    if (c.flags & layer_state_t::eLayerHidden) {
        Mutex::Autolock lock(mLocalSyncPointMutex);
        for (auto& point : mLocalSyncPoints) {
            point->setFrameAvailable();
        }
        mLocalSyncPoints.clear();
    }

    // Commit the transaction
    commitTransaction(c);
    return flags;
}

void Layer::commitTransaction(const State& stateToCommit) {
    mDrawingState = stateToCommit;
}

uint32_t Layer::getTransactionFlags(uint32_t flags) {
    return android_atomic_and(~flags, &mTransactionFlags) & flags;
}

uint32_t Layer::setTransactionFlags(uint32_t flags) {
    return android_atomic_or(flags, &mTransactionFlags);
}

bool Layer::setPosition(float x, float y, bool immediate) {
    if (mCurrentState.requested.transform.tx() == x && mCurrentState.requested.transform.ty() == y)
        return false;
    mCurrentState.sequence++;

    if (x > 117440511)
        x = 117440511;

    if (y > 117440511)
        y = 117440511;

    // We update the requested and active position simultaneously because
    // we want to apply the position portion of the transform matrix immediately,
    // but still delay scaling when resizing a SCALING_MODE_FREEZE layer.
    mCurrentState.requested.transform.set(x, y);
    if (immediate && !mFreezePositionUpdates) {
        mCurrentState.active.transform.set(x, y);
    }
    mFreezePositionUpdates = mFreezePositionUpdates || !immediate;

    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setLayer(uint32_t z) {
    if (mCurrentState.z == z)
        return false;
    mCurrentState.sequence++;
    mCurrentState.z = z;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setSize(uint32_t w, uint32_t h) {
    if (mCurrentState.requested.w == w && mCurrentState.requested.h == h)
        return false;
    mCurrentState.requested.w = w;
    mCurrentState.requested.h = h;
    sp<const DisplayDevice> hw = mFlinger->getDefaultDisplayDevice();

    //rk: add by hds to fix show half of ImageWallpaper bug.
    if(!strcmp(getName().string(),"com.android.systemui.ImageWallpaper"))
    {
        if(w <= (uint32_t) (hw->getWidth()))
        {
            mCurrentState.requested.w = 1;
            mCurrentState.requested.h = 1;
        }
    }

    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
#ifdef USE_HWC2
bool Layer::setAlpha(float alpha) {
#else
bool Layer::setAlpha(uint8_t alpha) {
#endif
    if (mCurrentState.alpha == alpha)
        return false;
    mCurrentState.sequence++;
    mCurrentState.alpha = alpha;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setMatrix(const layer_state_t::matrix22_t& matrix) {
    mCurrentState.sequence++;
    mCurrentState.requested.transform.set(
            matrix.dsdx, matrix.dsdy, matrix.dtdx, matrix.dtdy);
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setTransparentRegionHint(const Region& transparent) {
    mCurrentState.requestedTransparentRegion = transparent;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setFlags(uint8_t flags, uint8_t mask) {
    const uint32_t newFlags = (mCurrentState.flags & ~mask) | (flags & mask);
    if (mCurrentState.flags == newFlags)
        return false;
    mCurrentState.sequence++;
    mCurrentState.flags = newFlags;
    mCurrentState.mask = mask;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setCrop(const Rect& crop, bool immediate) {
    if (mCurrentState.crop == crop)
        return false;
    mCurrentState.sequence++;
    mCurrentState.requestedCrop = crop;
    if (immediate) {
        mCurrentState.crop = crop;
    }
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}
bool Layer::setFinalCrop(const Rect& crop) {
    if (mCurrentState.finalCrop == crop)
        return false;
    mCurrentState.sequence++;
    mCurrentState.finalCrop = crop;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

bool Layer::setOverrideScalingMode(int32_t scalingMode) {
    if (scalingMode == mOverrideScalingMode)
        return false;
    mOverrideScalingMode = scalingMode;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

uint32_t Layer::getEffectiveScalingMode() const {
    if (mOverrideScalingMode >= 0) {
      return mOverrideScalingMode;
    }
    return mCurrentScalingMode;
}

bool Layer::setLayerStack(uint32_t layerStack) {
    if (mCurrentState.layerStack == layerStack)
        return false;
    mCurrentState.sequence++;
    mCurrentState.layerStack = layerStack;
    mCurrentState.modified = true;
    setTransactionFlags(eTransactionNeeded);
    return true;
}

void Layer::deferTransactionUntil(const sp<IBinder>& handle,
        uint64_t frameNumber) {
    mCurrentState.handle = handle;
    mCurrentState.frameNumber = frameNumber;
    // We don't set eTransactionNeeded, because just receiving a deferral
    // request without any other state updates shouldn't actually induce a delay
    mCurrentState.modified = true;
    pushPendingState();
    mCurrentState.handle = nullptr;
    mCurrentState.frameNumber = 0;
    mCurrentState.modified = false;
}

void Layer::useSurfaceDamage() {
    if (mFlinger->mForceFullDamage) {
        surfaceDamageRegion = Region::INVALID_REGION;
    } else {
        surfaceDamageRegion = mSurfaceFlingerConsumer->getSurfaceDamage();
    }
}

void Layer::useEmptyDamage() {
    surfaceDamageRegion.clear();
}

// ----------------------------------------------------------------------------
// pageflip handling...
// ----------------------------------------------------------------------------

bool Layer::shouldPresentNow(const DispSync& dispSync) const {
    if (mSidebandStreamChanged || mAutoRefresh) {
        return true;
    }

    Mutex::Autolock lock(mQueueItemLock);
    if (mQueueItems.empty()) {
        return false;
    }
    auto timestamp = mQueueItems[0].mTimestamp;
    nsecs_t expectedPresent =
            mSurfaceFlingerConsumer->computeExpectedPresent(dispSync);

    // Ignore timestamps more than a second in the future
    bool isPlausible = timestamp < (expectedPresent + s2ns(1));
    ALOGW_IF(!isPlausible, "[%s] Timestamp %" PRId64 " seems implausible "
            "relative to expectedPresent %" PRId64, mName.string(), timestamp,
            expectedPresent);

    bool isDue = timestamp < expectedPresent;
    return isDue || !isPlausible;
}

bool Layer::onPreComposition() {
    mRefreshPending = false;
    return mQueuedFrames > 0 || mSidebandStreamChanged || mAutoRefresh;
}

bool Layer::onPostComposition() {
    bool frameLatencyNeeded = mFrameLatencyNeeded;
    if (mFrameLatencyNeeded) {
        nsecs_t desiredPresentTime = mSurfaceFlingerConsumer->getTimestamp();
        mFrameTracker.setDesiredPresentTime(desiredPresentTime);

        sp<Fence> frameReadyFence = mSurfaceFlingerConsumer->getCurrentFence();
        if (frameReadyFence->isValid()) {
            mFrameTracker.setFrameReadyFence(frameReadyFence);
        } else {
            // There was no fence for this frame, so assume that it was ready
            // to be presented at the desired present time.
            mFrameTracker.setFrameReadyTime(desiredPresentTime);
        }

        const HWComposer& hwc = mFlinger->getHwComposer();
#ifdef USE_HWC2
        sp<Fence> presentFence = hwc.getRetireFence(HWC_DISPLAY_PRIMARY);
#else
        sp<Fence> presentFence = hwc.getDisplayFence(HWC_DISPLAY_PRIMARY);
#endif
        if (presentFence->isValid()) {
            mFrameTracker.setActualPresentFence(presentFence);
        } else {
            // The HWC doesn't support present fences, so use the refresh
            // timestamp instead.
            nsecs_t presentTime = hwc.getRefreshTimestamp(HWC_DISPLAY_PRIMARY);
            mFrameTracker.setActualPresentTime(presentTime);
        }

        mFrameTracker.advanceFrame();
        mFrameLatencyNeeded = false;
    }
    return frameLatencyNeeded;
}

#ifdef USE_HWC2
void Layer::releasePendingBuffer() {
    mSurfaceFlingerConsumer->releasePendingBuffer();
}
#endif

bool Layer::isVisible() const {
    const Layer::State& s(mDrawingState);
#ifdef USE_HWC2
    return !(s.flags & layer_state_t::eLayerHidden) && s.alpha > 0.0f
            && (mActiveBuffer != NULL || mSidebandStream != NULL);
#else
    return !(s.flags & layer_state_t::eLayerHidden) && s.alpha
            && (mActiveBuffer != NULL || mSidebandStream != NULL);
#endif
}

Region Layer::latchBuffer(bool& recomputeVisibleRegions)
{
    ATRACE_CALL();

    if (android_atomic_acquire_cas(true, false, &mSidebandStreamChanged) == 0) {
        // mSidebandStreamChanged was true
        mSidebandStream = mSurfaceFlingerConsumer->getSidebandStream();
        if (mSidebandStream != NULL) {
            setTransactionFlags(eTransactionNeeded);
            mFlinger->setTransactionFlags(eTraversalNeeded);
        }
        recomputeVisibleRegions = true;

        const State& s(getDrawingState());
        return s.active.transform.transform(Region(Rect(s.active.w, s.active.h)));
    }

    Region outDirtyRegion;
    if (mQueuedFrames > 0 || mAutoRefresh) {

        // if we've already called updateTexImage() without going through
        // a composition step, we have to skip this layer at this point
        // because we cannot call updateTeximage() without a corresponding
        // compositionComplete() call.
        // we'll trigger an update in onPreComposition().
        if (mRefreshPending) {
            return outDirtyRegion;
        }

        // If the head buffer's acquire fence hasn't signaled yet, return and
        // try again later
        if (!headFenceHasSignaled()) {
            mFlinger->signalLayerUpdate();
            return outDirtyRegion;
        }

        // Capture the old state of the layer for comparisons later
        const State& s(getDrawingState());
        const bool oldOpacity = isOpaque(s);
        sp<GraphicBuffer> oldActiveBuffer = mActiveBuffer;

        struct Reject : public SurfaceFlingerConsumer::BufferRejecter {
            Layer::State& front;
            Layer::State& current;
            bool& recomputeVisibleRegions;
            bool stickyTransformSet;
            const char* name;
            int32_t overrideScalingMode;
            bool& freezePositionUpdates;

            Reject(Layer::State& front, Layer::State& current,
                    bool& recomputeVisibleRegions, bool stickySet,
                    const char* name,
                    int32_t overrideScalingMode,
                    bool& freezePositionUpdates)
                : front(front), current(current),
                  recomputeVisibleRegions(recomputeVisibleRegions),
                  stickyTransformSet(stickySet),
                  name(name),
                  overrideScalingMode(overrideScalingMode),
                  freezePositionUpdates(freezePositionUpdates) {
            }

            virtual bool reject(const sp<GraphicBuffer>& buf,
                    const BufferItem& item) {
                if (buf == NULL) {
                    return false;
                }

                uint32_t bufWidth  = buf->getWidth();
                uint32_t bufHeight = buf->getHeight();

                // check that we received a buffer of the right size
                // (Take the buffer's orientation into account)
                if (item.mTransform & Transform::ROT_90) {
                    swap(bufWidth, bufHeight);
                }

                int actualScalingMode = overrideScalingMode >= 0 ?
                        overrideScalingMode : item.mScalingMode;
                bool isFixedSize = actualScalingMode != NATIVE_WINDOW_SCALING_MODE_FREEZE;
                if (front.active != front.requested) {

                    if (isFixedSize ||
                            (bufWidth == front.requested.w &&
                             bufHeight == front.requested.h))
                    {
                        // Here we pretend the transaction happened by updating the
                        // current and drawing states. Drawing state is only accessed
                        // in this thread, no need to have it locked
                        front.active = front.requested;

                        // We also need to update the current state so that
                        // we don't end-up overwriting the drawing state with
                        // this stale current state during the next transaction
                        //
                        // NOTE: We don't need to hold the transaction lock here
                        // because State::active is only accessed from this thread.
                        current.active = front.active;
                        current.modified = true;

                        // recompute visible region
                        recomputeVisibleRegions = true;
                    }

                    ALOGD_IF(DEBUG_RESIZE,
                            "[%s] latchBuffer/reject: buffer (%ux%u, tr=%02x), scalingMode=%d\n"
                            "  drawing={ active   ={ wh={%4u,%4u} crop={%4d,%4d,%4d,%4d} (%4d,%4d) }\n"
                            "            requested={ wh={%4u,%4u} }}\n",
                            name,
                            bufWidth, bufHeight, item.mTransform, item.mScalingMode,
                            front.active.w, front.active.h,
                            front.crop.left,
                            front.crop.top,
                            front.crop.right,
                            front.crop.bottom,
                            front.crop.getWidth(),
                            front.crop.getHeight(),
                            front.requested.w, front.requested.h);
                }

                if (!isFixedSize && !stickyTransformSet) {
                    if (front.active.w != bufWidth ||
                        front.active.h != bufHeight) {
                        // reject this buffer
                        ALOGE("[%s] rejecting buffer: "
                                "bufWidth=%d, bufHeight=%d, front.active.{w=%d, h=%d}",
                                name, bufWidth, bufHeight, front.active.w, front.active.h);
                        return true;
                    }
                }

                // if the transparent region has changed (this test is
                // conservative, but that's fine, worst case we're doing
                // a bit of extra work), we latch the new one and we
                // trigger a visible-region recompute.
                if (!front.activeTransparentRegion.isTriviallyEqual(
                        front.requestedTransparentRegion)) {
                    front.activeTransparentRegion = front.requestedTransparentRegion;

                    // We also need to update the current state so that
                    // we don't end-up overwriting the drawing state with
                    // this stale current state during the next transaction
                    //
                    // NOTE: We don't need to hold the transaction lock here
                    // because State::active is only accessed from this thread.
                    current.activeTransparentRegion = front.activeTransparentRegion;

                    // recompute visible region
                    recomputeVisibleRegions = true;
                }

                if (front.crop != front.requestedCrop) {
                    front.crop = front.requestedCrop;
                    current.crop = front.requestedCrop;
                    recomputeVisibleRegions = true;
                }
                freezePositionUpdates = false;

                return false;
            }
        };

        Reject r(mDrawingState, getCurrentState(), recomputeVisibleRegions,
                getProducerStickyTransform() != 0, mName.string(),
                mOverrideScalingMode, mFreezePositionUpdates);


        // Check all of our local sync points to ensure that all transactions
        // which need to have been applied prior to the frame which is about to
        // be latched have signaled

        auto headFrameNumber = getHeadFrameNumber();
        bool matchingFramesFound = false;
        bool allTransactionsApplied = true;
        {
            Mutex::Autolock lock(mLocalSyncPointMutex);
            for (auto& point : mLocalSyncPoints) {
                if (point->getFrameNumber() > headFrameNumber) {
                    break;
                }

                matchingFramesFound = true;

                if (!point->frameIsAvailable()) {
                    // We haven't notified the remote layer that the frame for
                    // this point is available yet. Notify it now, and then
                    // abort this attempt to latch.
                    point->setFrameAvailable();
                    allTransactionsApplied = false;
                    break;
                }

                allTransactionsApplied &= point->transactionIsApplied();
            }
        }

        if (matchingFramesFound && !allTransactionsApplied) {
            mFlinger->signalLayerUpdate();
            return outDirtyRegion;
        }

        // This boolean is used to make sure that SurfaceFlinger's shadow copy
        // of the buffer queue isn't modified when the buffer queue is returning
        // BufferItem's that weren't actually queued. This can happen in shared
        // buffer mode.
        bool queuedBuffer = false;
        status_t updateResult = mSurfaceFlingerConsumer->updateTexImage(&r,
                mFlinger->mPrimaryDispSync, &mAutoRefresh, &queuedBuffer,
                mLastFrameNumberReceived);
        if (updateResult == BufferQueue::PRESENT_LATER) {
            // Producer doesn't want buffer to be displayed yet.  Signal a
            // layer update so we check again at the next opportunity.
            mFlinger->signalLayerUpdate();
            return outDirtyRegion;
        } else if (updateResult == SurfaceFlingerConsumer::BUFFER_REJECTED) {
            // If the buffer has been rejected, remove it from the shadow queue
            // and return early
            if (queuedBuffer) {
                Mutex::Autolock lock(mQueueItemLock);
                mQueueItems.removeAt(0);
                android_atomic_dec(&mQueuedFrames);
            }
            return outDirtyRegion;
        } else if (updateResult != NO_ERROR || mUpdateTexImageFailed) {
            // This can occur if something goes wrong when trying to create the
            // EGLImage for this buffer. If this happens, the buffer has already
            // been released, so we need to clean up the queue and bug out
            // early.
            if (queuedBuffer) {
                Mutex::Autolock lock(mQueueItemLock);
                mQueueItems.clear();
                android_atomic_and(0, &mQueuedFrames);
            }

            // Once we have hit this state, the shadow queue may no longer
            // correctly reflect the incoming BufferQueue's contents, so even if
            // updateTexImage starts working, the only safe course of action is
            // to continue to ignore updates.
            mUpdateTexImageFailed = true;

            return outDirtyRegion;
        }

        if (queuedBuffer) {
            // Autolock scope
            auto currentFrameNumber = mSurfaceFlingerConsumer->getFrameNumber();

            Mutex::Autolock lock(mQueueItemLock);

            // Remove any stale buffers that have been dropped during
            // updateTexImage
            while (mQueueItems[0].mFrameNumber != currentFrameNumber) {
                mQueueItems.removeAt(0);
                android_atomic_dec(&mQueuedFrames);
            }

            mQueueItems.removeAt(0);
        }


        // Decrement the queued-frames count.  Signal another event if we
        // have more frames pending.
        if ((queuedBuffer && android_atomic_dec(&mQueuedFrames) > 1)
                || mAutoRefresh) {
            mFlinger->signalLayerUpdate();
        }

        if (updateResult != NO_ERROR) {
            // something happened!
            recomputeVisibleRegions = true;
            return outDirtyRegion;
        }

        // update the active buffer
        mActiveBuffer = mSurfaceFlingerConsumer->getCurrentBuffer();
        if (mActiveBuffer == NULL) {
            // this can only happen if the very first buffer was rejected.
            return outDirtyRegion;
        }

        mRefreshPending = true;
        mFrameLatencyNeeded = true;
        if (oldActiveBuffer == NULL) {
             // the first time we receive a buffer, we need to trigger a
             // geometry invalidation.
            recomputeVisibleRegions = true;
         }

        Rect crop(mSurfaceFlingerConsumer->getCurrentCrop());
        const uint32_t transform(mSurfaceFlingerConsumer->getCurrentTransform());
        const uint32_t scalingMode(mSurfaceFlingerConsumer->getCurrentScalingMode());
        if ((crop != mCurrentCrop) ||
            (transform != mCurrentTransform) ||
            (scalingMode != mCurrentScalingMode))
        {
            mCurrentCrop = crop;
            mCurrentTransform = transform;
            mCurrentScalingMode = scalingMode;
            recomputeVisibleRegions = true;
        }

        if (oldActiveBuffer != NULL) {
            uint32_t bufWidth  = mActiveBuffer->getWidth();
            uint32_t bufHeight = mActiveBuffer->getHeight();
            if (bufWidth != uint32_t(oldActiveBuffer->width) ||
                bufHeight != uint32_t(oldActiveBuffer->height)) {
                recomputeVisibleRegions = true;
            }
        }

        mCurrentOpacity = getOpacityForFormat(mActiveBuffer->format);
        if (oldOpacity != isOpaque(s)) {
            recomputeVisibleRegions = true;
        }

        mCurrentFrameNumber = mSurfaceFlingerConsumer->getFrameNumber();

        // Remove any sync points corresponding to the buffer which was just
        // latched
        {
            Mutex::Autolock lock(mLocalSyncPointMutex);
            auto point = mLocalSyncPoints.begin();
            while (point != mLocalSyncPoints.end()) {
                if (!(*point)->frameIsAvailable() ||
                        !(*point)->transactionIsApplied()) {
                    // This sync point must have been added since we started
                    // latching. Don't drop it yet.
                    ++point;
                    continue;
                }

                if ((*point)->getFrameNumber() <= mCurrentFrameNumber) {
                    point = mLocalSyncPoints.erase(point);
                } else {
                    ++point;
                }
            }
        }

        // FIXME: postedRegion should be dirty & bounds
        Region dirtyRegion(Rect(s.active.w, s.active.h));

        // transform the dirty region to window-manager space
        outDirtyRegion = (s.active.transform.transform(dirtyRegion));
    }
    return outDirtyRegion;
}

uint32_t Layer::getEffectiveUsage(uint32_t usage) const
{
    // TODO: should we do something special if mSecure is set?
    if (mProtectedByApp) {
        // need a hardware-protected path to external video sink
        usage |= GraphicBuffer::USAGE_PROTECTED;
    }
    if (mPotentialCursor) {
        usage |= GraphicBuffer::USAGE_CURSOR;
    }
    usage |= GraphicBuffer::USAGE_HW_COMPOSER;
    return usage;
}

void Layer::updateTransformHint(const sp<const DisplayDevice>& hw) const {
    uint32_t orientation = 0;
    if (!mFlinger->mDebugDisableTransformHint) {
        // The transform hint is used to improve performance, but we can
        // only have a single transform hint, it cannot
        // apply to all displays.
        const Transform& planeTransform(hw->getTransform());
        orientation = planeTransform.getOrientation();
        if (orientation & Transform::ROT_INVALID) {
            orientation = 0;
        }
    }
    mSurfaceFlingerConsumer->setTransformHint(orientation);
}

// ----------------------------------------------------------------------------
// debugging
// ----------------------------------------------------------------------------

void Layer::dump(String8& result, Colorizer& colorizer) const
{
    const Layer::State& s(getDrawingState());

    colorizer.colorize(result, Colorizer::GREEN);
    result.appendFormat(
            "+ %s %p (%s)\n",
            getTypeId(), this, getName().string());
    colorizer.reset(result);

    s.activeTransparentRegion.dump(result, "transparentRegion");
    visibleRegion.dump(result, "visibleRegion");
    surfaceDamageRegion.dump(result, "surfaceDamageRegion");
    sp<Client> client(mClientRef.promote());

    result.appendFormat(            "      "
            "layerStack=%4d, z=%9d, pos=(%g,%g), size=(%4d,%4d), "
            "crop=(%4d,%4d,%4d,%4d), finalCrop=(%4d,%4d,%4d,%4d), "
            "isOpaque=%1d, invalidate=%1d, "
#ifdef USE_HWC2
            "alpha=%.3f, flags=0x%08x, tr=[%.2f, %.2f][%.2f, %.2f]\n"
#else
            "alpha=0x%02x, flags=0x%08x, tr=[%.2f, %.2f][%.2f, %.2f]\n"
#endif
            "      client=%p\n",
            s.layerStack, s.z, s.active.transform.tx(), s.active.transform.ty(), s.active.w, s.active.h,
            s.crop.left, s.crop.top,
            s.crop.right, s.crop.bottom,
            s.finalCrop.left, s.finalCrop.top,
            s.finalCrop.right, s.finalCrop.bottom,
            isOpaque(s), contentDirty,
            s.alpha, s.flags,
            s.active.transform[0][0], s.active.transform[0][1],
            s.active.transform[1][0], s.active.transform[1][1],
            client.get());

    sp<const GraphicBuffer> buf0(mActiveBuffer);
    uint32_t w0=0, h0=0, s0=0, f0=0;
    if (buf0 != 0) {
        w0 = buf0->getWidth();
        h0 = buf0->getHeight();
        s0 = buf0->getStride();
        f0 = buf0->format;
    }
    result.appendFormat(
            "      "
            "format=%2d, activeBuffer=[%4ux%4u:%4u,%3X],"
            " queued-frames=%d, mRefreshPending=%d\n",
            mFormat, w0, h0, s0,f0,
            mQueuedFrames, mRefreshPending);

    if (mSurfaceFlingerConsumer != 0) {
        mSurfaceFlingerConsumer->dump(result, "            ");
    }
}

#ifdef USE_HWC2
void Layer::miniDumpHeader(String8& result) {
    result.append("----------------------------------------");
    result.append("---------------------------------------\n");
    result.append(" Layer name\n");
    result.append("           Z | ");
    result.append(" Comp Type | ");
    result.append("  Disp Frame (LTRB) | ");
    result.append("         Source Crop (LTRB)\n");
    result.append("----------------------------------------");
    result.append("---------------------------------------\n");
}

void Layer::miniDump(String8& result, int32_t hwcId) const {
    if (mHwcLayers.count(hwcId) == 0) {
        return;
    }

    String8 name;
    if (mName.length() > 77) {
        std::string shortened;
        shortened.append(mName.string(), 36);
        shortened.append("[...]");
        shortened.append(mName.string() + (mName.length() - 36), 36);
        name = shortened.c_str();
    } else {
        name = mName;
    }

    result.appendFormat(" %s\n", name.string());

    const Layer::State& layerState(getDrawingState());
    const HWCInfo& hwcInfo = mHwcLayers.at(hwcId);
    result.appendFormat("  %10u | ", layerState.z);
    result.appendFormat("%10s | ",
            to_string(getCompositionType(hwcId)).c_str());
    const Rect& frame = hwcInfo.displayFrame;
    result.appendFormat("%4d %4d %4d %4d | ", frame.left, frame.top,
            frame.right, frame.bottom);
    const FloatRect& crop = hwcInfo.sourceCrop;
    result.appendFormat("%6.1f %6.1f %6.1f %6.1f\n", crop.left, crop.top,
            crop.right, crop.bottom);

    result.append("- - - - - - - - - - - - - - - - - - - - ");
    result.append("- - - - - - - - - - - - - - - - - - - -\n");
}
#endif

void Layer::dumpFrameStats(String8& result) const {
    mFrameTracker.dumpStats(result);
}

void Layer::clearFrameStats() {
    mFrameTracker.clearStats();
}

void Layer::logFrameStats() {
    mFrameTracker.logAndResetStats(mName);
}

void Layer::getFrameStats(FrameStats* outStats) const {
    mFrameTracker.getStats(outStats);
}

void Layer::getFenceData(String8* outName, uint64_t* outFrameNumber,
        bool* outIsGlesComposition, nsecs_t* outPostedTime,
        sp<Fence>* outAcquireFence, sp<Fence>* outPrevReleaseFence) const {
    *outName = mName;
    *outFrameNumber = mSurfaceFlingerConsumer->getFrameNumber();

#ifdef USE_HWC2
    *outIsGlesComposition = mHwcLayers.count(HWC_DISPLAY_PRIMARY) ?
            mHwcLayers.at(HWC_DISPLAY_PRIMARY).compositionType ==
            HWC2::Composition::Client : true;
#else
    *outIsGlesComposition = mIsGlesComposition;
#endif
    *outPostedTime = mSurfaceFlingerConsumer->getTimestamp();
    *outAcquireFence = mSurfaceFlingerConsumer->getCurrentFence();
    *outPrevReleaseFence = mSurfaceFlingerConsumer->getPrevReleaseFence();
}

std::vector<OccupancyTracker::Segment> Layer::getOccupancyHistory(
        bool forceFlush) {
    std::vector<OccupancyTracker::Segment> history;
    status_t result = mSurfaceFlingerConsumer->getOccupancyHistory(forceFlush,
            &history);
    if (result != NO_ERROR) {
        ALOGW("[%s] Failed to obtain occupancy history (%d)", mName.string(),
                result);
        return {};
    }
    return history;
}

bool Layer::getTransformToDisplayInverse() const {
    return mSurfaceFlingerConsumer->getTransformToDisplayInverse();
}

// ---------------------------------------------------------------------------

Layer::LayerCleaner::LayerCleaner(const sp<SurfaceFlinger>& flinger,
        const sp<Layer>& layer)
    : mFlinger(flinger), mLayer(layer) {
}

Layer::LayerCleaner::~LayerCleaner() {
    // destroy client resources
    mFlinger->onLayerDestroyed(mLayer);
}

// ---------------------------------------------------------------------------
}; // namespace android

#if defined(__gl_h_)
#error "don't include gl/gl.h in this file"
#endif

#if defined(__gl2_h_)
#error "don't include gl2/gl2.h in this file"
#endif
